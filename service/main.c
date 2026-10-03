/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meusbpca-svc: present the card in the Toshiba MEUSBPCA adapter as a disk,
 * through a loopback iSCSI target and the built-in Microsoft initiator.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "iscsi.h"
#include "../core/version.h"
#include "winusb_io.h"
#include "../tests/fake_device.h"

static volatile LONG console_stop;

static void usage(void)
{
    fputs("Usage:\n"
          "  meusbpca-svc run [options]          Serve in this console until Ctrl+C is pressed.\n"
          "  meusbpca-svc connect [--port N]     Connect the Windows iSCSI initiator (administrator).\n"
          "  meusbpca-svc disconnect [--port N]  Disconnect the Windows iSCSI initiator (administrator).\n"
          "  meusbpca-svc install [options]      Install and start the Windows service (administrator).\n"
          "  meusbpca-svc uninstall              Stop and uninstall the Windows service (administrator).\n"
          "Options:\n"
          "  --port N       Use TCP port N on 127.0.0.1 (the default is 3260).\n"
          "  --writable     Allow writes to the card (the default is read-only).\n"
          "  --image FILE   Serve a disk image instead of the adapter (run only; writes stay in memory).\n"
          "  --verbose      Log every SCSI command (run only).\n", stderr);
}

static df_u8 *load_image(const char *path, df_u32 *sectors)
{
    FILE *in = fopen(path, "rb");
    df_u8 *image = NULL;
    __int64 size;

    if (!in) {
        fprintf(stderr, "The image file %s could not be opened: %s.\n", path, strerror(errno));
        return NULL;
    }
    _fseeki64(in, 0, SEEK_END);
    size = _ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);
    if (size < DF_SECTOR_SIZE || size % DF_SECTOR_SIZE || size > 0x7FFFFFFF) {
        fprintf(stderr, "The image file %s is %I64d bytes, which is not a usable disk image. "
                "It must be a multiple of 512 bytes and smaller than 2 GB.\n", path, size);
    } else {
        image = (df_u8 *)malloc((size_t)size);
        if (image && fread(image, 1, (size_t)size, in) != (size_t)size) {
            fprintf(stderr, "The image file %s could not be read: %s.\n", path, strerror(errno));
            free(image);
            image = NULL;
        }
        *sectors = (df_u32)(size / DF_SECTOR_SIZE);
    }
    fclose(in);
    return image;
}

int app_serve(const app_options *opt, volatile LONG *stop, HANDLE listening)
{
    static wu_dev usb;
    static fake_dev fake;
    static df_dev dev;
    static df_scsi scsi;
    iscsi_config cfg;
    df_io io;
    df_u8 *image = NULL;
    df_u32 sectors = 0;
    int error;

    if (opt->image) {
        image = load_image(opt->image, &sectors);
        if (!image)
            return 1;
        fake_init(&fake, image, sectors, 1, "DISK IMAGE");
        fake_io(&fake, &io);
    } else {
        wu_init_reopening(&usb);
        wu_io(&usb, &io);
    }
    df_init(&dev, &io);
    df_scsi_init(&scsi, &dev, !opt->writable);

    cfg.port = opt->port;
    cfg.scsi = &scsi;
    cfg.stop = stop;
    cfg.listening = listening;
    cfg.verbose = opt->verbose;
    error = iscsi_serve(&cfg);
    if (error)
        fprintf(stderr, "The iSCSI target could not listen on 127.0.0.1:%u (Winsock error %d).\n",
                opt->port, error);

    if (!opt->image)
        wu_close(&usb);
    free(image);
    return error ? 1 : 0;
}

static BOOL WINAPI on_console_event(DWORD type)
{
    (void)type;
    InterlockedExchange(&console_stop, 1);
    return TRUE;
}

int main(int argc, char **argv)
{
    app_options opt;
    const char *command;
    int i, rc;

    /* Started by the service manager there is no console to print to. */
    if (argc < 2 || strcmp(argv[1], "service") != 0) {
        fputs(MEUSBPCA_BANNER("Toshiba MEUSBPCA PC Card Drive Service"), stdout);
        fflush(stdout);
    }

    opt.port = ISCSI_DEFAULT_PORT;
    opt.image = NULL;
    opt.writable = 0;
    opt.verbose = 0;

    if (argc < 2) {
        usage();
        return 2;
    }
    command = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opt.port = (unsigned short)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            opt.image = argv[++i];
        } else if (strcmp(argv[i], "--writable") == 0) {
            opt.writable = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opt.verbose = 1;
        } else {
            usage();
            return 2;
        }
    }

    if (strcmp(command, "run") == 0) {
        SetConsoleCtrlHandler(on_console_event, TRUE);
        fprintf(stderr, "The iSCSI target %s is starting to serve %s on 127.0.0.1:%u in %s mode. "
                "Press Ctrl+C to stop.\n", ISCSI_TARGET_NAME,
                opt.image ? opt.image : "the MEUSBPCA adapter", opt.port,
                opt.writable ? "read-write" : "read-only");
        rc = app_serve(&opt, &console_stop, NULL);
        if (rc == 0)
            fputs("The iSCSI target has been stopped.\n", stderr);
        return rc;
    }
    if (strcmp(command, "connect") == 0) {
        if (initiator_connect(opt.port) != 0)
            return 1;
        puts("The Windows iSCSI initiator has been connected to the MEUSBPCA target.");
        return 0;
    }
    if (strcmp(command, "disconnect") == 0) {
        if (initiator_disconnect(opt.port) != 0)
            return 1;
        puts("The Windows iSCSI initiator has been disconnected from the MEUSBPCA target.");
        return 0;
    }
    if (strcmp(command, "install") == 0)
        return service_install(&opt);
    if (strcmp(command, "uninstall") == 0)
        return service_uninstall();
    if (strcmp(command, "service") == 0)
        return service_dispatch(&opt);
    usage();
    return 2;
}
