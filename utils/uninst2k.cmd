@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Removes the MEUSBPCA filter driver from Windows 2000 and later.
setlocal
echo Toshiba MEUSBPCA Driver Uninstaller (Windows 2000 and later)
echo Version 0.1.0
echo Xlfdll Workstation
echo.
echo This removes the MEUSBPCA filter driver, its cached setup files and its
echo registry entries. Before continuing, remove the adapter in Device Manager
echo and unplug it. Administrator rights are required.
echo.
echo Press Ctrl+C to cancel, or
pause
echo.

set DRIVER=%SystemRoot%\system32\drivers\meusbpca.sys
if exist "%DRIVER%" del "%DRIVER%"
if exist "%DRIVER%" goto inuse
echo The driver file meusbpca.sys has been removed.

rem Windows keeps its own copy of the INF as oemNN.inf.
for %%f in ("%SystemRoot%\inf\oem*.inf") do call :checkinf "%%f"

rem The service entry, and the setting that ignores the adapter's serial number.
rem Windows XP and later can remove the service properly; Windows 2000 has no
rem tool for that, so there the entry goes with the registry and a restart.
if exist "%SystemRoot%\system32\sc.exe" sc delete meusbpca > nul 2>&1
set REGFILE=%TEMP%\meusbpca-remove.reg
echo REGEDIT4> "%REGFILE%"
echo.>> "%REGFILE%"
echo [-HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\meusbpca]>> "%REGFILE%"
echo.>> "%REGFILE%"
echo [HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\UsbFlags]>> "%REGFILE%"
echo "IgnoreHWSerNum07C4A006"=->> "%REGFILE%"
regedit /s "%REGFILE%"
del "%REGFILE%"
echo The registry entries of the MEUSBPCA driver have been removed.
echo.
echo The MEUSBPCA driver has been uninstalled.
echo Restart Windows before installing the driver again.
goto end

:checkinf
findstr /i /c:"meusbpca.sys" "%~1" > nul 2>&1
if errorlevel 1 goto :eof
if exist "%SystemRoot%\system32\pnputil.exe" pnputil -f -d "%~nx1" > nul 2>&1
if exist "%~1" del "%~1"
if exist "%~dpn1.pnf" del "%~dpn1.pnf"
echo The cached setup file %~nx1 has been removed.
goto :eof

:inuse
echo The driver file meusbpca.sys could not be deleted because it is in use.
echo Unplug the adapter, restart Windows and run this again.

:end
endlocal
