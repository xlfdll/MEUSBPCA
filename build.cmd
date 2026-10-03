@echo off
rem Build the user-mode parts with Visual Studio's C compiler.
rem   build.cmd [x64|x86]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

if defined VSCMD_VER goto :have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT echo Visual Studio was not found. & exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul 2>nul || exit /b 1
:have_env

set OUT=%~dp0build\%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
set CFLAGS=/nologo /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\"
set CORE=%~dp0core\datafab.c %~dp0core\scsi.c %~dp0core\bot.c

cl %CFLAGS% /Fe"%OUT%\test_core.exe" %CORE% %~dp0tests\fake_device.c %~dp0tests\test_core.c || exit /b 1
"%OUT%\test_core.exe" || exit /b 1

if exist "%~dp0tool\cli.c" (
    cl %CFLAGS% /Fe"%OUT%\meusbpca.exe" %CORE% %~dp0service\winusb_io.c %~dp0tool\cli.c ^
        /link winusb.lib setupapi.lib user32.lib || exit /b 1
)
if exist "%~dp0service\main.c" (
    cl %CFLAGS% /Fe"%OUT%\meusbpca-svc.exe" %CORE% %~dp0service\winusb_io.c %~dp0service\iscsi.c ^
        %~dp0service\initiator.c %~dp0service\service.c %~dp0service\main.c %~dp0tests\fake_device.c ^
        /link winusb.lib setupapi.lib user32.lib ws2_32.lib advapi32.lib || exit /b 1
)
echo The build has completed: %OUT%
