/*
 * ps5http: VLC's http:// and https:// access through the console's own HTTP
 * service (libSceHttp, with libSceSsl doing the TLS and the system's
 * certificates). VLC's http module with GnuTLS ended the app on the console
 * the moment a connection to a server started (runs 12-13); the console's
 * service is what PS5 apps use, so it is preferred here, and VLC's own http
 * module stays as the fallback.
 *
 * One request at a time per stream; a seek is a new request with a Range
 * header. Redirects are followed by VLC (VLC_ACCESS_REDIRECT), so playlists
 * (HLS) resolve their relative addresses against the final one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define MODULE_NAME ps5http
#define MODULE_STRING "ps5http"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_interrupt.h>
#include <vlc_plugin.h>
#include <vlc_url.h>

/* ---- the console's HTTP service (libSceNet, libSceSsl, libSceHttp) ---- */
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t pool_size);
int sceHttpInit(int net_pool, int ssl_ctx, size_t pool_size);
int sceHttpCreateTemplate(int http_ctx, const char *user_agent, int http_version, int auto_proxy);
int sceHttpSetAutoRedirect(int id, int enabled);
int sceHttpSetResolveTimeOut(int id, uint32_t usec);
int sceHttpSetConnectTimeOut(int id, uint32_t usec);
int sceHttpSetSendTimeOut(int id, uint32_t usec);
int sceHttpSetRecvTimeOut(int id, uint32_t usec);
int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int keep_alive);
int sceHttpDeleteConnection(int conn);
int sceHttpCreateRequestWithURL(int conn, int method, const char *url, uint64_t content_length);
int sceHttpDeleteRequest(int req);
int sceHttpAbortRequest(int req);
int sceHttpAddRequestHeader(int req, const char *name, const char *value, uint32_t mode);
int sceHttpSendRequest(int req, const void *data, size_t size);
int sceHttpGetStatusCode(int req, int *status);
int sceHttpGetAllResponseHeaders(int req, char **headers, size_t *size);
int sceHttpGetResponseContentLength(int req, int *result, uint64_t *length);
int sceHttpReadData(int req, void *data, size_t size);

/* Straight to the log, flushed: VLC keeps a listing's messages quiet, and the
 * console may end the app before the log is written out. */
#define PLOG(...) \
    do { \
        fprintf(stderr, "ps5http: " __VA_ARGS__); \
        fputc('\n', stderr); \
        fflush(stderr); \
    } while (0)

enum { HTTP_VERSION_1_1 = 2, METHOD_GET = 0, HEADER_OVERWRITE = 0, CONTENT_LENGTH_EXISTS = 0 };

static pthread_once_t service_once = PTHREAD_ONCE_INIT;
static int http_template = -1;
/* The console refused a request outright (run 15: 0x8095f00c on every https
 * request): VLC's own http module does the rest of the session's. */
static bool service_refused;

static void start_service(void)
{
    sceNetInit();
    int pool = sceNetPoolCreate("vlcps5_http", 1024 * 1024, 0);
    int ssl = sceSslInit(320 * 1024);
    int http = pool >= 0 && ssl >= 0 ? sceHttpInit(pool, ssl, 4 * 1024 * 1024) : -1;
    if (http < 0) {
        fprintf(stderr, "ps5http: the console's HTTP service didn't start (net %d, ssl %d, http %d)\n",
                pool, ssl, http);
        return;
    }
    int t = sceHttpCreateTemplate(http, "VLC/3.0.24 LibVLC/3.0.24", HTTP_VERSION_1_1, 1);
    if (t < 0) {
        fprintf(stderr, "ps5http: no request template (%#x)\n", (unsigned)t);
        return;
    }
    sceHttpSetAutoRedirect(t, 0); /* VLC follows them: see the top */
    sceHttpSetResolveTimeOut(t, 10 * 1000000);
    sceHttpSetConnectTimeOut(t, 10 * 1000000);
    sceHttpSetSendTimeOut(t, 15 * 1000000);
    sceHttpSetRecvTimeOut(t, 20 * 1000000);
    http_template = t;
    fprintf(stderr, "ps5http: the console's HTTP service is ready\n");
}

/* ---- one stream ---- */

