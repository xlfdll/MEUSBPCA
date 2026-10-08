# MEUSBPCA

Open-source Windows support for the **Toshiba MEUSBPCA USB to PC Card Adapter**
(DataFab DF-USB01 chip, USB ID `07C4:A006`), which never had a driver for modern
Windows.

The adapter only speaks ATA, so it works with ATA flash cards, PC Card hard
drives and CompactFlash cards in a PC Card sleeve. Modems, network cards and
linear memory cards cannot work with it.

## Platform Support

| Form | Tested on | Potentially Support |
|---|---|---|
| Kernel filter driver (`meusbpca.sys`) | Windows 98SE, ME, 2000, XP, XP x64, 7 32-bit | Windows Vista 32-bit; 8, 8.1 and 10 32-bit with Secure Boot off |
| iSCSI service and imaging tool (`meusbpca-svc.exe`, `meusbpca.exe`) | Windows 10 x64, 11 x64 | Windows Vista, 7, 8 and 8.1 (32-bit and x64), 10 32-bit, 11 ARM64 |

### Why Two Forms

The split follows Windows'
[kernel driver signing rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-).

- **Kernel filter driver** where Windows loads an unsigned kernel driver: 98SE,
  ME, 2000, XP (including XP x64), and 32-bit Windows from Vista on as long as
  Secure Boot is not in use. A filter under Microsoft's own USB storage driver
  is the most direct route there, and the systems before Vista have neither
  WinUSB nor an iSCSI initiator built in.
- **iSCSI service** where a kernel driver must be signed: 64-bit Windows from
  Vista on, and any Windows 8 or later with Secure Boot on, 32-bit included.
  From Windows 10 version 1607 the signature has to come from Microsoft. The
  service avoids the requirement entirely: it runs in user mode on top of two
  drivers Microsoft already ships and signs, WinUSB and the iSCSI initiator.

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
`meusbpca-svc.exe` in `build\<arch>\`. The DDK build leaves `meusbpca.sys` in a
folder under `src\driver\` named after the build environment.

| Folder | Contents |
|---|---|
| `src\core\` | Protocol library shared by everything |
| `src\driver\` | Kernel filter driver and its DDK build files |
| `src\service\` | iSCSI service |
| `src\tool\` | Command-line imaging tool |
| `src\tests\` | Core tests and the simulated adapter |
| `inf\` | Driver installation files |
| `utils\` | Windows ME file extractor and driver uninstall scripts |

## Platform Notes

### 64-bit Vista through 8.1, Windows 10 and 11

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

### Windows 2000, XP and 32-bit Vista through 8.1

1. Copy the `meusbpca.sys` built for the system into `inf\`: the 32-bit build,
   or the 64-bit build for XP x64.
2. Plug the adapter in and point the hardware wizard at `inf\`. It uses
   `meusbpca.inf`. Windows warns that the driver is not signed.

Windows installs the adapter a second time after the first restart, without
asking for anything. The adapter reports a different serial number each time
it is plugged in, and the driver tells Windows to identify it by its USB port
instead; that takes effect at the first restart.

### Windows ME

1. Copy the 32-bit `meusbpca.sys` into `inf\`.
2. Plug the adapter in and point the hardware wizard at `inf\`. It uses
   `meusb9x.inf`.

### Windows 98SE

98SE has no USB storage driver of its own, so the filter runs under the
Windows ME one. Three Microsoft files are needed, and this project does not
distribute them:

| File | Source |
|---|---|
| `USBSTOR.SYS`, `USBNTMAP.SYS` | The NUSB 3.3 package, or a Windows ME disc |
| `USBMPHLP.PDR` | The NUSB 3.3 package, which carries a copy patched for 98SE |

NUSB 3.3 (`nusb33e.exe`) is available from
[Phil's Computer Lab](https://www.philscomputerlab.com/windows-98-usb-storage-driver.html).
Open it with an archive tool such as 7-Zip and take the files out; the
installer itself is for English Windows 98SE only.

1. Copy the 32-bit `meusbpca.sys` into `inf\`.
2. Copy `USBSTOR.SYS`, `USBNTMAP.SYS` and `USBMPHLP.PDR` from NUSB 3.3 into
   `inf\`. The first two can instead be extracted from a Windows ME disc: from
   a command prompt in `inf\`, with the disc in drive D:, run
   `..\utils\getmefil.bat D:`.
3. Plug the adapter in and point the hardware wizard at `inf\`. It uses
   `meusb9x.inf` for the adapter and `meusb9xd.inf` for the disk behind it.

On 98SE and ME, Windows installs the adapter again each time it is plugged in,
without asking for anything.

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
