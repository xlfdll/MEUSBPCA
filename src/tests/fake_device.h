/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-memory stand-in for the DataFab adapter, speaking its bulk protocol. */
#ifndef MEUSBPCA_FAKE_DEVICE_H
#define MEUSBPCA_FAKE_DEVICE_H

#include "../core/datafab.h"

typedef struct fake_dev {
    df_u8       *image;
    df_u32       sectors;
    int          slot;              /* slot holding the card, -1 for no card */
    df_u8        ident[DF_SECTOR_SIZE];
    df_u8        status[2];
    const df_u8 *in_data;           /* queued for the next bulk IN */
    df_u32       in_len;
    df_u32       write_lba;         /* pending write data phase */
    df_u32       write_count;
    int          fail_writes;       /* report an ATA error after write data */
    unsigned     commands;          /* vendor commands seen */
} fake_dev;

/* image must hold sectors * 512 bytes and outlive the device. */
void fake_init(fake_dev *f, df_u8 *image, df_u32 sectors, int slot, const char *serial);
void fake_set_serial(fake_dev *f, const char *serial);
void fake_io(fake_dev *f, df_io *io);

#endif
