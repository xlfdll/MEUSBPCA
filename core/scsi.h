/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SCSI direct-access emulation on top of the DataFab transport. */
#ifndef MEUSBPCA_SCSI_H
#define MEUSBPCA_SCSI_H

#include "datafab.h"

#define DF_SCSI_GOOD             0x00
#define DF_SCSI_CHECK_CONDITION  0x02

#define DF_SENSE_LEN             18

typedef struct df_scsi {
    df_dev *dev;
    int     present;        /* a card answered the last probe */
    int     read_only;      /* refuse writes, report write-protect */
    df_u8   sense_key;
    df_u8   asc;
    df_u8   ascq;
    char    serial[21];     /* card seen at the last probe, to spot a swap */
} df_scsi;

void df_scsi_init(df_scsi *s, df_dev *dev, int read_only);

/*
 * Execute one CDB.
 *
 * buf/buf_len: data-out payload for write commands, otherwise room for data-in.
 * *in_len:     bytes of data-in produced (0 for commands without data-in).
 * Returns DF_SCSI_GOOD or DF_SCSI_CHECK_CONDITION; after the latter the sense
 * data stays available through df_scsi_sense() until the next command.
 */
int df_scsi_exec(df_scsi *s, const df_u8 *cdb, df_u8 *buf, df_u32 buf_len, df_u32 *in_len);

/* Fixed-format sense data for the last CHECK CONDITION. */
void df_scsi_sense(const df_scsi *s, df_u8 out[DF_SENSE_LEN]);

#endif