typedef struct ps5http_sys {
    int conn, req;
    uint64_t pos;         /* where the next byte read comes from */
    uint64_t size;        /* 0: unknown (live, chunked) */
    bool can_seek;
    bool eof;
    char *content_type;
    char *redirect;       /* Location of a 3xx */
    int status;
} ps5http_sys;

static void drop_request(ps5http_sys *sys)
{
    if (sys->req >= 0)
        sceHttpDeleteRequest(sys->req);
    if (sys->conn >= 0)
        sceHttpDeleteConnection(sys->conn);
    sys->req = sys->conn = -1;
}

/* A header's value from the raw response headers ("Name: value\r\n..."). */
static char *header_value(const char *headers, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = headers, *end = headers + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            eol = end;
        if ((size_t)(eol - p) > nlen && !strncasecmp(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            const char *ve = eol;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' '))
                ve--;
            return strndup(v, (size_t)(ve - v));
        }
        p = eol + 1;
    }
    return NULL;
}

static void abort_on_interrupt(void *data)
{
    int *req = data;
    if (*req >= 0)
        sceHttpAbortRequest(*req);
}

/* Sends a GET from `offset`; fills status, size, type, redirect. */
static int send_request(stream_t *access, uint64_t offset)
{
    ps5http_sys *sys = access->p_sys;
    drop_request(sys);
    free(sys->redirect);
    sys->redirect = NULL;
    sys->eof = false;

    sys->conn = sceHttpCreateConnectionWithURL(http_template, access->psz_url, 1);
    if (sys->conn < 0) {
        PLOG("connection failed (%#x)", (unsigned)sys->conn);
        return VLC_EGENERIC;
    }
    sys->req = sceHttpCreateRequestWithURL(sys->conn, METHOD_GET, access->psz_url, 0);
    if (sys->req < 0) {
        PLOG("request failed (%#x)", (unsigned)sys->req);
        drop_request(sys);
        return VLC_EGENERIC;
    }
    char *ua = var_InheritString(access, "http-user-agent");
    char *ref = var_InheritString(access, "http-referrer");
    if (ua && *ua)
        sceHttpAddRequestHeader(sys->req, "User-Agent", ua, HEADER_OVERWRITE);
    if (ref && *ref)
        sceHttpAddRequestHeader(sys->req, "Referer", ref, HEADER_OVERWRITE);
    free(ua);
    free(ref);
    sceHttpAddRequestHeader(sys->req, "Accept", "*/*", HEADER_OVERWRITE);
    if (offset > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", offset);
        sceHttpAddRequestHeader(sys->req, "Range", range, HEADER_OVERWRITE);
    }

    vlc_interrupt_register(abort_on_interrupt, &sys->req);
    int r = sceHttpSendRequest(sys->req, NULL, 0);
    vlc_interrupt_unregister();
    if (r < 0) {
        PLOG("sending failed (%#x): VLC's own http is used from now on", (unsigned)r);
        service_refused = true;
        drop_request(sys);
        return VLC_EGENERIC;
    }
    int status = 0;
    sceHttpGetStatusCode(sys->req, &status);
    sys->status = status;

    char *headers = NULL;
    size_t hlen = 0;
    if (sceHttpGetAllResponseHeaders(sys->req, &headers, &hlen) >= 0 && headers) {
        if (status >= 300 && status < 400)
            sys->redirect = header_value(headers, hlen, "Location");
        if (!sys->content_type)
            sys->content_type = header_value(headers, hlen, "Content-Type");
        char *ranges = header_value(headers, hlen, "Accept-Ranges");
        sys->can_seek = status == 206 || (ranges && !strcasecmp(ranges, "bytes"));
        free(ranges);
    }

    int has = 1;
    uint64_t len = 0;
    if (status >= 200 && status < 300 &&
        sceHttpGetResponseContentLength(sys->req, &has, &len) >= 0 && has == CONTENT_LENGTH_EXISTS)
        sys->size = (status == 206 ? offset : 0) + len;
    sys->pos = status == 206 ? offset : 0;
    PLOG("HTTP %d, %" PRIu64 " bytes%s", status, sys->size, sys->can_seek ? ", seekable" : "");
    return VLC_SUCCESS;
}

