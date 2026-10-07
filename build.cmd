@echo off
rem Build the user-mode parts with Visual Studio's C compiler.
rem   build.cmd [x64|x86|arm64]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

if defined VSCMD_VER goto :have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT echo Visual Studio was not found. & exit /b 1
rem ARM64 is cross-compiled unless the build machine is itself ARM64.
set VCARCH=%ARCH%
if /i "%ARCH%"=="arm64" if /i not "%PROCESSOR_ARCHITECTURE%"=="ARM64" set VCARCH=x64_arm64
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>nul
if not exist "%VCToolsInstallDir%lib\%ARCH%\libcmt.lib" echo The Visual Studio C++ build tools for %ARCH% are not installed. & exit /b 1
:have_env

set OUT=%~dp0build\%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
set CFLAGS=/nologo /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\"
set CORE=%~dp0src\core\datafab.c %~dp0src\core\scsi.c %~dp0src\core\bot.c

cl %CFLAGS% /Fe"%OUT%\test_core.exe" %CORE% %~dp0src\tests\fake_device.c %~dp0src\tests\test_core.c || exit /b 1
rem A cross-compiled test program cannot run on the build machine.
if /i "%ARCH%"=="arm64" if /i not "%PROCESSOR_ARCHITECTURE%"=="ARM64" goto :skip_tests
"%OUT%\test_core.exe" || exit /b 1
:skip_tests

if exist "%~dp0src\tool\cli.c" (
    cl %CFLAGS% /Fe"%OUT%\meusbpca.exe" %CORE% %~dp0src\service\winusb_io.c %~dp0src\tool\cli.c ^
        /link winusb.lib setupapi.lib user32.lib || exit /b 1
)
if exist "%~dp0src\service\main.c" (
    cl %CFLAGS% /Fe"%OUT%\meusbpca-svc.exe" %CORE% %~dp0src\service\winusb_io.c %~dp0src\service\iscsi.c ^
        %~dp0src\service\initiator.c %~dp0src\service\service.c %~dp0src\service\main.c %~dp0src\tests\fake_device.c ^
        /link winusb.lib setupapi.lib user32.lib ws2_32.lib advapi32.lib || exit /b 1
)
echo The build has completed: %OUT%
