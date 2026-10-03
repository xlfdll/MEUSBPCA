/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "fake_device.h"

static void put_word(fake_dev *f, unsigned word, unsigned value)
{
    f->ident[word * 2] = (df_u8)value;
    f->ident[word * 2 + 1] = (df_u8)(value >> 8);
}

static void put_string(fake_dev *f, unsigned first_word, unsigned words, const char *s)
{
    unsigned i, len = words * 2;
    df_u8 *dst = f->ident + first_word * 2;
    size_t n = strlen(s);

    for (i = 0; i < len; i++)
        dst[i ^ 1] = (df_u8)(i < n ? s[i] : ' ');
}

void fake_set_serial(fake_dev *f, const char *serial)
{
    put_string(f, 10, 10, serial);
}

void fake_init(fake_dev *f, df_u8 *image, df_u32 sectors, int slot, const char *serial)
{
    memset(f, 0, sizeof(*f));
    f->image = image;
    f->sectors = sectors;
    f->slot = slot;

    put_word(f, 1, (unsigned)(sectors / (16 * 63)));
    put_word(f, 3, 16);
    put_word(f, 6, 63);
    put_word(f, 49, 0x0200);
    put_word(f, 57, (unsigned)(sectors & 0xFFFF));
    put_word(f, 58, (unsigned)(sectors >> 16));
    put_word(f, 60, (unsigned)(sectors & 0xFFFF));
    put_word(f, 61, (unsigned)(sectors >> 16));
    put_string(f, 23, 4, "FAKE1.0");
    put_string(f, 27, 20, "FAKE ATA CARD");
    fake_set_serial(f, serial);
}

static int fake_bulk_out(void *ctx, const df_u8 *buf, df_u32 len)
{
    fake_dev *f = (fake_dev *)ctx;
    df_u32 lba, count;

    if (f->write_count) {
        if (len != f->write_count * DF_SECTOR_SIZE)
            return -1;
        if (f->fail_writes) {
            f->status[0] = 0x51;
            f->status[1] = 0x04;
        } else {
            memcpy(f->image + f->write_lba * DF_SECTOR_SIZE, buf, len);
            f->status[0] = 0x50;
            f->status[1] = 0x00;
        }
        f->write_count = 0;
        f->in_data = f->status;
        f->in_len = 2;
        return 0;
    }

    if (len != 8)
        return -1;
    f->commands++;
    f->in_len = 0;
    if (f->slot < 0 || ((buf[5] >> 4) & 1) != f->slot)
        return 0;                   /* command accepted, nothing ever answers */

    count = buf[1];
    lba = buf[2] | ((df_u32)buf[3] << 8) | ((df_u32)buf[4] << 16) | ((df_u32)(buf[5] & 0x0F) << 24);
    switch (buf[6]) {
    case 0xEC:
        f->in_data = f->ident;
        f->in_len = DF_SECTOR_SIZE;
        break;
    case 0x20:
        if (lba + count > f->sectors)
            return 0;
        f->in_data = f->image + lba * DF_SECTOR_SIZE;
        f->in_len = count * DF_SECTOR_SIZE;
        break;
    case 0x30:
        if (lba + count > f->sectors)
            return 0;
        f->write_lba = lba;
        f->write_count = count;
        break;
    default:
        break;
    }
    return 0;
}

static int fake_bulk_in(void *ctx, df_u8 *buf, df_u32 len)
{
    fake_dev *f = (fake_dev *)ctx;

    if (f->in_len == 0 || f->in_len != len)
        return -1;
    memcpy(buf, f->in_data, len);
    f->in_len = 0;
    return 0;
}

static void fake_recover(void *ctx)
{
    fake_dev *f = (fake_dev *)ctx;

    f->in_len = 0;
    f->write_count = 0;
}

void fake_io(fake_dev *f, df_io *io)
{
    io->ctx = f;
    io->bulk_out = fake_bulk_out;
    io->bulk_in = fake_bulk_in;
    io->recover = fake_recover;
    io->sleep_ms = 0;
    io->set_timeout = 0;
}
