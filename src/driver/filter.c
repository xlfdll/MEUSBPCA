/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meusbpca.sys: WDM lower filter under Microsoft's usbstor.sys for the Toshiba
 * MEUSBPCA USB to PC Card Adapter (DataFab DF-USB01).
 *
 * usbstor is bound to the adapter by the INF and speaks USB Mass Storage
 * Bulk-Only Transport to it. The adapter does not understand that, so this
 * filter answers in its place: it takes usbstor's bulk and class requests off
 * the stack, runs the SCSI command through the portable core, performs the
 * adapter's own vendor transfers on the real pipes, and completes usbstor's
 * requests as a Bulk-Only device would.
 *
 * Intercepted requests are pended and handled one at a time by a system
 * thread, so the vendor transfers can be made synchronously at PASSIVE_LEVEL.
 *
 * Restricted to what Windows 98SE's WDM exports: no remove locks, no
 * cancel-safe queues, no MmGetSystemAddressForMdlSafe.
 */
#include <wdm.h>
#include <usbdi.h>
#include <usbioctl.h>

#include "../core/bot.h"
#include "../core/scsi.h"
#include "../core/version.h"

/* MmGetSystemAddressForMdl is the only MDL mapping call 98SE has. */
#pragma warning(disable: 4995 4996)

#define XFER_MAX    0x20000UL       /* largest data phase; usbstor uses 64 KiB */
#define POOL_TAG    'PBSM'

/* Room for the interface description usbstor gets back: header plus 4 pipes. */
#define IFACE_MAX   (sizeof(USBD_INTERFACE_INFORMATION) + 3 * sizeof(USBD_PIPE_INFORMATION))

/* Define as 1 for a diagnostic build that logs every command and request
 * (set C_DEFINES=-DMEUSBPCA_TRACE=1 before running build). */
#ifndef MEUSBPCA_TRACE
#define MEUSBPCA_TRACE 0
#endif
#if MEUSBPCA_TRACE
#define TRACE(args) DbgPrint args
#else
#define TRACE(args)
#endif

/* Pipe requests usbstor may send; the last two are missing from old headers. */
#ifndef URB_FUNCTION_SYNC_RESET_PIPE
#define URB_FUNCTION_SYNC_RESET_PIPE    0x0030
#endif
#ifndef URB_FUNCTION_SYNC_CLEAR_STALL
#define URB_FUNCTION_SYNC_CLEAR_STALL   0x0031
#endif

#define BOT_GET_MAX_LUN 0xFE
#define BOT_RESET       0xFF

typedef enum _BOT_PHASE {
    PHASE_CBW,                      /* waiting for a command block wrapper */
    PHASE_DATA_OUT,
    PHASE_DATA_IN,
    PHASE_CSW
} BOT_PHASE;

typedef struct _FILTER_EXT {
    PDEVICE_OBJECT   Lower;
    USBD_PIPE_HANDLE BulkIn;        /* the adapter's pipes as they are now */
    USBD_PIPE_HANDLE BulkOut;
    USBD_PIPE_HANDLE UpperIn;       /* the handles usbstor was given; they stay */
    USBD_PIPE_HANDLE UpperOut;      /* valid for it across a reconfiguration */
    UCHAR            ConfigDesc[64];    /* descriptor usbstor configured with */
    ULONG            ConfigDescLen;
    union {                         /* as returned by SELECT_CONFIGURATION */
        USBD_INTERFACE_INFORMATION Info;
        UCHAR                      Raw[IFACE_MAX];
    } Interface;
    ULONG            InterfaceLen;
    ULONG            MaxTransfer;   /* largest single transfer the pipes take */
    BOOLEAN          NeedPortReset; /* a transfer to the adapter timed out */
    BOOLEAN          CardAnnounced; /* the current card has been logged */

    LIST_ENTRY       Queue;         /* pended IRPs from usbstor */
    KSPIN_LOCK       QueueLock;
    KSEMAPHORE       QueueSem;
    KEVENT           ThreadExit;
    BOOLEAN          ThreadStarted;
    BOOLEAN          Stopping;

    ULONG            TimeoutMs;
    df_dev           Card;
    df_scsi          Scsi;

    BOT_PHASE        Phase;
    df_cbw           Cbw;
    UCHAR            CswStatus;
    ULONG            DataLen;       /* data-in bytes produced by the command */
    ULONG            DataPos;
    ULONG            Transferred;   /* data bytes moved for this command */
    PUCHAR           Data;          /* XFER_MAX bytes, non-paged */

    UCHAR            Scratch[16];   /* non-paged home for small vendor transfers */
} FILTER_EXT, *PFILTER_EXT;

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath);

