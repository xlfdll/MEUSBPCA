/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal iSCSI target for the Microsoft initiator.
 *
 * Kept deliberately small: no authentication, no digests, error recovery
 * level 0, a command window of one, and solicited data only (InitialR2T=Yes)
 * apart from immediate data. Each connection gets a thread; SCSI execution is
 * serialized because there is one card behind it.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iscsi.h"

#define BHS_LEN         48
#define MAX_RECV_DSL    65536UL             /* largest data segment we accept */
#define MAX_BURST       262144UL
#define MAX_TRANSFER    (8UL * 1024 * 1024) /* largest single SCSI transfer */

#define OP_NOP_OUT      0x00
#define OP_SCSI_CMD     0x01
#define OP_TASK_MGMT    0x02
#define OP_LOGIN        0x03
#define OP_TEXT         0x04
#define OP_DATA_OUT     0x05
#define OP_LOGOUT       0x06

#define OP_NOP_IN       0x20
#define OP_SCSI_RSP     0x21
#define OP_TASK_RSP     0x22
#define OP_LOGIN_RSP    0x23
#define OP_TEXT_RSP     0x24
#define OP_DATA_IN      0x25
#define OP_LOGOUT_RSP   0x26
#define OP_R2T          0x31

#define CLOSE_CONNECTION 1

typedef struct server {
    const iscsi_config *cfg;
    CRITICAL_SECTION    scsi_lock;
    volatile LONG       connections;
} server;

typedef struct conn {
    server *srv;
    SOCKET  sock;
    df_u32  stat_sn;
    df_u32  exp_cmd_sn;
    df_u32  peer_max_dsl;   /* largest data segment the initiator accepts */
    df_u32  max_burst;
    df_u32  next_ttt;
    int     login_started;
    int     full_feature;
    int     discovery;
    int     sent_tpgt;
    int     sent_max_dsl;
    df_u8  *seg;            /* data segment of the PDU being handled */
    df_u8  *xfer;           /* SCSI data buffer, normal sessions only */
} conn;

static df_u32 get32(const df_u8 *p)
{
    return ((df_u32)p[0] << 24) | ((df_u32)p[1] << 16) | ((df_u32)p[2] << 8) | p[3];
}

static void put32(df_u8 *p, df_u32 v)
{
    p[0] = (df_u8)(v >> 24);
    p[1] = (df_u8)(v >> 16);
    p[2] = (df_u8)(v >> 8);
    p[3] = (df_u8)v;
}

