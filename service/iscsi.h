/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal iSCSI (RFC 3720) target: one LUN, one connection, loopback only. */
#ifndef MEUSBPCA_ISCSI_H
#define MEUSBPCA_ISCSI_H

#include <windows.h>

#include "../core/scsi.h"

#define ISCSI_DEFAULT_PORT  3260
#define ISCSI_TARGET_NAME   "iqn.2026-10.local.meusbpca:pccard"

typedef struct iscsi_config {
    unsigned short port;
    df_scsi       *scsi;
    volatile LONG *stop;        /* set non-zero to make iscsi_serve return */
    HANDLE         listening;   /* optional event, set once the port is open */
    int            verbose;     /* log every SCSI command to stderr */
} iscsi_config;

/* Serve until *stop is set. Returns 0, or a Winsock error if the port could
 * not be opened. */
int iscsi_serve(const iscsi_config *cfg);

#endif
