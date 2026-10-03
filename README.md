# MEUSBPCA

Open-source Windows support for the **Toshiba MEUSBPCA USB to PC Card adapter**
(DataFab DF-USB01 chip, USB ID `07C4:A006`), which never had a driver for modern
Windows.

The adapter only speaks ATA, so it works with ATA flash cards, PC Card hard
drives and CompactFlash cards in a PC Card sleeve. Modems, network cards and
linear memory cards cannot work with it.

## Status

| Part | Windows | State |
|---|---|---|
| `meusbpca.exe` command-line imaging tool | Vista and later (WinUSB) | Working; tested on Windows 11 x64 |
| `meusbpca-svc.exe` drive-letter service (iSCSI) | Vista and later | Reading works on Windows 11 x64; writing not yet tested on a real card; untested on older Windows |
| `meusbpca.sys` kernel filter | 98SE, ME, 2000, XP, 32-bit Vista/7/8 | Logging-only spike, not yet compiled or tested |

## How it works

The adapter uses a vendor protocol: an 8-byte command carrying the ATA
registers, followed by 512-byte sectors on two bulk endpoints. The protocol
follows the Linux `usb-storage` DataFab subdriver.

- `core/` is portable C89 shared by everything: the DataFab transport, a SCSI
  disk emulation on top of it, and USB Bulk-Only Transport framing.
- On Vista and later, the adapter is bound to Microsoft's WinUSB driver and
  `meusbpca-svc.exe` runs a small iSCSI target on `127.0.0.1`. Windows' built-in
  iSCSI initiator connects to it and the card appears as a removable drive. No
  kernel driver is involved, so nothing needs signing and Secure Boot stays on.
- On 98SE through 32-bit Windows 8, the plan is a lower filter under Microsoft's
  `usbstor.sys` that makes the adapter look like a standard mass-storage device.

## Building

Needs Visual Studio with the C++ tools and a Windows SDK.

```
build.cmd          (x64)
build.cmd x86
```

This builds and runs the core tests, then produces `build\<arch>\meusbpca.exe`
and `build\<arch>\meusbpca-svc.exe`.

The kernel filter in `driver/` is built separately with a Windows 2000-era DDK
(`build -cZ` in the Windows 2000 build environment of the Windows Server 2003
SP1 DDK).

## Using it on Windows Vista and later

1. Bind the adapter to WinUSB. Run [Zadig](https://zadig.akeo.ie), select
   **USB To PCMCIA** (`07C4 A006`), choose **WinUSB** and install.
2. Image a card without any service:

   ```
   meusbpca info
   meusbpca read card.img
   meusbpca write card.img --yes
   ```

3. Or get a drive letter. From an administrator prompt:

   ```
   meusbpca-svc install              read-only
   meusbpca-svc install --writable   read and write
   meusbpca-svc uninstall
   ```

   The service starts the Microsoft iSCSI Initiator service, connects it to the
   local target, and disconnects again when stopped. Cards can be inserted and
   removed while it runs.

   For testing without installing anything, `meusbpca-svc run --verbose` serves
   in a console, and `meusbpca-svc connect` / `disconnect` (administrator)
   drive the initiator. `--image FILE` serves a disk image instead of the
   adapter.

Eject the drive in Explorer before pulling a card you have written to.

## Windows 98SE

98SE has no generic USB storage driver. The kernel filter relies on the
Windows ME one: install `USBSTOR.SYS`, `USBNTMAP.SYS`, `USBMPHLP.PDR` and their
INFs from a Windows ME disc, or the NUSB pack, before installing this driver.
This project does not distribute Microsoft files.

## Licence

GPL-2.0-or-later; see `COPYING`. The protocol implementation is derived from
`drivers/usb/storage/datafab.c` in the Linux kernel, (c) 2000 Jimmie Mayfield
and (c) 2002 Alan Stern.
