@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
echo Toshiba MEUSBPCA Windows Me File Extractor
echo Version 0.1.0
echo Xlfdll Workstation
echo.
if "%1"=="" goto usage
if not exist %1\WIN9X\*.CAB goto nocabs

rem The files sit in one of the CD's cabinets; try them all.
for %%c in (%1\WIN9X\*.CAB) do extract /Y /L . %%c USBSTOR.SYS USBNTMAP.SYS > NUL

if not exist USBSTOR.SYS goto failed
if not exist USBNTMAP.SYS goto failed
echo USBSTOR.SYS and USBNTMAP.SYS have been extracted to this folder.
if exist USBMPHLP.PDR goto end
echo.
echo USBMPHLP.PDR is still needed in this folder. Use the copy from the
echo NUSB 3.3 package: the one on the Windows Me CD does not work on
echo Windows 98 Second Edition.
goto end

:usage
echo Usage:
echo   GETMEFIL drive:    Extract USBSTOR.SYS and USBNTMAP.SYS from the Windows
echo                      Me CD in that drive, for example GETMEFIL D:
echo.
echo Run it from the folder that holds MEUSB9X.INF. Windows 98 Second Edition
echo also needs USBMPHLP.PDR from the NUSB 3.3 package in that folder.
goto end

:nocabs
echo The Windows Me installation files were not found in %1\WIN9X.
goto end

:failed
echo The Windows Me USB storage files could not be extracted from %1\WIN9X.

:end