static ssize_t Read(stream_t *access, void *buf, size_t len)
{
    ps5http_sys *sys = access->p_sys;
    if (sys->eof || sys->req < 0)
        return 0;
    vlc_interrupt_register(abort_on_interrupt, &sys->req);
    int n = sceHttpReadData(sys->req, buf, len);
    vlc_interrupt_unregister();
    if (n <= 0) {
        if (n < 0)
            PLOG("read failed (%#x)", (unsigned)n);
        sys->eof = true;
        return 0;
    }
    sys->pos += (uint64_t)n;
    return n;
}

static int Seek(stream_t *access, uint64_t offset)
{
    ps5http_sys *sys = access->p_sys;
    if (!sys->can_seek)
        return VLC_EGENERIC;
    if (sys->size && offset >= sys->size) {
        drop_request(sys);
        sys->pos = offset;
        sys->eof = true;
        return VLC_SUCCESS;
    }
    if (send_request(access, offset) != VLC_SUCCESS)
        return VLC_EGENERIC;
    if (sys->status == 416) {
        sys->eof = true;
        return VLC_SUCCESS;
    }
    return sys->status == 206 || (offset == 0 && sys->status == 200) ? VLC_SUCCESS : VLC_EGENERIC;
}

static int Control(stream_t *access, int query, va_list args)
{
    ps5http_sys *sys = access->p_sys;
    switch (query) {
    case STREAM_CAN_SEEK:
        *va_arg(args, bool *) = sys->can_seek;
        break;
    case STREAM_CAN_FASTSEEK:
        *va_arg(args, bool *) = false;
        break;
    case STREAM_CAN_PAUSE:
    case STREAM_CAN_CONTROL_PACE:
        *va_arg(args, bool *) = true;
        break;
    case STREAM_GET_SIZE:
        if (!sys->size)
            return VLC_EGENERIC;
        *va_arg(args, uint64_t *) = sys->size;
        break;
    case STREAM_GET_PTS_DELAY:
        *va_arg(args, int64_t *) = INT64_C(1000) * var_InheritInteger(access, "network-caching");
        break;
    case STREAM_GET_CONTENT_TYPE:
        if (!sys->content_type)
            return VLC_EGENERIC;
        *va_arg(args, char **) = strdup(sys->content_type);
        break;
    case STREAM_SET_PAUSE_STATE:
        break;
    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    stream_t *access = (stream_t *)obj;
    ps5http_sys *sys = access->p_sys;
    drop_request(sys);
    free(sys->content_type);
    free(sys->redirect);
    free(sys);
}

static int Open(vlc_object_t *obj)
{
    stream_t *access = (stream_t *)obj;
    pthread_once(&service_once, start_service);
    if (http_template < 0 || service_refused)
        return VLC_EGENERIC; /* VLC's own http module takes over */

    ps5http_sys *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    sys->conn = sys->req = -1;
    access->p_sys = sys;
    PLOG("open %s", access->psz_url);

    if (send_request(access, 0) != VLC_SUCCESS)
        goto error;
    if (sys->redirect) {
        /* VLC opens the new address (and probes the modules again). */
        char *to = vlc_uri_resolve(access->psz_url, sys->redirect);
        PLOG("redirected (%d) to %s", sys->status, to ? to : sys->redirect);
        free(access->psz_url);
        access->psz_url = to ? to : strdup(sys->redirect);
        Close(obj);
        return VLC_ACCESS_REDIRECT;
    }
    if (sys->status == 401 || sys->status == 407) {
        /* A login: VLC's own http module asks for it. */
        PLOG("HTTP %d: leaving it to the http module", sys->status);
        goto error;
    }
    if (sys->status < 200 || sys->status >= 300) {
        PLOG("HTTP %d", sys->status);
        goto error;
    }
    access->pf_read = Read;
    access->pf_block = NULL;
    access->pf_seek = Seek;
    access->pf_control = Control;
    return VLC_SUCCESS;

error:
    Close(obj);
    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname("PS5 HTTP")
    set_description("HTTP/HTTPS through the console's network service")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access", 300)
    add_shortcut("http", "https")
    set_callbacks(Open, Close)
vlc_module_end()
