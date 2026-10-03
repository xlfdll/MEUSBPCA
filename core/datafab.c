/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "datafab.h"

#define ATA_READ_SECTORS   0x20
#define ATA_WRITE_SECTORS  0x30
#define ATA_IDENTIFY       0xEC

#define DF_DATA_IN         0x01
#define DF_DATA_OUT        0x02

void df_init(df_dev *dev, const df_io *io)
{
    unsigned i;

    dev->io = *io;
    dev->slot = -1;
    dev->sectors = 0;
    for (i = 0; i < DF_SECTOR_SIZE; i++)
        dev->ident[i] = 0;
}

static void df_recover(df_dev *dev)
{
    if (dev->io.recover)
        dev->io.recover(dev->io.ctx);
}

static int df_command(df_dev *dev, df_u8 count, df_u32 lba, df_u8 device,
                      df_u8 ata_cmd, df_u8 phase)
{
    df_u8 cmd[8];

    cmd[0] = 0;
    cmd[1] = count;
    cmd[2] = (df_u8)(lba & 0xFF);
    cmd[3] = (df_u8)((lba >> 8) & 0xFF);
    cmd[4] = (df_u8)((lba >> 16) & 0xFF);
    cmd[5] = (df_u8)(device | ((lba >> 24) & 0x0F));
    cmd[6] = ata_cmd;
    cmd[7] = phase;
    return dev->io.bulk_out(dev->io.ctx, cmd, 8);
}

df_u16 df_ident_word(const df_dev *dev, unsigned word)
{
    return (df_u16)(dev->ident[word * 2] | (dev->ident[word * 2 + 1] << 8));
}

static df_u32 df_capacity(const df_dev *dev)
{
    df_u32 n;

    if (df_ident_word(dev, 49) & 0x0200) {      /* LBA supported */
        n = df_ident_word(dev, 60) | ((df_u32)df_ident_word(dev, 61) << 16);
        if (n)
            return n;
    }
    n = df_ident_word(dev, 57) | ((df_u32)df_ident_word(dev, 58) << 16);
    if (n)
        return n;
    return (df_u32)df_ident_word(dev, 1) * df_ident_word(dev, 3) * df_ident_word(dev, 6);
}

static int df_identify_slot(df_dev *dev, int slot)
{
    df_u8 device = (df_u8)(0xA0 | (slot << 4));

    if (df_command(dev, 1, 0, device, ATA_IDENTIFY, DF_DATA_IN) == 0 &&
        dev->io.bulk_in(dev->io.ctx, dev->ident, DF_SECTOR_SIZE) == 0) {
        dev->slot = slot;
        dev->sectors = df_capacity(dev);
        return DF_OK;
    }
    df_recover(dev);
    return DF_ERR_IO;
}

static void df_timeout(df_dev *dev, unsigned ms)
{
    if (dev->io.set_timeout)
        dev->io.set_timeout(dev->io.ctx, ms);
}

static int df_probe(df_dev *dev)
{
    int round, slot;

    /* The slot that answered last time is almost always still the one. */
    if (dev->slot >= 0 && df_identify_slot(dev, dev->slot) == DF_OK)
        return DF_OK;

    for (round = 0; round < 3; round++) {
        for (slot = 0; slot < 2; slot++) {
            if (df_identify_slot(dev, slot) == DF_OK)
                return DF_OK;
        }
        if (dev->io.sleep_ms)
            dev->io.sleep_ms(dev->io.ctx, 20);
    }
    dev->slot = -1;
    dev->sectors = 0;
    return DF_ERR_NO_CARD;
}

int df_identify(df_dev *dev)
{
    int rc;

    df_timeout(dev, DF_PROBE_TIMEOUT_MS);
    rc = df_probe(dev);
    df_timeout(dev, DF_IO_TIMEOUT_MS);
    return rc;
}

int df_read(df_dev *dev, df_u32 lba, df_u32 count, df_u8 *buf)
{
    df_u8 device;
    df_u32 n;

    if (dev->slot < 0)
        return DF_ERR_NO_CARD;
    if (lba > dev->sectors || count > dev->sectors - lba)
        return DF_ERR_RANGE;
    device = (df_u8)(0xE0 | (dev->slot << 4));

    while (count) {
        n = count > DF_MAX_SECTORS ? DF_MAX_SECTORS : count;
        if (df_command(dev, (df_u8)n, lba, device, ATA_READ_SECTORS, DF_DATA_IN) != 0 ||
            dev->io.bulk_in(dev->io.ctx, buf, n * DF_SECTOR_SIZE) != 0) {
            df_recover(dev);
            return DF_ERR_IO;
        }
        lba += n;
        count -= n;
        buf += n * DF_SECTOR_SIZE;
    }
    return DF_OK;
}

int df_write(df_dev *dev, df_u32 lba, df_u32 count, const df_u8 *buf)
{
    df_u8 device;
    df_u8 status[2];
    df_u32 n;

    if (dev->slot < 0)
        return DF_ERR_NO_CARD;
    if (lba > dev->sectors || count > dev->sectors - lba)
        return DF_ERR_RANGE;
    device = (df_u8)(0xE0 | (dev->slot << 4));

    while (count) {
        n = count > DF_MAX_SECTORS ? DF_MAX_SECTORS : count;
        if (df_command(dev, (df_u8)n, lba, device, ATA_WRITE_SECTORS, DF_DATA_OUT) != 0 ||
            dev->io.bulk_out(dev->io.ctx, buf, n * DF_SECTOR_SIZE) != 0 ||
            dev->io.bulk_in(dev->io.ctx, status, 2) != 0) {
            df_recover(dev);
            return DF_ERR_IO;
        }
        /* ATA status DRDY|DSC with a clear error register. */
        if (status[0] != 0x50 || status[1] != 0x00)
            return DF_ERR_STATUS;
        lba += n;
        count -= n;
        buf += n * DF_SECTOR_SIZE;
    }
    return DF_OK;
}

void df_ata_string(const df_dev *dev, unsigned first_word, unsigned words, char *out)
{
    unsigned i, len = words * 2;
    const df_u8 *src = dev->ident + first_word * 2;

    for (i = 0; i < len; i += 2) {
        out[i] = (char)src[i + 1];
        out[i + 1] = (char)src[i];
    }
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\0'))
        len--;
    out[len] = '\0';
    for (i = 0; i < len; i++) {
        if (out[i] < 0x20 || out[i] > 0x7E)
            out[i] = '?';
    }
}
