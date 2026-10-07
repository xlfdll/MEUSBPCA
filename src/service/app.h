/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pieces shared between the console entry point and the Windows service. */
#ifndef MEUSBPCA_APP_H
#define MEUSBPCA_APP_H

#include <windows.h>

#define SERVICE_NAME "meusbpca"

typedef struct app_options {
    unsigned short port;
    const char    *image;       /* serve a disk image instead of the adapter */
    int            writable;
    int            verbose;
} app_options;

/* Run the iSCSI target until *stop is set. listening (optional) is signalled
 * once the port is open. Returns a process exit code. */
int app_serve(const app_options *opt, volatile LONG *stop, HANDLE listening);

/* initiator.c: drive the Microsoft iSCSI initiator. Need administrator rights.
 * Return 0 on success. */
int initiator_connect(unsigned short port);
int initiator_disconnect(unsigned short port);

/* service.c */
int service_install(const app_options *opt);
int service_uninstall(void);
int service_dispatch(const app_options *opt);

#endif
