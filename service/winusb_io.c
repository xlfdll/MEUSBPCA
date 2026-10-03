/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>

#include "winusb_io.h"

#include <setupapi.h>

/* GUID_DEVINTERFACE_USB_DEVICE: the hub exposes it for every USB device, so
 * the adapter is found whatever interface GUID its WinUSB INF registered. */
static const GUID usb_device_guid =
    { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

static int find_path(char *path, size_t path_len)
{
    char wanted[32];
    char buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)buffer;
    SP_DEVICE_INTERFACE_DATA iface;
    HDEVINFO set;
    DWORD index;
    int found = 0;

    sprintf(wanted, "vid_%04x&pid_%04x", MEUSBPCA_VID, MEUSBPCA_PID);
    set = SetupDiGetClassDevsA(&usb_device_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return 0;

    iface.cbSize = sizeof(iface);
    for (index = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &usb_device_guid, index, &iface); index++) {
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buffer), NULL, NULL))
            continue;
        CharLowerA(detail->DevicePath);
        if (strstr(detail->DevicePath, wanted) && strlen(detail->DevicePath) < path_len) {
            strcpy(path, detail->DevicePath);
            found = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void apply_timeout(wu_dev *dev)
{
    WinUsb_SetPipePolicy(dev->usb, dev->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof(dev->timeout_ms), &dev->timeout_ms);
    WinUsb_SetPipePolicy(dev->usb, dev->ep_out, PIPE_TRANSFER_TIMEOUT, sizeof(dev->timeout_ms), &dev->timeout_ms);
}

static DWORD connect_adapter(wu_dev *dev)
{
    char path[512];
    USB_INTERFACE_DESCRIPTOR iface;
    WINUSB_PIPE_INFORMATION pipe;
    DWORD error;
    UCHAR i;

    if (!find_path(path, sizeof(path)))
        return ERROR_NOT_FOUND;

    dev->file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (dev->file == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WinUsb_Initialize(dev->file, &dev->usb)) {
        error = GetLastError();
        wu_close(dev);
        return error;
    }

    if (WinUsb_QueryInterfaceSettings(dev->usb, 0, &iface)) {
        for (i = 0; i < iface.bNumEndpoints; i++) {
            if (!WinUsb_QueryPipe(dev->usb, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk)
                continue;
            if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId))
                dev->ep_in = pipe.PipeId;
            else
                dev->ep_out = pipe.PipeId;
        }
    }
    if (!dev->ep_in || !dev->ep_out) {
        wu_close(dev);
        return ERROR_BAD_DEVICE;
    }

    apply_timeout(dev);
    return 0;
}

static void reset(wu_dev *dev)
{
    dev->file = INVALID_HANDLE_VALUE;
    dev->usb = NULL;
    dev->ep_in = 0;
    dev->ep_out = 0;
    dev->timeout_ms = DF_IO_TIMEOUT_MS;
    dev->reopen = 0;
    dev->last_attempt = 0;
}

DWORD wu_open(wu_dev *dev)
{
    reset(dev);
    return connect_adapter(dev);
}

void wu_init_reopening(wu_dev *dev)
{
    reset(dev);
    dev->reopen = 1;
    dev->last_attempt = GetTickCount() - 1000;
}

static int ready(wu_dev *dev)
{
    DWORD now;

    if (dev->usb)
        return 1;
    if (!dev->reopen)
        return 0;
    now = GetTickCount();
    if (now - dev->last_attempt < 1000)
        return 0;
    dev->last_attempt = now;
    return connect_adapter(dev) == 0;
}

void wu_close(wu_dev *dev)
{
    if (dev->usb) {
        WinUsb_Free(dev->usb);
        dev->usb = NULL;
    }
    if (dev->file != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->file);
        dev->file = INVALID_HANDLE_VALUE;
    }
}

static int wu_bulk_out(void *ctx, const df_u8 *buf, df_u32 len)
{
    wu_dev *dev = (wu_dev *)ctx;
    ULONG done = 0;

    if (!ready(dev))
        return -1;
    if (!WinUsb_WritePipe(dev->usb, dev->ep_out, (PUCHAR)buf, len, &done, NULL))
        return -1;
    return done == len ? 0 : -1;
}

static int wu_bulk_in(void *ctx, df_u8 *buf, df_u32 len)
{
    wu_dev *dev = (wu_dev *)ctx;
    ULONG done = 0;

    if (!ready(dev))
        return -1;
    if (!WinUsb_ReadPipe(dev->usb, dev->ep_in, buf, len, &done, NULL))
        return -1;
    return done == len ? 0 : -1;
}

static void wu_recover(void *ctx)
{
    wu_dev *dev = (wu_dev *)ctx;

    if (!dev->usb)
        return;
    WinUsb_AbortPipe(dev->usb, dev->ep_in);
    WinUsb_AbortPipe(dev->usb, dev->ep_out);
    /* A reset that fails means the adapter itself is gone. */
    if ((!WinUsb_ResetPipe(dev->usb, dev->ep_in) || !WinUsb_ResetPipe(dev->usb, dev->ep_out)) && dev->reopen)
        wu_close(dev);
}

static void wu_set_timeout(void *ctx, unsigned ms)
{
    wu_dev *dev = (wu_dev *)ctx;

    dev->timeout_ms = ms;
    if (dev->usb)
        apply_timeout(dev);
}

static void wu_sleep(void *ctx, unsigned ms)
{
    (void)ctx;
    Sleep(ms);
}

void wu_io(wu_dev *dev, df_io *io)
{
    io->ctx = dev;
    io->bulk_out = wu_bulk_out;
    io->bulk_in = wu_bulk_in;
    io->recover = wu_recover;
    io->sleep_ms = wu_sleep;
    io->set_timeout = wu_set_timeout;
}