/* ---- transfers to the real adapter ---- */

static NTSTATUS SyncComplete(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Send an internal USB request down and wait for it, canceling it on timeout. */
static NTSTATUS SubmitRequest(PFILTER_EXT ext, ULONG code, PURB urb, ULONG timeoutMs)
{
    KEVENT event;
    LARGE_INTEGER due;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;
    PIRP irp;

    irp = IoAllocateIrp(ext->Lower->StackSize, FALSE);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = code;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, SyncComplete, &event, TRUE, TRUE, TRUE);

    IoCallDriver(ext->Lower, irp);
    due.QuadPart = Int32x32To64(-10000, (LONG)timeoutMs);
    if (KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &due) == STATUS_TIMEOUT) {
        IoCancelIrp(irp);
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS SubmitUrb(PFILTER_EXT ext, PURB urb)
{
    return SubmitRequest(ext, IOCTL_INTERNAL_USB_SUBMIT_URB, urb, ext->TimeoutMs);
}

static int BulkTransfer(PFILTER_EXT ext, USBD_PIPE_HANDLE pipe, PVOID buf, ULONG len, ULONG flags)
{
    BOOLEAN in = (flags & USBD_TRANSFER_DIRECTION_IN) != 0;
    PUCHAR target = (PUCHAR)buf;
    ULONG done = 0, n;
    NTSTATUS status;
    URB urb;

    if (!pipe)
        return -1;
    /* Commands and status bytes come from the caller's stack; hand the USB
     * stack memory from the device extension instead. */
    if (len <= sizeof(ext->Scratch)) {
        target = ext->Scratch;
        if (!in)
            RtlCopyMemory(target, buf, len);
    }

    /* Windows 2000 refuses a transfer larger than the pipe's limit, so a long
     * data phase goes out as several transfers. The limit is a multiple of the
     * packet size, so the adapter sees one continuous stream. */
    while (done < len) {
        n = len - done;
        if (n > ext->MaxTransfer)
            n = ext->MaxTransfer;
        RtlZeroMemory(&urb, sizeof(urb));
        urb.UrbHeader.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
        urb.UrbHeader.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
        urb.UrbBulkOrInterruptTransfer.PipeHandle = pipe;
        urb.UrbBulkOrInterruptTransfer.TransferFlags = flags;
        urb.UrbBulkOrInterruptTransfer.TransferBuffer = target + done;
        urb.UrbBulkOrInterruptTransfer.TransferBufferLength = n;

        status = SubmitUrb(ext, &urb);
        if (!NT_SUCCESS(status) || !USBD_SUCCESS(urb.UrbHeader.Status) ||
            urb.UrbBulkOrInterruptTransfer.TransferBufferLength != n) {
            if (status == STATUS_CANCELLED)
                ext->NeedPortReset = TRUE;
            /* An empty slot answers IDENTIFY with two status bytes: not worth a line. */
            if (!(in && len == DF_SECTOR_SIZE && urb.UrbBulkOrInterruptTransfer.TransferBufferLength == 2))
                DbgPrint("meusbpca: A bulk %s transfer of %lu bytes to the adapter has failed at offset %lu "
                         "(status %lx, USB status %lx, %lu bytes moved, timeout %lu ms).\n",
                         in ? "IN" : "OUT", len, done, status, urb.UrbHeader.Status,
                         urb.UrbBulkOrInterruptTransfer.TransferBufferLength, ext->TimeoutMs);
            return -1;
        }
        done += n;
    }
    if (in && target != (PUCHAR)buf)
        RtlCopyMemory(buf, target, len);
    return 0;
}

static int IoBulkOut(void *ctx, const df_u8 *buf, df_u32 len)
{
    PFILTER_EXT ext = (PFILTER_EXT)ctx;

    if (len == 8)
        TRACE(("meusbpca: [trace] adapter command %02X %02X %02X %02X %02X %02X %02X %02X\n",
               buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]));
    else
        TRACE(("meusbpca: [trace] adapter data out, %lu bytes\n", len));
    return BulkTransfer(ext, ext->BulkOut, (PVOID)buf, len, 0);
}

