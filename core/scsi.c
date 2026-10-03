/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scsi.h"

#define OP_TEST_UNIT_READY        0x00
#define OP_REQUEST_SENSE          0x03
#define OP_READ_6                 0x08
#define OP_WRITE_6                0x0A
#define OP_INQUIRY                0x12
#define OP_MODE_SENSE_6           0x1A
#define OP_START_STOP             0x1B
#define OP_PREVENT_ALLOW          0x1E
#define OP_READ_FORMAT_CAPACITIES 0x23
#define OP_READ_CAPACITY          0x25
#define OP_READ_10                0x28
#define OP_WRITE_10               0x2A
#define OP_VERIFY_10              0x2F
#define OP_SYNCHRONIZE_CACHE      0x35
#define OP_MODE_SENSE_10          0x5A
#define OP_REPORT_LUNS            0xA0
#define OP_READ_12                0xA8
#define OP_WRITE_12               0xAA

#define SK_NO_SENSE         0x00
#define SK_NOT_READY        0x02
#define SK_MEDIUM_ERROR     0x03
#define SK_ILLEGAL_REQUEST  0x05
#define SK_UNIT_ATTENTION   0x06
#define SK_DATA_PROTECT     0x07

static const char inquiry_vendor[]  = "TOSHIBA ";
static const char inquiry_product[] = "MEUSBPCA PC Card";
static const char inquiry_rev[]     = "1.00";

static df_u32 get_be16(const df_u8 *p)
{
    return ((df_u32)p[0] << 8) | p[1];
}

static df_u32 get_be32(const df_u8 *p)
{
    return ((df_u32)p[0] << 24) | ((df_u32)p[1] << 16) | ((df_u32)p[2] << 8) | p[3];
}

static void put_be32(df_u8 *p, df_u32 v)
{
    p[0] = (df_u8)(v >> 24);
    p[1] = (df_u8)(v >> 16);
    p[2] = (df_u8)(v >> 8);
    p[3] = (df_u8)v;
}

static void mem_set(df_u8 *p, df_u8 v, df_u32 n)
{
    while (n--)
        *p++ = v;
}

static void mem_copy(df_u8 *dst, const df_u8 *src, df_u32 n)
{
    while (n--)
        *dst++ = *src++;
}

static int str_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int check(df_scsi *s, df_u8 key, df_u8 asc, df_u8 ascq)
{
    s->sense_key = key;
    s->asc = asc;
    s->ascq = ascq;
    return DF_SCSI_CHECK_CONDITION;
}

/* Copy a response to the caller, limited by the CDB allocation length. */
static int data_in(const df_u8 *resp, df_u32 len, df_u32 alloc,
                   df_u8 *buf, df_u32 buf_len, df_u32 *in_len)
{
    if (len > alloc)
        len = alloc;
    if (len > buf_len)
        len = buf_len;
    mem_copy(buf, resp, len);
    *in_len = len;
    return DF_SCSI_GOOD;
}

void df_scsi_init(df_scsi *s, df_dev *dev, int read_only)
{
    s->dev = dev;
    s->present = 0;
    s->read_only = read_only;
    s->sense_key = SK_NO_SENSE;
    s->asc = 0;
    s->ascq = 0;
    s->serial[0] = '\0';
}

void df_scsi_sense(const df_scsi *s, df_u8 out[DF_SENSE_LEN])
{
    mem_set(out, 0, DF_SENSE_LEN);
    out[0] = 0x70;                  /* current error, fixed format */
    out[2] = s->sense_key;
    out[7] = DF_SENSE_LEN - 8;
    out[12] = s->asc;
    out[13] = s->ascq;
}

/*
 * Make sure a card is there. With probe set the card is re-identified even if
 * one was present, which is how removal and swaps are noticed.
 */
static int media_check(df_scsi *s, int probe)
{
    char serial[21];

    if (s->present && !probe)
        return DF_SCSI_GOOD;

    if (df_identify(s->dev) != DF_OK) {
        s->present = 0;
        return check(s, SK_NOT_READY, 0x3A, 0x00);      /* medium not present */
    }
    df_ata_string(s->dev, 10, 10, serial);
    if (s->present && str_equal(serial, s->serial))
        return DF_SCSI_GOOD;

    s->present = 1;
    mem_copy((df_u8 *)s->serial, (const df_u8 *)serial, sizeof(serial));
    return check(s, SK_UNIT_ATTENTION, 0x28, 0x00);     /* medium may have changed */
}

