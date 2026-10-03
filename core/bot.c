/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bot.h"

static df_u32 get_le32(const df_u8 *p)
{
    return p[0] | ((df_u32)p[1] << 8) | ((df_u32)p[2] << 16) | ((df_u32)p[3] << 24);
}

static void put_le32(df_u8 *p, df_u32 v)
{
    p[0] = (df_u8)v;
    p[1] = (df_u8)(v >> 8);
    p[2] = (df_u8)(v >> 16);
    p[3] = (df_u8)(v >> 24);
}

int df_cbw_parse(const df_u8 *raw, df_u32 len, df_cbw *cbw)
{
    unsigned i;

    if (len != DF_CBW_LEN || get_le32(raw) != 0x43425355UL)     /* "USBC" */
        return -1;
    cbw->tag = get_le32(raw + 4);
    cbw->data_len = get_le32(raw + 8);
    cbw->data_in = (raw[12] & 0x80) != 0;
    cbw->lun = (df_u8)(raw[13] & 0x0F);
    cbw->cdb_len = (df_u8)(raw[14] & 0x1F);
    if (cbw->cdb_len < 1 || cbw->cdb_len > 16)
        return -1;
    for (i = 0; i < 16; i++)
        cbw->cdb[i] = (df_u8)(i < cbw->cdb_len ? raw[15 + i] : 0);
    return 0;
}

void df_csw_build(df_u8 out[DF_CSW_LEN], df_u32 tag, df_u32 residue, df_u8 status)
{
    put_le32(out, 0x53425355UL);                                /* "USBS" */
    put_le32(out + 4, tag);
    put_le32(out + 8, residue);
    out[12] = status;
}