static int IoBulkIn(void *ctx, df_u8 *buf, df_u32 len)
{
    PFILTER_EXT ext = (PFILTER_EXT)ctx;
    int rc = BulkTransfer(ext, ext->BulkIn, buf, len,
                          USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK);

    TRACE(("meusbpca: [trace] adapter data in, %lu bytes: %s\n", len, rc == 0 ? "ok" : "failed"));
    return rc;
}

static void ResetPipe(PFILTER_EXT ext, USBD_PIPE_HANDLE pipe)
{
    URB urb;

    if (!pipe)
        return;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.UrbHeader.Length = sizeof(struct _URB_PIPE_REQUEST);
    urb.UrbHeader.Function = URB_FUNCTION_RESET_PIPE;
    urb.UrbPipeRequest.PipeHandle = pipe;
    SubmitUrb(ext, &urb);
}

/*
 * Read and discard whatever the adapter still has queued on its IN endpoint.
 * It refuses new commands while it holds unread data, which is the state it
 * is left in when a previous owner stopped in the middle of a read: a guest
 * that was restarted, or another driver the adapter was taken away from.
 * Returns the number of bytes thrown away.
 */
static ULONG DrainAdapter(PFILTER_EXT ext)
{
    ULONG total = 0, got;
    URB urb;

    if (!ext->BulkIn || !ext->Data)
        return 0;
    /* One vendor command carries at most 255 sectors; allow for two. */
    while (total < 2 * 255 * DF_SECTOR_SIZE) {
        RtlZeroMemory(&urb, sizeof(urb));
        urb.UrbHeader.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
        urb.UrbHeader.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
        urb.UrbBulkOrInterruptTransfer.PipeHandle = ext->BulkIn;
        urb.UrbBulkOrInterruptTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
        urb.UrbBulkOrInterruptTransfer.TransferBuffer = ext->Data;
        urb.UrbBulkOrInterruptTransfer.TransferBufferLength = ext->MaxTransfer;
        SubmitRequest(ext, IOCTL_INTERNAL_USB_SUBMIT_URB, &urb, 300);
        got = urb.UrbBulkOrInterruptTransfer.TransferBufferLength;
        if (got > ext->MaxTransfer)
            got = 0;
        total += got;
        if (got < ext->MaxTransfer)
            break;
    }
    return total;
}

/*
 * Take the adapter out of its configuration and put it back in.
 *
 * After a USB reset that arrives once the adapter has been in use (VMware
 * issues one when a guest restarts), the adapter answers control requests but
 * no longer accepts data, and being configured again does not change that:
 * it seems to treat the request as "already configured". Deconfiguring first
 * makes the next configuration a real one. usbstor keeps the pipe handles it
 * was given; only this driver uses the new ones.
 */
static BOOLEAN ReconfigureAdapter(PFILTER_EXT ext)
{
    PUSBD_INTERFACE_INFORMATION info;
    NTSTATUS status;
    ULONG size, i;
    PURB urb;

    if (!ext->ConfigDescLen || !ext->InterfaceLen)
        return FALSE;
    size = sizeof(struct _URB_SELECT_CONFIGURATION) - sizeof(USBD_INTERFACE_INFORMATION) + ext->InterfaceLen;
    urb = (PURB)ExAllocatePoolWithTag(NonPagedPool, size, POOL_TAG);
    if (!urb)
        return FALSE;

    RtlZeroMemory(urb, size);
    urb->UrbHeader.Length = sizeof(struct _URB_SELECT_CONFIGURATION);
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    SubmitRequest(ext, IOCTL_INTERNAL_USB_SUBMIT_URB, urb, 5000);
    ext->BulkIn = NULL;
    ext->BulkOut = NULL;

    RtlZeroMemory(urb, size);
    urb->UrbHeader.Length = (USHORT)size;
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->UrbSelectConfiguration.ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)ext->ConfigDesc;
    info = &urb->UrbSelectConfiguration.Interface;
    RtlCopyMemory(info, ext->Interface.Raw, ext->InterfaceLen);
    status = SubmitRequest(ext, IOCTL_INTERNAL_USB_SUBMIT_URB, urb, 5000);
    if (NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status)) {
        for (i = 0; i < info->NumberOfPipes; i++) {
            if (info->Pipes[i].PipeType != UsbdPipeTypeBulk)
                continue;
            if (info->Pipes[i].EndpointAddress & 0x80)
                ext->BulkIn = info->Pipes[i].PipeHandle;
            else
                ext->BulkOut = info->Pipes[i].PipeHandle;
        }
    } else {
        DbgPrint("meusbpca: The adapter could not be configured again (status %lx, USB status %lx).\n",
                 status, urb->UrbHeader.Status);
    }
    ExFreePool(urb);
    return ext->BulkIn && ext->BulkOut;
}

