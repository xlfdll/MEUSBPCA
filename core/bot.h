/* SPDX-License-Identifier: GPL-2.0-or-later */
/* USB Mass Storage Bulk-Only Transport framing (device side). */
#ifndef MEUSBPCA_BOT_H
#define MEUSBPCA_BOT_H

#include "datafab.h"

#define DF_CBW_LEN  31
#define DF_CSW_LEN  13

#define DF_CSW_PASSED       0
#define DF_CSW_FAILED       1
#define DF_CSW_PHASE_ERROR  2

typedef struct df_cbw {
    df_u32 tag;
    df_u32 data_len;    /* dCBWDataTransferLength */
    int    data_in;     /* host expects data from the device */
    df_u8  lun;
    df_u8  cdb_len;
    df_u8  cdb[16];
} df_cbw;

/* Returns 0 if raw holds a valid command block wrapper. */
int df_cbw_parse(const df_u8 *raw, df_u32 len, df_cbw *cbw);

void df_csw_build(df_u8 out[DF_CSW_LEN], df_u32 tag, df_u32 residue, df_u8 status);

#endif
