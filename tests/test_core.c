/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests for the portable core, run against the fake device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/bot.h"
#include "../core/scsi.h"
#include "../core/version.h"
#include "fake_device.h"

#define SECTORS 1000

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static df_u8 image[SECTORS * DF_SECTOR_SIZE];
static df_u8 buf[300 * DF_SECTOR_SIZE];

static void fill_image(void)
{
    df_u32 i;

    for (i = 0; i < sizeof(image); i++)
        image[i] = (df_u8)(i * 31 + (i >> 9));
}

static void cdb10(df_u8 *cdb, df_u8 op, df_u32 lba, unsigned count)
{
    memset(cdb, 0, 16);
    cdb[0] = op;
    cdb[2] = (df_u8)(lba >> 24);
    cdb[3] = (df_u8)(lba >> 16);
    cdb[4] = (df_u8)(lba >> 8);
    cdb[5] = (df_u8)lba;
    cdb[7] = (df_u8)(count >> 8);
    cdb[8] = (df_u8)count;
}

static int sense_is(const df_scsi *s, df_u8 key, df_u8 asc)
{
    df_u8 sense[DF_SENSE_LEN];

    df_scsi_sense(s, sense);
    return sense[0] == 0x70 && sense[2] == key && sense[12] == asc;
}

static void test_transport(void)
{
    fake_dev fake;
    df_io io;
    df_dev dev;
    char text[41];

    fill_image();
    fake_init(&fake, image, SECTORS, 1, "SERIAL-A");
    fake_io(&fake, &io);
    df_init(&dev, &io);

    CHECK(df_read(&dev, 0, 1, buf) == DF_ERR_NO_CARD);
    CHECK(df_identify(&dev) == DF_OK);
    CHECK(dev.slot == 1);
    CHECK(dev.sectors == SECTORS);
    df_ata_string(&dev, 10, 10, text);
    CHECK(strcmp(text, "SERIAL-A") == 0);
    df_ata_string(&dev, 27, 20, text);
    CHECK(strcmp(text, "FAKE ATA CARD") == 0);

    /* 300 sectors spans three vendor commands. */
    fake.commands = 0;
    CHECK(df_read(&dev, 7, 300, buf) == DF_OK);
    CHECK(fake.commands == 3);
    CHECK(memcmp(buf, image + 7 * DF_SECTOR_SIZE, 300 * DF_SECTOR_SIZE) == 0);

    CHECK(df_read(&dev, SECTORS - 1, 1, buf) == DF_OK);
    CHECK(df_read(&dev, SECTORS - 1, 2, buf) == DF_ERR_RANGE);
    CHECK(df_read(&dev, SECTORS, 1, buf) == DF_ERR_RANGE);

    memset(buf, 0xA5, 200 * DF_SECTOR_SIZE);
    CHECK(df_write(&dev, 500, 200, buf) == DF_OK);
    CHECK(image[500 * DF_SECTOR_SIZE] == 0xA5);
    CHECK(image[700 * DF_SECTOR_SIZE - 1] == 0xA5);
    CHECK(image[700 * DF_SECTOR_SIZE] != 0xA5);
    CHECK(image[500 * DF_SECTOR_SIZE - 1] != 0xA5);

    fake.fail_writes = 1;
    CHECK(df_write(&dev, 0, 1, buf) == DF_ERR_STATUS);
    fake.fail_writes = 0;

    fake.slot = -1;
    CHECK(df_read(&dev, 0, 1, buf) == DF_ERR_IO);
    CHECK(df_identify(&dev) == DF_ERR_NO_CARD);
    CHECK(dev.slot == -1);

    fake.slot = 0;
    CHECK(df_identify(&dev) == DF_OK);
    CHECK(dev.slot == 0);
}