static void IoRecover(void *ctx)
{
    PFILTER_EXT ext = (PFILTER_EXT)ctx;
    ULONG drained;

    /*
     * A transfer that timed out means the adapter has stopped answering.
     * It may be holding data from a read nobody finished, so collect that
     * first. If there was none, reconfigure it, and only if that cannot be
     * done fall back to resetting its USB port.
     */
    if (ext->NeedPortReset) {
        ext->NeedPortReset = FALSE;
        drained = DrainAdapter(ext);
        if (drained) {
            DbgPrint("meusbpca: The adapter had stopped answering; %lu bytes of unread data "
                     "have been discarded from it.\n", drained);
        } else if (ReconfigureAdapter(ext)) {
            DbgPrint("meusbpca: The adapter had stopped answering; it has been reconfigured.\n");
        } else {
            DbgPrint("meusbpca: The adapter has stopped answering; its USB port is being reset.\n");
            SubmitRequest(ext, IOCTL_INTERNAL_USB_RESET_PORT, NULL, 5000);
        }
    }
    /* Same recovery the WinUSB service uses, which is proven on this adapter:
     * the failed transfer is already canceled, then both pipes are reset. */
    ResetPipe(ext, ext->BulkIn);
    ResetPipe(ext, ext->BulkOut);
}