static int io_failed(df_scsi *s, int rc, int writing)
{
    if (rc == DF_ERR_RANGE)
        return check(s, SK_ILLEGAL_REQUEST, 0x21, 0x00);    /* LBA out of range */
    if (df_identify(s->dev) != DF_OK) {
        s->present = 0;
        return check(s, SK_NOT_READY, 0x3A, 0x00);
    }
    if (writing)
        return check(s, SK_MEDIUM_ERROR, 0x0C, 0x00);       /* write error */
    return check(s, SK_MEDIUM_ERROR, 0x11, 0x00);           /* unrecovered read error */
}

static int do_read(df_scsi *s, df_u32 lba, df_u32 count,
                   df_u8 *buf, df_u32 buf_len, df_u32 *in_len)
{
    int rc = media_check(s, 0);

    if (rc != DF_SCSI_GOOD)
        return rc;
    if (count > buf_len / DF_SECTOR_SIZE)
        return check(s, SK_ILLEGAL_REQUEST, 0x24, 0x00);
    rc = df_read(s->dev, lba, count, buf);
    if (rc != DF_OK)
        return io_failed(s, rc, 0);
    *in_len = count * DF_SECTOR_SIZE;
    return DF_SCSI_GOOD;
}

static int do_write(df_scsi *s, df_u32 lba, df_u32 count,
                    const df_u8 *buf, df_u32 buf_len)
{
    int rc = media_check(s, 0);

    if (rc != DF_SCSI_GOOD)
        return rc;
    if (s->read_only)
        return check(s, SK_DATA_PROTECT, 0x27, 0x00);       /* write protected */
    if (count > buf_len / DF_SECTOR_SIZE)
        return check(s, SK_ILLEGAL_REQUEST, 0x24, 0x00);
    rc = df_write(s->dev, lba, count, buf);
    if (rc != DF_OK)
        return io_failed(s, rc, 1);
    return DF_SCSI_GOOD;
}

static int do_inquiry(df_scsi *s, const df_u8 *cdb,
                      df_u8 *buf, df_u32 buf_len, df_u32 *in_len)
{
    df_u8 resp[36];
    df_u32 alloc = get_be16(cdb + 3);
    df_u32 i;

    if (cdb[1] & 0x01) {            /* vital product data */
        mem_set(resp, 0, sizeof(resp));
        resp[1] = cdb[2];
        switch (cdb[2]) {
        case 0x00:                  /* supported pages */
            resp[3] = 2;
            resp[4] = 0x00;
            resp[5] = 0x80;
            return data_in(resp, 6, alloc, buf, buf_len, in_len);
        case 0x80:                  /* unit serial number */
            resp[3] = 20;
            mem_set(resp + 4, ' ', 20);
            for (i = 0; i < 20 && s->present && s->serial[i]; i++)
                resp[4 + i] = (df_u8)s->serial[i];
            return data_in(resp, 24, alloc, buf, buf_len, in_len);
        default:
            return check(s, SK_ILLEGAL_REQUEST, 0x24, 0x00);
        }
    }
    if (cdb[2] != 0)
        return check(s, SK_ILLEGAL_REQUEST, 0x24, 0x00);

    mem_set(resp, 0, sizeof(resp));
    resp[0] = 0x00;                 /* direct-access device */
    resp[1] = 0x80;                 /* removable medium */
    resp[2] = 0x02;                 /* SCSI-2 */
    resp[3] = 0x02;
    resp[4] = sizeof(resp) - 5;
    mem_copy(resp + 8, (const df_u8 *)inquiry_vendor, 8);
    mem_copy(resp + 16, (const df_u8 *)inquiry_product, 16);
    mem_copy(resp + 32, (const df_u8 *)inquiry_rev, 4);
    return data_in(resp, sizeof(resp), alloc, buf, buf_len, in_len);
}

