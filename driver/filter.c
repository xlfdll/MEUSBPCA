/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meusbpca.sys, spike build: a pass-through WDM lower filter under usbstor.sys
 * that only logs the URBs usbstor sends. It answers three questions on each
 * target OS before the real translator is written:
 *
 *   1. does the lower filter load (in particular on Windows 98SE / ME)?
 *   2. does usbstor accept the adapter when bound by VID/PID?
 *   3. what do usbstor's URBs look like (buffer or MDL, flags, lengths)?
 *
 * Output goes to the kernel debugger; DebugView shows it on both 9x and NT.
 * usbstor is expected to fail to bring up a disk: the adapter does not speak
 * Bulk-Only Transport, and nothing here translates yet.
 *
 * Restricted to what Windows 98SE's WDM exports: no remove locks.
 *
 * NOT YET COMPILED: written without a DDK at hand.
 */
#include <wdm.h>
#include <usbdi.h>
#include <usbioctl.h>

#include "../core/version.h"

typedef struct _FILTER_EXT {
    PDEVICE_OBJECT Lower;
} FILTER_EXT, *PFILTER_EXT;

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath);

static NTSTATUS FilterPass(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS FilterPower(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->Lower, Irp);
}

static NTSTATUS FilterPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = stack->MinorFunction;
    NTSTATUS status;

    DbgPrint("meusbpca: pnp minor %02x\n", minor);
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(ext->Lower, Irp);
    if (minor == IRP_MN_REMOVE_DEVICE) {
        IoDetachDevice(ext->Lower);
        IoDeleteDevice(Device);
    }
    return status;
}

static void LogUrb(const char *when, PURB urb)
{
    USHORT function = urb->UrbHeader.Function;

    switch (function) {
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        DbgPrint("meusbpca: %s bulk pipe %p flags %lx len %lu buf %p mdl %p status %lx\n", when,
                 urb->UrbBulkOrInterruptTransfer.PipeHandle,
                 urb->UrbBulkOrInterruptTransfer.TransferFlags,
                 urb->UrbBulkOrInterruptTransfer.TransferBufferLength,
                 urb->UrbBulkOrInterruptTransfer.TransferBuffer,
                 urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,
                 urb->UrbHeader.Status);
        break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        DbgPrint("meusbpca: %s get descriptor type %u index %u len %lu status %lx\n", when,
                 urb->UrbControlDescriptorRequest.DescriptorType,
                 urb->UrbControlDescriptorRequest.Index,
                 urb->UrbControlDescriptorRequest.TransferBufferLength,
                 urb->UrbHeader.Status);
        break;
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_ENDPOINT:
        DbgPrint("meusbpca: %s class request %02x value %04x index %04x len %lu flags %lx status %lx\n", when,
                 urb->UrbControlVendorClassRequest.Request,
                 urb->UrbControlVendorClassRequest.Value,
                 urb->UrbControlVendorClassRequest.Index,
                 urb->UrbControlVendorClassRequest.TransferBufferLength,
                 urb->UrbControlVendorClassRequest.TransferFlags,
                 urb->UrbHeader.Status);
        break;
    case URB_FUNCTION_SELECT_CONFIGURATION:
        DbgPrint("meusbpca: %s select configuration status %lx\n", when, urb->UrbHeader.Status);
        break;
    default:
        DbgPrint("meusbpca: %s urb function %04x len %u status %lx\n", when, function,
                 urb->UrbHeader.Length, urb->UrbHeader.Status);
        break;
    }
}

static NTSTATUS UrbComplete(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
    DbgPrint("meusbpca: irp status %lx irql %u\n", Irp->IoStatus.Status, KeGetCurrentIrql());
    LogUrb("done", (PURB)Context);
    return STATUS_SUCCESS;
}

static NTSTATUS FilterInternalControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PURB urb;

    if (stack->Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
        return FilterPass(Device, Irp);

    urb = (PURB)stack->Parameters.Others.Argument1;
    DbgPrint("meusbpca: submit at irql %u\n", KeGetCurrentIrql());
    LogUrb("send", urb);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, UrbComplete, urb, TRUE, TRUE, TRUE);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS FilterAddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT device;
    PFILTER_EXT ext;
    NTSTATUS status;

    status = IoCreateDevice(Driver, sizeof(FILTER_EXT), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status))
        return status;

    ext = (PFILTER_EXT)device->DeviceExtension;
    ext->Lower = IoAttachDeviceToDeviceStack(device, Pdo);
    if (!ext->Lower) {
        IoDeleteDevice(device);
        return STATUS_NO_SUCH_DEVICE;
    }
    device->Flags |= ext->Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrint("meusbpca: The filter has been attached to the adapter.\n");
    return STATUS_SUCCESS;
}

static VOID FilterUnload(PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrint("meusbpca: The filter driver has been unloaded.\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrint(MEUSBPCA_BANNER("Toshiba MEUSBPCA Filter Driver (URB logging spike)"));
    DbgPrint("meusbpca: The filter driver has been loaded.\n");

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = FilterPass;
    Driver->MajorFunction[IRP_MJ_PNP] = FilterPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = FilterPower;
    Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = FilterInternalControl;
    Driver->DriverExtension->AddDevice = FilterAddDevice;
    Driver->DriverUnload = FilterUnload;
    return STATUS_SUCCESS;
}
