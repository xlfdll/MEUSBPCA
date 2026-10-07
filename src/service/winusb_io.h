/* SPDX-License-Identifier: GPL-2.0-or-later */
/* WinUSB access to the adapter, exposed as a df_io for the portable core. */
#ifndef MEUSBPCA_WINUSB_IO_H
#define MEUSBPCA_WINUSB_IO_H

#include <windows.h>
#include <winusb.h>

#include "../core/datafab.h"

#define MEUSBPCA_VID 0x07C4
#define MEUSBPCA_PID 0xA006

typedef struct wu_dev {
    HANDLE                  file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR                   ep_in;
    UCHAR                   ep_out;
    ULONG                   timeout_ms;
    int                     reopen;         /* reconnect on demand (service) */
    DWORD                   last_attempt;   /* tick of the last reopen try */
} wu_dev;

/* Returns 0 on success; otherwise a Win32 error code (ERROR_NOT_FOUND if the
 * adapter is not connected, or not bound to WinUSB). */
DWORD wu_open(wu_dev *dev);

/* Start closed and connect whenever a transfer is attempted, at most once a
 * second; the adapter may come and go while the caller keeps running. */
void  wu_init_reopening(wu_dev *dev);

void  wu_close(wu_dev *dev);
void  wu_io(wu_dev *dev, df_io *io);

#endif