static int do_mode_sense(df_scsi *s, const df_u8 *cdb, int ten,
                         df_u8 *buf, df_u32 buf_len, df_u32 *in_len)
{
    df_u8 resp[8 + 20];
    df_u8 page = (df_u8)(cdb[2] & 0x3F);
    df_u32 hdr = ten ? 8 : 4;
    df_u32 len = hdr;
    df_u32 alloc = ten ? get_be16(cdb + 7) : cdb[4];
    df_u8 wp = (df_u8)(s->read_only ? 0x80 : 0x00);

    if (page != 0x08 && page != 0x3F)
        return check(s, SK_ILLEGAL_REQUEST, 0x24, 0x00);

    mem_set(resp, 0, sizeof(resp));
    resp[len] = 0x08;               /* caching page, write cache off */
    resp[len + 1] = 0x12;
    len += 20;

    if (ten) {
        resp[1] = (df_u8)(len - 2);
        resp[3] = wp;
    } else {
        resp[0] = (df_u8)(len - 1);
        resp[2] = wp;
    }
    return data_in(resp, len, alloc, buf, buf_len, in_len);
}

int df_scsi_exec(df_scsi *s, const df_u8 *cdb, df_u8 *buf, df_u32 buf_len, df_u32 *in_len)
{
    df_u8 resp[DF_SENSE_LEN];
    df_u32 count;
    int rc;

    *in_len = 0;

    if (cdb[0] == OP_REQUEST_SENSE) {
        df_scsi_sense(s, resp);
        check(s, SK_NO_SENSE, 0, 0);
        return data_in(resp, DF_SENSE_LEN, cdb[4], buf, buf_len, in_len);
    }
    check(s, SK_NO_SENSE, 0, 0);

    switch (cdb[0]) {
    case OP_TEST_UNIT_READY:
        return media_check(s, 1);

    case OP_INQUIRY:
        return do_inquiry(s, cdb, buf, buf_len, in_len);

    case OP_REPORT_LUNS:
        mem_set(resp, 0, 16);
        resp[3] = 8;                /* one LUN, number 0 */
        return data_in(resp, 16, get_be32(cdb + 6), buf, buf_len, in_len);

    case OP_READ_CAPACITY:
        rc = media_check(s, 0);
        if (rc != DF_SCSI_GOOD)
            return rc;
        put_be32(resp, s->dev->sectors - 1);
        put_be32(resp + 4, DF_SECTOR_SIZE);
        return data_in(resp, 8, 8, buf, buf_len, in_len);

    case OP_READ_FORMAT_CAPACITIES:
        mem_set(resp, 0, 12);
        resp[3] = 8;
        put_be32(resp + 4, s->present ? s->dev->sectors : 0xFFFFFFFFUL);
        put_be32(resp + 8, DF_SECTOR_SIZE);
        resp[8] = (df_u8)(s->present ? 0x02 : 0x03);    /* formatted / no medium */
        return data_in(resp, 12, get_be16(cdb + 7), buf, buf_len, in_len);

    case OP_READ_6:
        count = cdb[4] ? cdb[4] : 256;
        return do_read(s, ((df_u32)(cdb[1] & 0x1F) << 16) | get_be16(cdb + 2), count,
                       buf, buf_len, in_len);
    case OP_READ_10:
        return do_read(s, get_be32(cdb + 2), get_be16(cdb + 7), buf, buf_len, in_len);
    case OP_READ_12:
        return do_read(s, get_be32(cdb + 2), get_be32(cdb + 6), buf, buf_len, in_len);

    case OP_WRITE_6:
        count = cdb[4] ? cdb[4] : 256;
        return do_write(s, ((df_u32)(cdb[1] & 0x1F) << 16) | get_be16(cdb + 2), count,
                        buf, buf_len);
    case OP_WRITE_10:
        return do_write(s, get_be32(cdb + 2), get_be16(cdb + 7), buf, buf_len);
    case OP_WRITE_12:
        return do_write(s, get_be32(cdb + 2), get_be32(cdb + 6), buf, buf_len);

    case OP_MODE_SENSE_6:
        return do_mode_sense(s, cdb, 0, buf, buf_len, in_len);
    case OP_MODE_SENSE_10:
        return do_mode_sense(s, cdb, 1, buf, buf_len, in_len);

    case OP_VERIFY_10:
    case OP_SYNCHRONIZE_CACHE:
        return media_check(s, 0);

    case OP_START_STOP:
    case OP_PREVENT_ALLOW:
        return DF_SCSI_GOOD;

    default:
        return check(s, SK_ILLEGAL_REQUEST, 0x20, 0x00);    /* invalid opcode */
    }
}