static void test_scsi(void)
{
    fake_dev fake;
    df_io io;
    df_dev dev;
    df_scsi scsi;
    df_u8 cdb[16];
    df_u32 n;

    fill_image();
    fake_init(&fake, image, SECTORS, 1, "SERIAL-A");
    fake_io(&fake, &io);
    df_init(&dev, &io);
    df_scsi_init(&scsi, &dev, 0);

    /* INQUIRY and REPORT LUNS work before any medium check. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = 36;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 36 && buf[0] == 0x00 && buf[1] == 0x80);
    CHECK(memcmp(buf + 8, "TOSHIBA MEUSBPCA PC Card1.00", 28) == 0);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA0;
    cdb[9] = 16;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 16 && buf[3] == 8);

    /* First TEST UNIT READY reports the newly seen card once. */
    memset(cdb, 0, sizeof(cdb));
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x06, 0x28));
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 8);
    CHECK(buf[2] == ((SECTORS - 1) >> 8) && buf[3] == ((SECTORS - 1) & 0xFF));
    CHECK(buf[6] == 0x02 && buf[7] == 0x00);

    cdb10(cdb, 0x28, 10, 130);
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 130 * DF_SECTOR_SIZE);
    CHECK(memcmp(buf, image + 10 * DF_SECTOR_SIZE, n) == 0);

    /* READ(6) with count 0 means 256 sectors. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x08;
    cdb[3] = 3;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 256 * DF_SECTOR_SIZE);
    CHECK(memcmp(buf, image + 3 * DF_SECTOR_SIZE, n) == 0);

    memset(buf, 0x3C, 4 * DF_SECTOR_SIZE);
    cdb10(cdb, 0x2A, 900, 4);
    CHECK(df_scsi_exec(&scsi, cdb, buf, 4 * DF_SECTOR_SIZE, &n) == DF_SCSI_GOOD);
    CHECK(n == 0 && image[900 * DF_SECTOR_SIZE] == 0x3C);

    cdb10(cdb, 0x28, SECTORS, 1);
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x05, 0x21));

    /* REQUEST SENSE returns the sense and clears it. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x03;
    cdb[4] = 18;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 18 && buf[2] == 0x05 && buf[12] == 0x21);
    CHECK(sense_is(&scsi, 0x00, 0x00));

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xEE;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x05, 0x20));

    /* MODE SENSE(6), all pages: header plus caching page, not write-protected. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1A;
    cdb[2] = 0x3F;
    cdb[4] = 255;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 24 && buf[0] == 23 && buf[2] == 0x00 && buf[4] == 0x08);

    /* Card swapped: reported once, then the new card is used. */
    fake_set_serial(&fake, "SERIAL-B");
    memset(cdb, 0, sizeof(cdb));
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x06, 0x28));
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);

    /* Card removed: not ready, medium not present, from every media command. */
    fake.slot = -1;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x02, 0x3A));
    cdb10(cdb, 0x28, 0, 1);
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x02, 0x3A));

    /* Removal noticed in the middle of I/O, without a TEST UNIT READY first. */
    fake.slot = 1;
    memset(cdb, 0, sizeof(cdb));
    df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n);
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    fake.slot = -1;
    cdb10(cdb, 0x28, 0, 1);
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x02, 0x3A));

    /* Read-only mode refuses writes and reports write protection. */
    fake.slot = 1;
    df_scsi_init(&scsi, &dev, 1);
    memset(cdb, 0, sizeof(cdb));
    df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n);
    cdb10(cdb, 0x2A, 0, 1);
    CHECK(df_scsi_exec(&scsi, cdb, buf, DF_SECTOR_SIZE, &n) == DF_SCSI_CHECK_CONDITION);
    CHECK(sense_is(&scsi, 0x07, 0x27));
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x5A;
    cdb[2] = 0x3F;
    cdb[8] = 255;
    CHECK(df_scsi_exec(&scsi, cdb, buf, sizeof(buf), &n) == DF_SCSI_GOOD);
    CHECK(n == 28 && buf[3] == 0x80);
}

static void test_bot(void)
{
    static const df_u8 raw[DF_CBW_LEN] = {
        'U', 'S', 'B', 'C', 0x78, 0x56, 0x34, 0x12, 0x00, 0x02, 0x00, 0x00,
        0x80, 0x00, 0x0A, 0x28, 0, 0, 0, 0, 5, 0, 0, 1, 0
    };
    df_cbw cbw;
    df_u8 csw[DF_CSW_LEN];
    df_u8 bad[DF_CBW_LEN];

    CHECK(df_cbw_parse(raw, DF_CBW_LEN, &cbw) == 0);
    CHECK(cbw.tag == 0x12345678UL && cbw.data_len == 512 && cbw.data_in == 1);
    CHECK(cbw.lun == 0 && cbw.cdb_len == 10 && cbw.cdb[0] == 0x28 && cbw.cdb[5] == 5);

    memcpy(bad, raw, sizeof(bad));
    bad[0] = 'X';
    CHECK(df_cbw_parse(bad, DF_CBW_LEN, &cbw) != 0);
    CHECK(df_cbw_parse(raw, DF_CBW_LEN - 1, &cbw) != 0);

    df_csw_build(csw, 0x12345678UL, 512, DF_CSW_FAILED);
    CHECK(memcmp(csw, "USBS\x78\x56\x34\x12\x00\x02\x00\x00\x01", DF_CSW_LEN) == 0);
}

int main(void)
{
    fputs(MEUSBPCA_BANNER("Toshiba MEUSBPCA Core Tests"), stdout);

    test_transport();
    test_scsi();
    test_bot();
    if (failures) {
        printf("%d core test check(s) have failed.\n", failures);
        return 1;
    }
    printf("All core tests have passed.\n");
    return 0;
}
