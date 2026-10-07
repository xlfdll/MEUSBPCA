/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DataFab DF-USB01 vendor protocol (Toshiba MEUSBPCA USB to PC Card Adapter).
 *
 * Portable C89, no OS headers: shared by the WDM filter driver, the iSCSI
 * service, the command-line tool and the host tests.
 *
 * Protocol after the Linux usb-storage DataFab subdriver
 * (drivers/usb/storage/datafab.c, (c) 2000 Jimmie Mayfield, (c) 2002 Alan Stern).
 */
#ifndef MEUSBPCA_DATAFAB_H
#define MEUSBPCA_DATAFAB_H

typedef unsigned char  df_u8;
typedef unsigned short df_u16;
typedef unsigned long  df_u32;

#define DF_SECTOR_SIZE   512
#define DF_MAX_SECTORS   128    /* per vendor command: 64 KiB, as Linux does */

#define DF_OK            0
#define DF_PROBE_TIMEOUT_MS  1000   /* IDENTIFY: an empty slot must fail fast */
#define DF_IO_TIMEOUT_MS     10000

#define DF_ERR_IO        (-1)   /* transfer failed or short */
#define DF_ERR_NO_CARD   (-2)   /* no slot answered IDENTIFY */
#define DF_ERR_STATUS    (-3)   /* card reported a write error */
#define DF_ERR_RANGE     (-4)   /* LBA beyond the card */

/* Bulk pipe access supplied by the host environment. All return 0 on success. */
typedef struct df_io {
    void *ctx;
    int  (*bulk_out)(void *ctx, const df_u8 *buf, df_u32 len);
    /* Must fail unless exactly len bytes arrive. */
    int  (*bulk_in)(void *ctx, df_u8 *buf, df_u32 len);
    /* Clear stalls / abort after a failed transfer. May be NULL. */
    void (*recover)(void *ctx);
    /* May be NULL. */
    void (*sleep_ms)(void *ctx, unsigned ms);
    /* Transfer timeout for the calls that follow. May be NULL. */
    void (*set_timeout)(void *ctx, unsigned ms);
} df_io;

typedef struct df_dev {
    df_io  io;
    int    slot;                        /* 0 or 1 once identified, -1 before */
    df_u32 sectors;
    df_u8  ident[DF_SECTOR_SIZE];       /* raw ATA IDENTIFY DEVICE block */
} df_dev;

void df_init(df_dev *dev, const df_io *io);

/* Probe both slots; on success fills slot, sectors and ident. */
int df_identify(df_dev *dev);

/* Any sector count; split into DF_MAX_SECTORS commands internally. */
int df_read(df_dev *dev, df_u32 lba, df_u32 count, df_u8 *buf);
int df_write(df_dev *dev, df_u32 lba, df_u32 count, const df_u8 *buf);

/* Copy an ATA string (byte-swapped words) out of ident, trimmed, NUL-terminated.
 * out must hold words * 2 + 1 bytes. */
void df_ata_string(const df_dev *dev, unsigned first_word, unsigned words, char *out);

df_u16 df_ident_word(const df_dev *dev, unsigned word);

#endif
