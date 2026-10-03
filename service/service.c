/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows service wrapper: serve the target, log the initiator in on start
 * and out on stop. */
#include <stdio.h>
#include <string.h>

#include "app.h"

static app_options           svc_options;
static SERVICE_STATUS_HANDLE svc_handle;
static HANDLE                svc_stop_event;
static volatile LONG         svc_stop;

static void report(DWORD state, DWORD exit_code)
{
    SERVICE_STATUS status;

    memset(&status, 0, sizeof(status));
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = exit_code;
    status.dwWaitHint = 15000;
    SetServiceStatus(svc_handle, &status);
}

static void WINAPI control_handler(DWORD control)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        SetEvent(svc_stop_event);
}

static DWORD WINAPI serve_thread(void *listening)
{
    return (DWORD)app_serve(&svc_options, &svc_stop, (HANDLE)listening);
}

static void WINAPI service_main(DWORD argc, char **argv)
{
    HANDLE listening, thread, waits[2];
    DWORD exit_code = 0;

    (void)argc;
    (void)argv;
    svc_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, control_handler);
    if (!svc_handle)
        return;
    report(SERVICE_START_PENDING, 0);

    svc_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    listening = CreateEventA(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, serve_thread, listening, 0, NULL);
    if (!svc_stop_event || !listening || !thread) {
        report(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    waits[0] = listening;
    waits[1] = thread;
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        /* A failed login is not fatal: the target keeps running and the user
         * can connect from the iSCSI Initiator control panel. */
        initiator_connect(svc_options.port);
        report(SERVICE_RUNNING, 0);

        waits[0] = svc_stop_event;
        WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        report(SERVICE_STOP_PENDING, 0);
        initiator_disconnect(svc_options.port);
    }
    InterlockedExchange(&svc_stop, 1);
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    report(SERVICE_STOPPED, exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : 0);
}

int service_dispatch(const app_options *opt)
{
    static SERVICE_TABLE_ENTRYA table[] = {
        { SERVICE_NAME, service_main },
        { NULL, NULL }
    };

    svc_options = *opt;
    return StartServiceCtrlDispatcherA(table) ? 0 : 1;
}

int service_install(const app_options *opt)
{
    static char description[] =
        "Presents ATA PC Cards in the Toshiba MEUSBPCA USB adapter as a disk, through a local iSCSI target.";
    SERVICE_DESCRIPTIONA desc;
    SC_HANDLE scm, svc;
    char exe[MAX_PATH], command[MAX_PATH + 64];
    int rc = 1;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return 1;
    sprintf(command, "\"%s\" service --port %u%s", exe, opt->port, opt->writable ? " --writable" : "");

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "The service manager could not be opened (Windows error %lu). "
                "Administrator rights are required.\n", GetLastError());
        return 1;
    }
    svc = CreateServiceA(scm, SERVICE_NAME, "MEUSBPCA PC Card adapter", SERVICE_ALL_ACCESS,
                         SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                         command, NULL, NULL, "MSiSCSI\0", NULL, NULL);
    if (!svc) {
        fprintf(stderr, "The MEUSBPCA service could not be installed (Windows error %lu).\n", GetLastError());
    } else {
        desc.lpDescription = description;
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
        if (StartServiceA(svc, 0, NULL)) {
            puts("The MEUSBPCA service has been installed and started.");
            rc = 0;
        } else {
            fprintf(stderr, "The MEUSBPCA service has been installed, but it could not be started "
                    "(Windows error %lu).\n", GetLastError());
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return rc;
}

int service_uninstall(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS status;
    int tries, rc = 1;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "The service manager could not be opened (Windows error %lu).\n", GetLastError());
        return 1;
    }
    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        fprintf(stderr, "The MEUSBPCA service could not be opened (Windows error %lu). "
                "It is not installed, or administrator rights are required.\n", GetLastError());
    } else {
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        for (tries = 0; tries < 300; tries++) {
            if (!QueryServiceStatus(svc, &status) || status.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(100);
        }
        if (DeleteService(svc)) {
            puts("The MEUSBPCA service has been stopped and uninstalled.");
            rc = 0;
        } else {
            fprintf(stderr, "The MEUSBPCA service could not be uninstalled (Windows error %lu).\n", GetLastError());
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return rc;
}
