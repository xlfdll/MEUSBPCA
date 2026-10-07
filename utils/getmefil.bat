@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
echo Toshiba MEUSBPCA Windows Me File Extractor
echo Version 0.1.0
echo Xlfdll Workstation
echo.
if "%1"=="" goto usage
if not exist %1\WIN9X\*.CAB goto nocabs

rem The three files sit in one of the CD's cabinets; try them all.
for %%c in (%1\WIN9X\*.CAB) do extract /Y /L . %%c USBSTOR.SYS USBNTMAP.SYS USBMPHLP.PDR > NUL

if not exist USBSTOR.SYS goto failed
if not exist USBNTMAP.SYS goto failed
if not exist USBMPHLP.PDR goto failed
echo USBSTOR.SYS, USBNTMAP.SYS and USBMPHLP.PDR have been extracted to this folder.
goto end

:usage
echo Usage:
echo   GETMEFIL drive:    Extract the Windows Me USB storage files from the
echo                      Windows Me CD in that drive, for example GETMEFIL D:
goto end

:nocabs
echo The Windows Me installation files were not found in %1\WIN9X.
goto end

:failed
echo The Windows Me USB storage files could not be extracted from %1\WIN9X.

:end
