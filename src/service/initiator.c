/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Point the Microsoft iSCSI initiator at the local target, through iscsicli
 * (present on every Windows since Vista). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "iscsi.h"

/* Run a command line hidden, capturing stdout+stderr. Returns its exit code,
 * or -1 if it could not be started. */
static int run(char *cmdline, char *out, size_t out_len)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    DWORD got, code = (DWORD)-1;
    size_t used = 0;
    char chunk[512];

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    while (ReadFile(rd, chunk, sizeof(chunk), &got, NULL) && got) {
        if (out && used + got < out_len) {
            memcpy(out + used, chunk, got);
            used += got;
        }
    }
    if (out && out_len)
        out[used] = '\0';
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
}

static int start_initiator_service(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS status;
    int tries, ok = 0;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return -1;
    svc = OpenServiceA(scm, "MSiSCSI", SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc) {
        StartServiceA(svc, 0, NULL);        /* fails harmlessly if already running */
        for (tries = 0; tries < 100 && !ok; tries++) {
            if (!QueryServiceStatus(svc, &status))
                break;
            if (status.dwCurrentState == SERVICE_RUNNING)
                ok = 1;
            else
                Sleep(100);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok ? 0 : -1;
}

/* A session id looks like ffffe001a1b2c3d4-4000013700000002. */
static int is_session_id(const char *p)
{
    int i;

    for (i = 0; i < 33; i++) {
        if (i == 16 ? p[i] != '-' : !isxdigit((unsigned char)p[i]))
            return 0;
    }
    return 1;
}

/* Find the session logged in to our target; copies its id (33 chars + NUL). */
static int find_session(char *id)
{
    static char list[16384];
    char cmd[] = "iscsicli SessionList";
    const char *target, *p;

    if (run(cmd, list, sizeof(list)) != 0)
        return 0;
    target = strstr(list, ISCSI_TARGET_NAME);
    if (!target)
        return 0;
    /* The id is printed a few lines above the target name. */
    for (p = target; p >= list; p--) {
        if (is_session_id(p)) {
            memcpy(id, p, 33);
            id[33] = '\0';
            return 1;
        }
    }
    return 0;
}

int initiator_connect(unsigned short port)
{
    char cmd[256], out[2048], id[34];
    int rc, tries;

    if (start_initiator_service() != 0) {
        fputs("The Microsoft iSCSI Initiator service could not be started. "
              "Administrator rights are required.\n", stderr);
        return -1;
    }
    if (find_session(id))
        return 0;

    sprintf(cmd, "iscsicli QAddTargetPortal 127.0.0.1 %u", port);
    rc = run(cmd, out, sizeof(out));
    /* Adding the portal does not always run discovery at once (seen right
     * after the initiator service starts), so refresh and retry the login. */
    for (tries = 0; rc == 0 && tries < 10; tries++) {
        sprintf(cmd, "iscsicli QLoginTarget %s", ISCSI_TARGET_NAME);
        if (run(cmd, out, sizeof(out)) == 0)
            return 0;
        Sleep(1000);
        sprintf(cmd, "iscsicli RefreshTargetPortal 127.0.0.1 %u", port);
        rc = run(cmd, out, sizeof(out));
    }
    if (rc == 0)
        rc = -1;
    if (rc != 0)
        fprintf(stderr, "The Windows iSCSI initiator could not be connected to the MEUSBPCA target. "
                "The command \"%s\" has failed with exit code %d:\n%s\n", cmd, rc, out);
    return rc;
}

int initiator_disconnect(unsigned short port)
{
    char cmd[256], out[2048], id[34];
    int rc = 0, tries;

    for (tries = 0; tries < 4 && find_session(id); tries++) {
        sprintf(cmd, "iscsicli LogoutTarget %s", id);
        rc = run(cmd, out, sizeof(out));
        if (rc != 0) {
            fprintf(stderr, "The Windows iSCSI initiator could not be disconnected from the MEUSBPCA target. "
                    "The command \"%s\" has failed with exit code %d:\n%s\n", cmd, rc, out);
            break;
        }
    }
    sprintf(cmd, "iscsicli RemoveTargetPortal 127.0.0.1 %u", port);
    run(cmd, out, sizeof(out));
    return rc;
}