static void IoSleep(void *ctx, unsigned ms)
{
    LARGE_INTEGER delay;

    UNREFERENCED_PARAMETER(ctx);
    delay.QuadPart = Int32x32To64(-10000, (LONG)ms);
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static void IoSetTimeout(void *ctx, unsigned ms)
{
    ((PFILTER_EXT)ctx)->TimeoutMs = ms;
}

/* ---- Bulk-Only Transport emulation toward usbstor ---- */

static VOID CompleteUrb(PIRP Irp, PURB urb, NTSTATUS status, USBD_STATUS usbStatus)
{
    urb->UrbHeader.Status = usbStatus;
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static PUCHAR MapTransfer(PVOID buffer, PMDL mdl)
{
    if (buffer)
        return (PUCHAR)buffer;
    if (mdl && !mdl->Next)
        return (PUCHAR)MmGetSystemAddressForMdl(mdl);
    return NULL;
}

/* Run the SCSI command in the current CBW through the core. */
static VOID Execute(PFILTER_EXT ext)
{
    df_u32 produced = 0;
    df_u32 room = ext->Cbw.data_in ? ext->Cbw.data_len : ext->DataPos;
    int status;

    TRACE(("meusbpca: [trace] SCSI %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X, %s %lu bytes\n",
           ext->Cbw.cdb[0], ext->Cbw.cdb[1], ext->Cbw.cdb[2], ext->Cbw.cdb[3], ext->Cbw.cdb[4],
           ext->Cbw.cdb[5], ext->Cbw.cdb[6], ext->Cbw.cdb[7], ext->Cbw.cdb[8], ext->Cbw.cdb[9],
           ext->Cbw.data_in ? "in" : "out", ext->Cbw.data_len));
    status = df_scsi_exec(&ext->Scsi, ext->Cbw.cdb, ext->Data, room, &produced);
    TRACE(("meusbpca: [trace] SCSI result %d, %lu bytes produced\n", status, produced));

    ext->DataLen = produced;
    ext->DataPos = 0;
    ext->CswStatus = (UCHAR)(status == DF_SCSI_GOOD ? DF_CSW_PASSED : DF_CSW_FAILED);
    if (!ext->Cbw.data_in)
        ext->Transferred = status == DF_SCSI_GOOD ? ext->Cbw.data_len : 0;
    if (status != DF_SCSI_GOOD)
        DbgPrint("meusbpca: SCSI command %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                 "has failed with sense %X/%02X/%02X.\n",
                 ext->Cbw.cdb[0], ext->Cbw.cdb[1], ext->Cbw.cdb[2], ext->Cbw.cdb[3], ext->Cbw.cdb[4],
                 ext->Cbw.cdb[5], ext->Cbw.cdb[6], ext->Cbw.cdb[7], ext->Cbw.cdb[8], ext->Cbw.cdb[9],
                 ext->Scsi.sense_key, ext->Scsi.asc, ext->Scsi.ascq);
    if (status != DF_SCSI_GOOD && (ext->Scsi.sense_key == 0x02 || ext->Scsi.sense_key == 0x06))
        ext->CardAnnounced = FALSE;     /* card gone, or a new one */
    if (status == DF_SCSI_GOOD && ext->Cbw.cdb[0] == 0x25 && !ext->CardAnnounced) {
        ext->CardAnnounced = TRUE;
        DbgPrint("meusbpca: A card has been detected in slot %d (%lu sectors).\n",
                 ext->Card.slot, ext->Card.sectors);
    }
}

static VOID HandleBulk(PFILTER_EXT ext, PIRP Irp, PURB urb)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *x = &urb->UrbBulkOrInterruptTransfer;
    PUCHAR buf = MapTransfer(x->TransferBuffer, x->TransferBufferMDL);
    ULONG len = x->TransferBufferLength;
    ULONG n;

    if (!buf && len)
        goto fail;

    if (x->PipeHandle == ext->UpperOut) {
        if (ext->Phase == PHASE_CBW) {
            if (df_cbw_parse(buf, len, &ext->Cbw) != 0)
                goto fail;
            ext->DataLen = 0;
            ext->DataPos = 0;
            ext->Transferred = 0;
            if (ext->Cbw.data_len > XFER_MAX) {
                ext->CswStatus = DF_CSW_PHASE_ERROR;
                ext->Phase = PHASE_CSW;
            } else if (ext->Cbw.data_len && !ext->Cbw.data_in) {
                ext->Phase = PHASE_DATA_OUT;
            } else {
                Execute(ext);
                ext->Phase = ext->Cbw.data_len ? PHASE_DATA_IN : PHASE_CSW;
            }
        } else if (ext->Phase == PHASE_DATA_OUT) {
            n = ext->Cbw.data_len - ext->DataPos;
            if (n > len)
                n = len;
            RtlCopyMemory(ext->Data + ext->DataPos, buf, n);
            ext->DataPos += n;
            if (ext->DataPos >= ext->Cbw.data_len) {
                Execute(ext);
                ext->Phase = PHASE_CSW;
            }
        } else {
            goto fail;
        }
        CompleteUrb(Irp, urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    }

    if (ext->Phase == PHASE_DATA_IN) {
        n = ext->DataLen - ext->DataPos;
        if (n > len)
            n = len;
        RtlCopyMemory(buf, ext->Data + ext->DataPos, n);
        ext->DataPos += n;
        ext->Transferred += n;
        /* A short or empty packet ends the data phase, as on a real device. */
        if (n < len || ext->DataPos >= ext->DataLen)
            ext->Phase = PHASE_CSW;
        x->TransferBufferLength = n;
    } else if (ext->Phase == PHASE_CSW) {
        if (len < DF_CSW_LEN)
            goto fail;
        df_csw_build(buf, ext->Cbw.tag, ext->Cbw.data_len - ext->Transferred, ext->CswStatus);
        ext->Phase = PHASE_CBW;
        x->TransferBufferLength = DF_CSW_LEN;
    } else {
        goto fail;
    }
    CompleteUrb(Irp, urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
    return;

fail:
    DbgPrint("meusbpca: An unexpected bulk request has been refused (phase %d, length %lu).\n",
             ext->Phase, len);
    ext->Phase = PHASE_CBW;
    x->TransferBufferLength = 0;
    CompleteUrb(Irp, urb, STATUS_UNSUCCESSFUL, USBD_STATUS_STALL_PID);
}

static VOID HandleClass(PFILTER_EXT ext, PIRP Irp, PURB urb)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *x = &urb->UrbControlVendorClassRequest;
    PUCHAR buf = MapTransfer(x->TransferBuffer, x->TransferBufferMDL);

    if (x->Request == BOT_GET_MAX_LUN && buf && x->TransferBufferLength >= 1) {
        buf[0] = 0;                 /* one logical unit */
        x->TransferBufferLength = 1;
    } else if (x->Request == BOT_RESET) {
        ext->Phase = PHASE_CBW;
        x->TransferBufferLength = 0;
    } else {
        x->TransferBufferLength = 0;
        CompleteUrb(Irp, urb, STATUS_UNSUCCESSFUL, USBD_STATUS_STALL_PID);
        return;
    }
    CompleteUrb(Irp, urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
}

/* usbstor resetting or aborting a pipe: apply it to the pipe as it is now. */
static VOID HandlePipeRequest(PFILTER_EXT ext, PIRP Irp, PURB urb)
{
    USBD_PIPE_HANDLE upper = urb->UrbPipeRequest.PipeHandle;

    if (urb->UrbHeader.Function != URB_FUNCTION_ABORT_PIPE) {
        if (upper == ext->UpperIn)
            ResetPipe(ext, ext->BulkIn);
        else if (upper == ext->UpperOut)
            ResetPipe(ext, ext->BulkOut);
    }
    CompleteUrb(Irp, urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
}

static VOID WorkerThread(PVOID Context)
{
    PFILTER_EXT ext = (PFILTER_EXT)Context;
    PLIST_ENTRY entry;
    PIRP irp;
    PURB urb;

    for (;;) {
        KeWaitForSingleObject(&ext->QueueSem, Executive, KernelMode, FALSE, NULL);
        entry = ExInterlockedRemoveHeadList(&ext->Queue, &ext->QueueLock);
        if (!entry) {
            if (ext->Stopping)
                break;
            continue;
        }
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        urb = (PURB)IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1;

        if (ext->Stopping || irp->Cancel)
            CompleteUrb(irp, urb, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        else if (urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
            HandleBulk(ext, irp, urb);
        else if (urb->UrbHeader.Function == URB_FUNCTION_CLASS_INTERFACE)
            HandleClass(ext, irp, urb);
        else
            HandlePipeRequest(ext, irp, urb);
    }
    KeSetEvent(&ext->ThreadExit, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID StopWorker(PFILTER_EXT ext)
{
    LARGE_INTEGER grace;

    if (!ext->ThreadStarted)
        return;
    ext->Stopping = TRUE;
    KeReleaseSemaphore(&ext->QueueSem, IO_NO_INCREMENT, 1, FALSE);
    KeWaitForSingleObject(&ext->ThreadExit, Executive, KernelMode, FALSE, NULL);
    /* Let the thread get out of this driver's code before it can be unloaded. */
    grace.QuadPart = Int32x32To64(-10000, 100);
    KeDelayExecutionThread(KernelMode, FALSE, &grace);
    ext->ThreadStarted = FALSE;
}

/* ---- requests passed through, with a look at the results ---- */

static VOID CapturePipes(PFILTER_EXT ext, PUSBD_INTERFACE_INFORMATION info)
{
    ULONG limit = XFER_MAX;
    ULONG i;

    for (i = 0; i < info->NumberOfPipes; i++) {
        if (info->Pipes[i].PipeType != UsbdPipeTypeBulk)
            continue;
        if (info->Pipes[i].MaximumTransferSize >= 64 && info->Pipes[i].MaximumTransferSize < limit)
            limit = info->Pipes[i].MaximumTransferSize;
        if (info->Pipes[i].EndpointAddress & 0x80)
            ext->BulkIn = info->Pipes[i].PipeHandle;
        else
            ext->BulkOut = info->Pipes[i].PipeHandle;
    }
    ext->MaxTransfer = limit & ~63UL;       /* whole 64-byte packets only */
    ext->Phase = PHASE_CBW;
    DbgPrint("meusbpca: The bulk pipes have been captured (in %p, out %p, up to %lu bytes per transfer).\n",
             ext->BulkIn, ext->BulkOut, ext->MaxTransfer);
}

/* Make the interface look like Mass Storage / SCSI / Bulk-Only to usbstor. */
static VOID PatchConfigDescriptor(PUCHAR data, ULONG len)
{
    ULONG pos = 0;

    while (pos + 2 <= len && data[pos] >= 2 && pos + data[pos] <= len) {
        if (data[pos + 1] == USB_INTERFACE_DESCRIPTOR_TYPE && data[pos] >= 9) {
            data[pos + 5] = 0x08;
            data[pos + 6] = 0x06;
            data[pos + 7] = 0x50;
        }
        pos += data[pos];
    }
}

static NTSTATUS PassedUrbComplete(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;
    PURB urb = (PURB)Context;
    PUCHAR data;

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
    if (!NT_SUCCESS(Irp->IoStatus.Status))
        return STATUS_SUCCESS;

    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_SELECT_CONFIGURATION:
        ext->InterfaceLen = 0;
        if (urb->UrbSelectConfiguration.ConfigurationDescriptor) {
            PUSBD_INTERFACE_INFORMATION info = &urb->UrbSelectConfiguration.Interface;

            data = (PUCHAR)urb->UrbSelectConfiguration.ConfigurationDescriptor;
            CapturePipes(ext, info);
            ext->UpperIn = ext->BulkIn;
            ext->UpperOut = ext->BulkOut;
            if (info->Length <= IFACE_MAX) {
                RtlCopyMemory(ext->Interface.Raw, info, info->Length);
                ext->InterfaceLen = info->Length;
            }
            ext->ConfigDescLen = data[2] | ((ULONG)data[3] << 8);
            if (ext->ConfigDescLen <= sizeof(ext->ConfigDesc))
                RtlCopyMemory(ext->ConfigDesc, data, ext->ConfigDescLen);
            else
                ext->ConfigDescLen = 0;
        } else {
            ext->BulkIn = ext->BulkOut = NULL;
            ext->UpperIn = ext->UpperOut = NULL;
            ext->ConfigDescLen = 0;
        }
        break;
    case URB_FUNCTION_SELECT_INTERFACE:
        CapturePipes(ext, &urb->UrbSelectInterface.Interface);
        ext->UpperIn = ext->BulkIn;
        ext->UpperOut = ext->BulkOut;
        break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        data = (PUCHAR)urb->UrbControlDescriptorRequest.TransferBuffer;
        if (data && urb->UrbControlDescriptorRequest.DescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE)
            PatchConfigDescriptor(data, urb->UrbControlDescriptorRequest.TransferBufferLength);
        /* The adapter reports a different serial number every time it is
         * plugged in, and usbstor names the disk after it, so Windows would
         * install the disk afresh each time. Say there is no serial number;
         * usbstor then names the disk after the port instead. */
        if (data && urb->UrbControlDescriptorRequest.DescriptorType == USB_DEVICE_DESCRIPTOR_TYPE &&
            urb->UrbControlDescriptorRequest.TransferBufferLength >= 18)
            data[16] = 0;
        break;
    default:
        break;
    }
    return STATUS_SUCCESS;
}

/*
 * usbstor follows SELECT_CONFIGURATION with SELECT_INTERFACE, which puts a
 * SET_INTERFACE request on the wire. The adapter has a single alternate
 * setting and stops accepting bulk OUT data after that request, so answer it
 * here from what SELECT_CONFIGURATION returned and keep it off the bus.
 */
static BOOLEAN AnswerSelectInterface(PFILTER_EXT ext, PIRP Irp, PURB urb)
{
    PUSBD_INTERFACE_INFORMATION want = &urb->UrbSelectInterface.Interface;

    if (!ext->InterfaceLen || want->Length < ext->InterfaceLen ||
        want->AlternateSetting != ext->Interface.Info.AlternateSetting)
        return FALSE;
    RtlCopyMemory(want, ext->Interface.Raw, ext->InterfaceLen);
    DbgPrint("meusbpca: The interface selection has been answered without involving the adapter.\n");
    CompleteUrb(Irp, urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
    return TRUE;
}

static NTSTATUS FilterPass(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS FilterInternalControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PURB urb;

    if (stack->Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB) {
        TRACE(("meusbpca: [trace] internal request from above, code %08lX\n",
               stack->Parameters.DeviceIoControl.IoControlCode));
        return FilterPass(Device, Irp);
    }
    urb = (PURB)stack->Parameters.Others.Argument1;

    if (urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
        TRACE(("meusbpca: [trace] request from above, function %04X\n", urb->UrbHeader.Function));

    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_RESET_PIPE:
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE:
    case URB_FUNCTION_SYNC_CLEAR_STALL:
        /* Answered by this driver; never reaches the adapter as is. */
        IoMarkIrpPending(Irp);
        ExInterlockedInsertTailList(&ext->Queue, &Irp->Tail.Overlay.ListEntry, &ext->QueueLock);
        KeReleaseSemaphore(&ext->QueueSem, IO_NO_INCREMENT, 1, FALSE);
        return STATUS_PENDING;

    case URB_FUNCTION_SELECT_INTERFACE:
        if (AnswerSelectInterface(ext, Irp, urb))
            return STATUS_SUCCESS;
        /* fall through */
    case URB_FUNCTION_SELECT_CONFIGURATION:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, PassedUrbComplete, urb, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->Lower, Irp);

    default:
        return FilterPass(Device, Irp);
    }
}

static NTSTATUS FilterPower(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;

    TRACE(("meusbpca: [trace] power request, minor %02X\n",
           IoGetCurrentIrpStackLocation(Irp)->MinorFunction));
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->Lower, Irp);
}

static NTSTATUS FilterPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT lower = ext->Lower;
    NTSTATUS status;

    TRACE(("meusbpca: [trace] PnP request, minor %02X\n", stack->MinorFunction));
    if (stack->MinorFunction != IRP_MN_REMOVE_DEVICE)
        return FilterPass(Device, Irp);

    StopWorker(ext);
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(lower, Irp);
    IoDetachDevice(lower);
    if (ext->Data)
        ExFreePool(ext->Data);
    IoDeleteDevice(Device);
    DbgPrint("meusbpca: The filter has been detached from the adapter.\n");
    return status;
}

static NTSTATUS FilterAddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT device;
    PFILTER_EXT ext;
    HANDLE thread;
    NTSTATUS status;
    df_io io;

    status = IoCreateDevice(Driver, sizeof(FILTER_EXT), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status)) {
        DbgPrint("meusbpca: The filter device could not be created (status %lx).\n", status);
        return status;
    }
    ext = (PFILTER_EXT)device->DeviceExtension;
    RtlZeroMemory(ext, sizeof(FILTER_EXT));
    InitializeListHead(&ext->Queue);
    KeInitializeSpinLock(&ext->QueueLock);
    KeInitializeSemaphore(&ext->QueueSem, 0, MAXLONG);
    KeInitializeEvent(&ext->ThreadExit, NotificationEvent, FALSE);
    ext->TimeoutMs = DF_IO_TIMEOUT_MS;
    ext->MaxTransfer = 4096;
    ext->Phase = PHASE_CBW;

    io.ctx = ext;
    io.bulk_out = IoBulkOut;
    io.bulk_in = IoBulkIn;
    io.recover = IoRecover;
    io.sleep_ms = IoSleep;
    io.set_timeout = IoSetTimeout;
    df_init(&ext->Card, &io);
    df_scsi_init(&ext->Scsi, &ext->Card, 0);

    ext->Data = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, XFER_MAX, POOL_TAG);
    if (!ext->Data) {
        IoDeleteDevice(device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ext->Lower = IoAttachDeviceToDeviceStack(device, Pdo);
    if (!ext->Lower) {
        DbgPrint("meusbpca: The filter could not be attached to the device stack.\n");
        ExFreePool(ext->Data);
        IoDeleteDevice(device);
        return STATUS_NO_SUCH_DEVICE;
    }

    status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WorkerThread, ext);
    if (!NT_SUCCESS(status)) {
        DbgPrint("meusbpca: The worker thread could not be created (status %lx).\n", status);
        IoDetachDevice(ext->Lower);
        ExFreePool(ext->Data);
        IoDeleteDevice(device);
        return status;
    }
    ZwClose(thread);
    ext->ThreadStarted = TRUE;

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
    DbgPrint(MEUSBPCA_BANNER("Toshiba MEUSBPCA Filter Driver"));
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