static void trace(const conn *c, const char *fmt, ...)
{
    va_list args;

    if (!c->srv->cfg->verbose)
        return;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static int recv_exact(conn *c, df_u8 *buf, df_u32 len)
{
    fd_set set;
    struct timeval tv;
    int n;

    while (len) {
        if (*c->srv->cfg->stop)
            return -1;
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        n = select(0, &set, NULL, NULL, &tv);
        if (n < 0)
            return -1;
        if (n == 0)
            continue;
        n = recv(c->sock, (char *)buf, (int)len, 0);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (df_u32)n;
    }
    return 0;
}

static int send_all(conn *c, const df_u8 *buf, df_u32 len)
{
    int n;

    while (len) {
        n = send(c->sock, (const char *)buf, (int)len, 0);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (df_u32)n;
    }
    return 0;
}

/* Read one PDU: header into bhs, data segment (NUL-terminated) into c->seg. */
static int recv_pdu(conn *c, df_u8 *bhs, df_u32 *dlen)
{
    df_u8 skip[4];
    df_u32 ahs;

    if (recv_exact(c, bhs, BHS_LEN) != 0)
        return -1;
    ahs = (df_u32)bhs[4] * 4;
    *dlen = ((df_u32)bhs[5] << 16) | ((df_u32)bhs[6] << 8) | bhs[7];
    if (*dlen > MAX_RECV_DSL)
        return -1;
    for (; ahs; ahs -= 4) {
        if (recv_exact(c, skip, 4) != 0)
            return -1;
    }
    if (recv_exact(c, c->seg, (*dlen + 3) & ~3UL) != 0)
        return -1;
    c->seg[*dlen] = 0;
    return 0;
}

static int send_pdu(conn *c, df_u8 *bhs, const df_u8 *data, df_u32 dlen)
{
    static const df_u8 pad[4] = { 0, 0, 0, 0 };

    bhs[4] = 0;
    bhs[5] = (df_u8)(dlen >> 16);
    bhs[6] = (df_u8)(dlen >> 8);
    bhs[7] = (df_u8)dlen;
    if (send_all(c, bhs, BHS_LEN) != 0)
        return -1;
    if (dlen && send_all(c, data, dlen) != 0)
        return -1;
    if (dlen & 3)
        return send_all(c, pad, 4 - (dlen & 3));
    return 0;
}

/* Sequence numbers common to every target PDU. The command window is one. */
static void stamp(conn *c, df_u8 *bhs, int carries_status)
{
    if (carries_status)
        put32(bhs + 24, c->stat_sn++);
    put32(bhs + 28, c->exp_cmd_sn);
    put32(bhs + 32, c->exp_cmd_sn);
}

static void note_cmd_sn(conn *c, const df_u8 *req)
{
    if (!(req[0] & 0x40))           /* immediate commands do not advance it */
        c->exp_cmd_sn = get32(req + 24) + 1;
}

static void begin_response(df_u8 *rsp, df_u8 opcode, const df_u8 *req)
{
    memset(rsp, 0, BHS_LEN);
    rsp[0] = opcode;
    memcpy(rsp + 16, req + 16, 4);  /* initiator task tag */
}

/* ---- login and text negotiation ---- */

static void append(char *text, size_t *len, size_t cap, const char *fmt, ...)
{
    va_list args;
    int n;

    if (*len >= cap)
        return;
    va_start(args, fmt);
    n = _vsnprintf(text + *len, cap - *len, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *len) {
        *len = cap;                 /* overflow: response is dropped by caller */
        return;
    }
    *len += (size_t)n + 1;          /* keep the terminating NUL as separator */
}

static int list_has(const char *list, const char *item)
{
    size_t n = strlen(item);

    while (*list) {
        if (strncmp(list, item, n) == 0 && (list[n] == ',' || list[n] == '\0'))
            return 1;
        while (*list && *list != ',')
            list++;
        if (*list == ',')
            list++;
    }
    return 0;
}

static df_u32 min_u32(df_u32 a, df_u32 b)
{
    return a < b ? a : b;
}

static int handle_login(conn *c, const df_u8 *req, df_u32 dlen)
{
    df_u8 rsp[BHS_LEN];
    char text[1024];
    size_t tlen = 0;
    const char *p, *end = (const char *)c->seg + dlen;
    int csg = (req[1] >> 2) & 3;
    int nsg = req[1] & 3;
    int transit = req[1] & 0x80;
    unsigned status = 0;

    if (!c->login_started) {
        c->login_started = 1;
        c->exp_cmd_sn = get32(req + 24);
    }
    if ((req[1] & 0x40) || req[3] != 0)     /* continued text / unknown version */
        status = 0x0200;

    for (p = (const char *)c->seg; p < end; p += strlen(p) + 1) {
        const char *value = strchr(p, '=');
        size_t klen;

        if (!value)
            continue;
        klen = (size_t)(value - p);
        value++;
#define KEY(name) (klen == sizeof(name) - 1 && memcmp(p, name, klen) == 0)
        if (KEY("InitiatorName") || KEY("InitiatorAlias")) {
            /* nothing to answer */
        } else if (KEY("SessionType")) {
            c->discovery = strcmp(value, "Discovery") == 0;
        } else if (KEY("TargetName")) {
            if (strcmp(value, ISCSI_TARGET_NAME) != 0)
                status = 0x0203;            /* target not found */
        } else if (KEY("AuthMethod")) {
            if (list_has(value, "None"))
                append(text, &tlen, sizeof(text), "AuthMethod=None");
            else
                status = 0x0201;            /* authentication failure */
        } else if (KEY("HeaderDigest") || KEY("DataDigest")) {
            if (list_has(value, "None"))
                append(text, &tlen, sizeof(text), "%.*s=None", (int)klen, p);
            else
                status = 0x0200;
        } else if (KEY("MaxRecvDataSegmentLength")) {
            c->peer_max_dsl = min_u32(strtoul(value, NULL, 0), MAX_RECV_DSL);
            if (c->peer_max_dsl < 512)
                c->peer_max_dsl = 512;
        } else if (KEY("InitialR2T")) {
            append(text, &tlen, sizeof(text), "InitialR2T=Yes");
        } else if (KEY("ImmediateData")) {
            append(text, &tlen, sizeof(text), "ImmediateData=%s",
                   strcmp(value, "Yes") == 0 ? "Yes" : "No");
        } else if (KEY("MaxBurstLength")) {
            c->max_burst = min_u32(strtoul(value, NULL, 0), MAX_BURST);
            if (c->max_burst < 512)
                c->max_burst = 512;
            append(text, &tlen, sizeof(text), "MaxBurstLength=%lu", c->max_burst);
        } else if (KEY("FirstBurstLength")) {
            append(text, &tlen, sizeof(text), "FirstBurstLength=%lu",
                   min_u32(strtoul(value, NULL, 0), MAX_RECV_DSL));
        } else if (KEY("DefaultTime2Wait")) {
            append(text, &tlen, sizeof(text), "DefaultTime2Wait=%s", value);
        } else if (KEY("DefaultTime2Retain")) {
            append(text, &tlen, sizeof(text), "DefaultTime2Retain=0");
        } else if (KEY("MaxOutstandingR2T") || KEY("MaxConnections")) {
            append(text, &tlen, sizeof(text), "%.*s=1", (int)klen, p);
        } else if (KEY("DataPDUInOrder") || KEY("DataSequenceInOrder")) {
            append(text, &tlen, sizeof(text), "%.*s=Yes", (int)klen, p);
        } else if (KEY("ErrorRecoveryLevel")) {
            append(text, &tlen, sizeof(text), "ErrorRecoveryLevel=0");
        } else if (KEY("OFMarker") || KEY("IFMarker")) {
            append(text, &tlen, sizeof(text), "%.*s=No", (int)klen, p);
        } else {
            append(text, &tlen, sizeof(text), "%.*s=NotUnderstood", (int)klen, p);
        }
#undef KEY
    }

    if (!c->sent_tpgt && !c->discovery) {
        c->sent_tpgt = 1;
        append(text, &tlen, sizeof(text), "TargetPortalGroupTag=1");
    }
    if (csg == 1 && !c->sent_max_dsl) {
        c->sent_max_dsl = 1;
        append(text, &tlen, sizeof(text), "MaxRecvDataSegmentLength=%lu", MAX_RECV_DSL);
    }
    if (tlen >= sizeof(text))
        status = 0x0300;                    /* target error */

    begin_response(rsp, OP_LOGIN_RSP, req);
    memcpy(rsp + 8, req + 8, 8);            /* ISID and TSIH */
    rsp[1] = (df_u8)(csg << 2);
    rsp[36] = (df_u8)(status >> 8);
    rsp[37] = (df_u8)status;
    if (status) {
        tlen = 0;
    } else if (transit) {
        rsp[1] |= (df_u8)(0x80 | nsg);
        if (nsg == 3) {
            c->full_feature = 1;
            rsp[14] = 0;
            rsp[15] = 1;                    /* TSIH of the new session */
        }
    }
    stamp(c, rsp, 1);
    if (send_pdu(c, rsp, (const df_u8 *)text, (df_u32)tlen) != 0 || status)
        return CLOSE_CONNECTION;
    if (c->full_feature)
        trace(c, "An iSCSI %s session has logged in.", c->discovery ? "discovery" : "normal");
    return 0;
}

static int handle_text(conn *c, const df_u8 *req, df_u32 dlen)
{
    df_u8 rsp[BHS_LEN];
    char text[256];
    size_t tlen = 0;

    note_cmd_sn(c, req);
    if (dlen >= 11 && memcmp(c->seg, "SendTargets", 11) == 0) {
        append(text, &tlen, sizeof(text), "TargetName=%s", ISCSI_TARGET_NAME);
        append(text, &tlen, sizeof(text), "TargetAddress=127.0.0.1:%u,1", c->srv->cfg->port);
    }
    begin_response(rsp, OP_TEXT_RSP, req);
    rsp[1] = 0x80;
    put32(rsp + 20, 0xFFFFFFFFUL);
    stamp(c, rsp, 1);
    return send_pdu(c, rsp, (const df_u8 *)text, (df_u32)tlen) != 0;
}

static int handle_nop_out(conn *c, const df_u8 *req, df_u32 dlen)
{
    df_u8 rsp[BHS_LEN];

    if (get32(req + 16) == 0xFFFFFFFFUL)    /* reply to a ping we never send */
        return 0;
    note_cmd_sn(c, req);
    begin_response(rsp, OP_NOP_IN, req);
    rsp[1] = 0x80;
    memcpy(rsp + 8, req + 8, 8);            /* LUN */
    put32(rsp + 20, 0xFFFFFFFFUL);
    stamp(c, rsp, 1);
    return send_pdu(c, rsp, c->seg, dlen) != 0;
}

static int handle_task_mgmt(conn *c, const df_u8 *req)
{
    df_u8 rsp[BHS_LEN];

    note_cmd_sn(c, req);
    begin_response(rsp, OP_TASK_RSP, req);
    rsp[1] = 0x80;
    /* Commands run to completion before the next PDU is read, so a task to
     * abort never exists; resets have nothing to clear. */
    rsp[2] = (df_u8)((req[1] & 0x7F) == 1 ? 1 : 0);
    stamp(c, rsp, 1);
    return send_pdu(c, rsp, NULL, 0) != 0;
}

static int handle_logout(conn *c, const df_u8 *req)
{
    df_u8 rsp[BHS_LEN];

    note_cmd_sn(c, req);
    begin_response(rsp, OP_LOGOUT_RSP, req);
    rsp[1] = 0x80;
    stamp(c, rsp, 1);
    send_pdu(c, rsp, NULL, 0);
    trace(c, "The iSCSI session has logged out.");
    return CLOSE_CONNECTION;
}

/* ---- SCSI ---- */

static int send_scsi_response(conn *c, const df_u8 *req, int status,
                              const df_u8 *sense, df_u32 residual)
{
    df_u8 rsp[BHS_LEN];
    df_u8 data[2 + DF_SENSE_LEN];
    df_u32 dlen = 0;

    begin_response(rsp, OP_SCSI_RSP, req);
    rsp[1] = (df_u8)(0x80 | (residual ? 0x02 : 0));    /* underflow */
    rsp[3] = (df_u8)status;
    put32(rsp + 44, residual);
    stamp(c, rsp, 1);
    if (status == DF_SCSI_CHECK_CONDITION) {
        data[0] = 0;
        data[1] = DF_SENSE_LEN;
        memcpy(data + 2, sense, DF_SENSE_LEN);
        dlen = sizeof(data);
    }
    return send_pdu(c, rsp, data, dlen) != 0;
}

/* Data-In sequence with the status folded into the last PDU. */
static int send_data_in(conn *c, const df_u8 *req, const df_u8 *data, df_u32 len, df_u32 expected)
{
    df_u8 rsp[BHS_LEN];
    df_u32 offset = 0, sn = 0, n;

    while (offset < len) {
        n = min_u32(len - offset, c->peer_max_dsl);
        begin_response(rsp, OP_DATA_IN, req);
        put32(rsp + 20, 0xFFFFFFFFUL);
        put32(rsp + 36, sn++);
        put32(rsp + 40, offset);
        if (offset + n == len) {
            rsp[1] = (df_u8)(0x80 | 0x01 | (len < expected ? 0x02 : 0));
            put32(rsp + 44, expected - len);
            stamp(c, rsp, 1);
        } else {
            stamp(c, rsp, 0);
        }
        if (send_pdu(c, rsp, data + offset, n) != 0)
            return 1;
        offset += n;
    }
    return 0;
}

/* Collect the data-out of a write: immediate data first, the rest by R2T. */
static int receive_data_out(conn *c, const df_u8 *req, df_u32 immediate, df_u32 total)
{
    df_u8 pdu[BHS_LEN];
    df_u32 got = immediate, r2t_sn = 0, want, burst, dlen, offset;

    while (got < total) {
        want = min_u32(total - got, c->max_burst);
        begin_response(pdu, OP_R2T, req);
        pdu[1] = 0x80;
        memcpy(pdu + 8, req + 8, 8);        /* LUN */
        put32(pdu + 20, c->next_ttt);
        put32(pdu + 24, c->stat_sn);
        put32(pdu + 36, r2t_sn++);
        put32(pdu + 40, got);
        put32(pdu + 44, want);
        stamp(c, pdu, 0);
        if (send_pdu(c, pdu, NULL, 0) != 0)
            return -1;

        for (burst = 0;;) {
            if (recv_pdu(c, pdu, &dlen) != 0)
                return -1;
            if ((pdu[0] & 0x3F) == OP_NOP_OUT) {
                if (handle_nop_out(c, pdu, dlen) != 0)
                    return -1;
                continue;
            }
            if ((pdu[0] & 0x3F) != OP_DATA_OUT || get32(pdu + 20) != c->next_ttt)
                return -1;
            offset = get32(pdu + 40);
            if (offset > total || dlen > total - offset)
                return -1;
            memcpy(c->xfer + offset, c->seg, dlen);
            burst += dlen;
            if (pdu[1] & 0x80)
                break;
        }
        if (burst != want)
            return -1;
        got += burst;
        c->next_ttt++;
    }
    return 0;
}

static int handle_scsi(conn *c, const df_u8 *req, df_u32 dlen)
{
    static const df_u8 no_lun_sense[DF_SENSE_LEN] =
        { 0x70, 0, 0x05, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x25, 0, 0, 0, 0, 0 };
    df_u8 sense[DF_SENSE_LEN];
    const df_u8 *cdb = req + 32;
    df_u32 expected = get32(req + 20);
    df_u32 in_len = 0;
    int writing = req[1] & 0x20;
    int status;

    note_cmd_sn(c, req);
    if (c->discovery || expected > MAX_TRANSFER || (writing && dlen > expected))
        return CLOSE_CONNECTION;

    if (writing) {
        memcpy(c->xfer, c->seg, dlen);
        if (receive_data_out(c, req, dlen, expected) != 0)
            return CLOSE_CONNECTION;
    }

    if ((req[8] & 0x3F) || req[9]) {        /* only LUN 0 exists */
        if (cdb[0] == 0x12 && !writing) {   /* INQUIRY: "no device here" */
            memset(c->xfer, 0, 36);
            c->xfer[0] = 0x7F;
            c->xfer[4] = 31;
            if (expected)
                return send_data_in(c, req, c->xfer, min_u32(36, expected), expected);
            return send_scsi_response(c, req, DF_SCSI_GOOD, NULL, 0);
        }
        return send_scsi_response(c, req, DF_SCSI_CHECK_CONDITION, no_lun_sense, expected);
    }

    EnterCriticalSection(&c->srv->scsi_lock);
    status = df_scsi_exec(c->srv->cfg->scsi, cdb, c->xfer, expected, &in_len);
    df_scsi_sense(c->srv->cfg->scsi, sense);
    LeaveCriticalSection(&c->srv->scsi_lock);

    if (status == DF_SCSI_GOOD)
        trace(c, "SCSI command %02X (LBA %lu, length %lu) has completed.", cdb[0], get32(cdb + 2), expected);
    else
        trace(c, "SCSI command %02X (LBA %lu, length %lu) has failed with sense %X/%02X/%02X.", cdb[0],
              get32(cdb + 2), expected, sense[2], sense[12], sense[13]);

    if (status == DF_SCSI_GOOD && in_len)
        return send_data_in(c, req, c->xfer, in_len, expected);
    return send_scsi_response(c, req, status, sense, writing ? 0 : expected);
}

/* ---- connections ---- */

static DWORD WINAPI connection_thread(void *param)
{
    conn *c = (conn *)param;
    df_u8 bhs[BHS_LEN];
    df_u32 dlen;
    int rc = 0;

    while (rc == 0 && recv_pdu(c, bhs, &dlen) == 0) {
        df_u8 op = (df_u8)(bhs[0] & 0x3F);

        if (!c->full_feature) {
            rc = op == OP_LOGIN ? handle_login(c, bhs, dlen) : CLOSE_CONNECTION;
            if (c->full_feature && !c->discovery) {
                c->xfer = (df_u8 *)malloc(MAX_TRANSFER);
                if (!c->xfer)
                    rc = CLOSE_CONNECTION;
            }
            continue;
        }
        switch (op) {
        case OP_NOP_OUT:    rc = handle_nop_out(c, bhs, dlen);  break;
        case OP_SCSI_CMD:   rc = handle_scsi(c, bhs, dlen);     break;
        case OP_TASK_MGMT:  rc = handle_task_mgmt(c, bhs);      break;
        case OP_TEXT:       rc = handle_text(c, bhs, dlen);     break;
        case OP_LOGOUT:     rc = handle_logout(c, bhs);         break;
        default:            rc = CLOSE_CONNECTION;              break;
        }
    }
    trace(c, "The iSCSI connection has been closed.");
    closesocket(c->sock);
    InterlockedDecrement(&c->srv->connections);
    free(c->xfer);
    free(c->seg);
    free(c);
    return 0;
}

static void start_connection(server *srv, SOCKET sock)
{
    conn *c = (conn *)calloc(1, sizeof(*c));
    BOOL nodelay = TRUE;
    HANDLE thread;

    if (c)
        c->seg = (df_u8 *)malloc(MAX_RECV_DSL + 8);
    if (!c || !c->seg) {
        free(c);
        closesocket(sock);
        return;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    c->srv = srv;
    c->sock = sock;
    c->stat_sn = 1;
    c->peer_max_dsl = 8192;                 /* RFC 3720 defaults until negotiated */
    c->max_burst = MAX_BURST;
    c->next_ttt = 1;

    InterlockedIncrement(&srv->connections);
    thread = CreateThread(NULL, 0, connection_thread, c, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedDecrement(&srv->connections);
        closesocket(sock);
        free(c->seg);
        free(c);
    }
}

int iscsi_serve(const iscsi_config *cfg)
{
    server srv;
    WSADATA wsa;
    struct sockaddr_in addr;
    SOCKET listener, sock;
    fd_set set;
    struct timeval tv;
    int error;

    error = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (error)
        return error;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(cfg->port);
    if (listener == INVALID_SOCKET ||
        bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 4) != 0) {
        error = WSAGetLastError();
        if (listener != INVALID_SOCKET)
            closesocket(listener);
        WSACleanup();
        return error;
    }

    srv.cfg = cfg;
    srv.connections = 0;
    InitializeCriticalSection(&srv.scsi_lock);
    if (cfg->listening)
        SetEvent(cfg->listening);

    while (!*cfg->stop) {
        FD_ZERO(&set);
        FD_SET(listener, &set);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(0, &set, NULL, NULL, &tv) <= 0)
            continue;
        sock = accept(listener, NULL, NULL);
        if (sock != INVALID_SOCKET)
            start_connection(&srv, sock);
    }

    closesocket(listener);
    while (srv.connections)                 /* threads notice *stop within a second */
        Sleep(50);
    DeleteCriticalSection(&srv.scsi_lock);
    WSACleanup();
    return 0;
}
