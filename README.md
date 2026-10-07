# MEUSBPCA

Open-source Windows support for the **Toshiba MEUSBPCA USB to PC Card Adapter**
(DataFab DF-USB01 chip, USB ID `07C4:A006`), which never had a driver for modern
Windows.

The adapter only speaks ATA, so it works with ATA flash cards, PC Card hard
drives and CompactFlash cards in a PC Card sleeve. Modems, network cards and
linear memory cards cannot work with it.

## Current Status

| Part | Tested on | What was tested |
|---|---|---|
| `meusbpca.exe` command-line imaging tool | Windows 10 x64, Windows 11 x64 | Identifying a card, reading a whole card, writing a whole card |
| `meusbpca-svc.exe` drive-letter service (iSCSI) | Windows 10 x64, Windows 11 x64 | Reading and writing files through the drive letter, read-only mode, card removal and insertion |
| `meusbpca.sys` kernel filter driver | Windows 2000, Windows XP (in VMware) | Installing, reading, copying and deleting files, eject and reconnect |
| `src/core/` protocol library | x86 and x64 builds | Host tests against a simulated adapter |

## Platform Support

| Windows | Form | Prerequisites |
|---|---|---|
| 98SE | Kernel filter driver (`meusbpca.sys`) | Windows ME USB storage files, or NUSB |
| ME, 2000, XP | Kernel filter driver (`meusbpca.sys`) | None |
| XP x64 | Kernel filter driver (64-bit build of `meusbpca.sys`) | None |
| Vista, 7, 8, 8.1 (32-bit) | Kernel filter driver (`meusbpca.sys`) | None |
| Vista, 7, 8, 8.1 (64-bit) | iSCSI service (`meusbpca-svc.exe`) | WinUSB bound with Zadig |
| 10, 11 (x86, x64) | iSCSI service (`meusbpca-svc.exe`) | WinUSB bound with Zadig |
| 11 (ARM64) | iSCSI service (`meusbpca-svc.exe`) | WinUSB selected by hand in Device Manager |

The `meusbpca.exe` imaging tool runs on Vista and later, with the same WinUSB
prerequisite as the service.

### Why Two Forms

The split follows Windows' kernel driver signing rules.

- **Kernel filter driver** where Windows loads an unsigned kernel driver: 98SE,
  ME, 2000, XP (including XP x64) and 32-bit Vista through 8.1. A filter under
  Microsoft's own USB storage driver is the most direct route there, and those
  older systems have neither WinUSB nor an iSCSI initiator built in.
- **iSCSI service** where a kernel driver must be signed: 64-bit Windows from
  Vista on, and ARM64. Windows 10 and 11 go further and require the signature
  to come from Microsoft. The service avoids the requirement entirely: it runs
  in user mode on top of two drivers Microsoft already ships and signs, WinUSB
  and the iSCSI initiator, so it installs with Secure Boot on. Windows 10 and
  11 use the service on every architecture.

### How It Works

The adapter uses a vendor protocol: an 8-byte command carrying the ATA
registers, followed by 512-byte sectors on two bulk endpoints. The protocol
follows the Linux `usb-storage` DataFab subdriver.

- `src/core/` is portable C89 shared by everything: the DataFab transport, a SCSI
  disk emulation on top of it, and USB Bulk-Only Transport framing.
- On Vista and later, the adapter is bound to Microsoft's WinUSB driver and
  `meusbpca-svc.exe` runs a small iSCSI target on `127.0.0.1`. Windows' built-in
  iSCSI initiator connects to it and the card appears as a removable drive. No
  kernel driver is involved, so nothing needs signing and Secure Boot stays on.
- On 98SE through 32-bit Windows 8.1 and on XP x64, `meusbpca.sys` is a lower
  filter under Microsoft's `usbstor.sys`. It answers `usbstor`'s standard
  mass-storage requests itself and talks to the adapter in its own protocol, so
  Windows sees an ordinary removable USB drive.

## Development

| What | Runs on | Toolchain | Command |
|---|---|---|---|
| Tool and service, x64 | Vista x64 through 11 | Visual Studio (C++ tools) and a Windows SDK | `build.cmd x64` |
| Tool and service, x86 | 32-bit Vista through 10 | Same | `build.cmd x86` |
| Tool and service, ARM64 | 11 on ARM64 | Same, plus the MSVC ARM64 build tools component | `build.cmd arm64` |
| Kernel filter, x86 | 98SE through 32-bit 8.1 | Windows Server 2003 SP1 DDK, Windows 2000 build environment | `build -cZ` in `src\driver\` |
| Kernel filter, x64 | XP x64 | Windows Server 2003 SP1 DDK, x64 build environment | `build -cZ` in `src\driver\` |

`build.cmd` also builds and runs the core tests, and puts `meusbpca.exe` and
`meusbpca-svc.exe` in `build\<arch>\`.

## Platform Notes

### Windows Vista and Later

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

### Windows 98SE

98SE has no generic USB storage driver. The kernel filter relies on the
Windows ME one: install `USBSTOR.SYS`, `USBNTMAP.SYS`, `USBMPHLP.PDR` and their
INFs from a Windows ME disc, or the NUSB pack, before installing this driver.
This project does not distribute Microsoft files.

### VMware Workstation

The kernel filter driver works with the adapter passed through to a virtual
machine, including across guest restarts. VMware resets the adapter when a
guest restarts, which leaves it refusing data; the driver detects that and
reconfigures the adapter.

Stop the `meusbpca` service on the host (`net stop meusbpca`) before connecting
the adapter to a VM. The service uses the adapter continuously, and taking the
adapter away from it leaves the adapter unresponsive in the guest.

## License & Acknowledgement

Released under GPL-2.0-or-later; see `COPYING`.

The protocol implementation is derived from `drivers/usb/storage/datafab.c` in
the Linux kernel, (c) 2000 Jimmie Mayfield and (c) 2002 Alan Stern.
