@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Removes the MEUSBPCA filter driver from Windows 98SE and Windows Me.
echo Toshiba MEUSBPCA Driver Uninstaller (Windows 98SE and Windows Me)
echo Version 0.1.0
echo Xlfdll Workstation
echo.
if "%1"=="/?" goto usage
echo This removes the MEUSBPCA filter driver, its cached setup files and the
echo adapter's entries in the registry. Unplug the adapter before continuing.
if "%1"=="/ALL" echo The Windows Me USB storage files will be removed as well.
if "%1"=="/all" echo The Windows Me USB storage files will be removed as well.
echo.
echo Press Ctrl+C to cancel, or
pause
echo.

if exist %windir%\SYSTEM32\DRIVERS\MEUSBPCA.SYS del %windir%\SYSTEM32\DRIVERS\MEUSBPCA.SYS
if exist %windir%\SYSTEM32\DRIVERS\MEUSBPCA.SYS goto inuse
echo The driver file MEUSBPCA.SYS has been removed.

rem Windows keeps third-party INFs in INF\OTHER, with the provider's name in front.
if exist %windir%\INF\OTHER\*MEUSB9X.INF del %windir%\INF\OTHER\*MEUSB9X.INF
if exist %windir%\INF\MEUSB9XD.INF del %windir%\INF\MEUSB9XD.INF
echo The cached setup files have been removed.

rem Every plug-in of the adapter left its own entry, because its serial number changes.
echo REGEDIT4> %TEMP%\MEUSBRM.REG
echo.>> %TEMP%\MEUSBRM.REG
echo [-HKEY_LOCAL_MACHINE\Enum\USB\VID_07C4&PID_A006]>> %TEMP%\MEUSBRM.REG
regedit /s %TEMP%\MEUSBRM.REG
del %TEMP%\MEUSBRM.REG
echo The adapter's registry entries have been removed.

if "%1"=="/ALL" goto mefiles
if "%1"=="/all" goto mefiles
goto done

:mefiles
if exist %windir%\SYSTEM32\DRIVERS\USBSTOR.SYS del %windir%\SYSTEM32\DRIVERS\USBSTOR.SYS
if exist %windir%\SYSTEM32\DRIVERS\USBNTMAP.SYS del %windir%\SYSTEM32\DRIVERS\USBNTMAP.SYS
if exist %windir%\SYSTEM\IOSUBSYS\USBMPHLP.PDR del %windir%\SYSTEM\IOSUBSYS\USBMPHLP.PDR
echo The Windows Me USB storage files have been removed.

:done
echo.
echo The MEUSBPCA driver has been uninstalled. Restart Windows to finish.
goto end

:usage
echo Usage:
echo   UNINST9X         Remove the MEUSBPCA driver.
echo   UNINST9X /ALL    Also remove the Windows Me USB storage files
echo                    (USBSTOR.SYS, USBNTMAP.SYS, USBMPHLP.PDR). Use this
echo                    on Windows 98SE only: on Windows Me they are part of
echo                    Windows, and on 98SE other USB drives may rely on them.
goto end

:inuse
echo The driver file MEUSBPCA.SYS could not be deleted because it is in use.
echo Unplug the adapter, restart Windows and run this again.

:end
