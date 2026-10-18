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
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_interrupt.h>
#include <stdatomic.h>
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
/* failures in a row that no answer came between: the service itself only
 * after a few (one cancelled search turned it off for the whole session and
 * the next channel went to VLC's own http, 2026-10-18) */
static atomic_int service_failures;
static atomic_int https_name_refusals;

/* Servers the console's SSL turned down: straight to VLC's own http next
 * time (every segment of a channel paid a refused attempt first). After
 * several different ones it refuses https as a whole this session (a log of
 * 2026-10-13: every https server refused, GitHub too). */
#define REFUSED_MAX 64
static vlc_mutex_t refused_lock = VLC_STATIC_MUTEX;
static char *refused_hosts[REFUSED_MAX];
static unsigned refused_count;
static bool https_refused;

static char *url_host(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p)
        return NULL;
    p += 3;
    size_t n = strcspn(p, "/?#");
    return strndup(p, n);
}

static bool host_refused(const char *url)
{
    if (strncasecmp(url, "https:", 6))
        return false;
    char *host = url_host(url);
    bool refused = false;
    vlc_mutex_lock(&refused_lock);
    refused = https_refused;
    for (unsigned i = 0; host && !refused && i < refused_count; i++)
        refused = !strcasecmp(refused_hosts[i], host);
    vlc_mutex_unlock(&refused_lock);
    free(host);
    return refused;
}

static unsigned ssl_refused_count;

static void refuse_host(const char *url, bool ssl)
{
    char *host = url_host(url);
    if (!host)
        return;
    vlc_mutex_lock(&refused_lock);
    bool known = false;
    for (unsigned i = 0; i < refused_count; i++)
        known = known || !strcasecmp(refused_hosts[i], host);
    if (!known && refused_count < REFUSED_MAX) {
        refused_hosts[refused_count++] = host;
        host = NULL;
        if (ssl && ++ssl_refused_count == 4) {
            https_refused = true;
            fprintf(stderr, "ps5http: SSL refused 4 different servers: VLC's own http takes every https link\n");
        }
    }
    vlc_mutex_unlock(&refused_lock);
    free(host);
}

static void start_service(void)
{
    sceNetInit();
    int pool = sceNetPoolCreate("vlcps5_http", 4 * 1024 * 1024, 0); /* room for parallel pieces */
    int ssl = sceSslInit(320 * 1024);
    int http = pool >= 0 && ssl >= 0 ? sceHttpInit(pool, ssl, 8 * 1024 * 1024) : -1;
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
    long serial;          /* the player's open this belongs to (0: none) */
    bool logged_direct;
    mtime_t opened_at;    /* for the speed in the log */
    uint64_t read_bytes;
    mtime_t read_wait;    /* time spent inside the console's read */
    bool cancel_logged;
    struct parallel *par; /* a segment fetched over several connections, or NULL */
} ps5http_sys;

/* The player (src/player.cc): its open number was stopped */
extern int vlc_ps5_stream_cancelled(long serial) __attribute__((weak));
extern int vlc_ps5_log_details(void) __attribute__((weak));

static bool cancelled(ps5http_sys *sys)
{
    return sys->serial && vlc_ps5_stream_cancelled && vlc_ps5_stream_cancelled(sys->serial);
}

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

/* The app's own name lookup (ps5/compat/vlc_resolver.c): the console's
 * DNS can be one that blocks (a PS5 set up to keep away from updates sends
 * names to an address that refuses): every named server failed with
 * 0x8041013d while bare addresses worked (2026-10-18). */
extern int vlcps5_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                              struct addrinfo **res);
extern void vlcps5_freeaddrinfo(struct addrinfo *ai);
static atomic_bool direct_broken;   /* a server answered an address-only request wrongly */

/* An http:// link with a name: the same link with the address our lookup
 * gives, and the name for the Host header ("name" or "name:port"). NULL: as
 * it is (https: the secure connection needs the name; an address already). */
static char *direct_url(const char *url, char **host_header)
{
    *host_header = NULL;
    if (atomic_load(&direct_broken) || strncasecmp(url, "http://", 7))
        return NULL;
    const char *h = url + 7;
    size_t hlen = strcspn(h, "/?#");
    if (!hlen || h[0] == '[' || memchr(h, '@', hlen))
        return NULL;
    char *hostport = strndup(h, hlen);
    if (!hostport)
        return NULL;
    char *colon = strchr(hostport, ':');
    char name[256];
    snprintf(name, sizeof(name), "%.*s", colon ? (int)(colon - hostport) : (int)hlen, hostport);
    struct in_addr ip;
    if (inet_pton(AF_INET, name, &ip) == 1) {
        free(hostport);
        return NULL;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (vlcps5_getaddrinfo(name, NULL, &hints, &res) != 0 || !res) {
        free(hostport);
        return NULL;
    }
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, addr, sizeof(addr));
    vlcps5_freeaddrinfo(res);
    char *out = NULL;
    if (asprintf(&out, "http://%s%s%s", addr, colon ? colon : "", h + hlen) < 0) {
        free(hostport);
        return NULL;
    }
    *host_header = hostport;
    return out;
}

/* ---- a whole segment over several connections ------------------------------
 *
 * One connection to a far server is often slower than the line: the console
 * read BBC America at a steady 700 KiB/s, a 4 s segment in 5 s, and the
 * channel froze while the same server played on a PC (2026-10-18). A file of
 * a few MiB served with byte ranges (a live channel's segment) is fetched by
 * PAR_WORKERS connections at once, PAR_CHUNK at a time, into memory; VLC
 * reads it in order as the pieces come. */
#define PAR_CHUNK   ((uint64_t)512 << 10)
#define PAR_WORKERS 4
#define PAR_MIN     ((uint64_t)1 << 20)    /* smaller: one connection is enough */
#define PAR_MAX     ((uint64_t)24 << 20)   /* bigger (films): one connection, as before */

typedef struct parallel {
    char *url, *ua, *ref;
    long serial;
    uint8_t *buf;
    uint64_t size;
    unsigned chunks, next;
    uint32_t *filled;       /* bytes in so far, per chunk */
    bool *failed;
    bool stop;
    int reqs[PAR_WORKERS];  /* each worker's request, for an abort */
    pthread_t threads[PAR_WORKERS];
    int nthreads;
    uint64_t fetched;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} parallel;

static void add_headers(int req, const char *host_header, const char *ua, const char *ref)
{
    if (host_header)
        sceHttpAddRequestHeader(req, "Host", host_header, HEADER_OVERWRITE);
    if (ua && *ua)
        sceHttpAddRequestHeader(req, "User-Agent", ua, HEADER_OVERWRITE);
    if (ref && *ref)
        sceHttpAddRequestHeader(req, "Referer", ref, HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "Accept", "*/*", HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "Cache-Control", "no-cache", HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "Pragma", "no-cache", HEADER_OVERWRITE);
}

static bool par_cancelled(parallel *p)
{
    return p->serial && vlc_ps5_stream_cancelled && vlc_ps5_stream_cancelled(p->serial);
}

/* One piece, from where it got to: true when it's all in. */
static bool par_fetch(parallel *p, int w, unsigned idx)
{
    uint64_t start = idx * PAR_CHUNK, end = start + PAR_CHUNK;
    if (end > p->size)
        end = p->size;
    pthread_mutex_lock(&p->lock);
    uint64_t from = start + p->filled[idx];
    pthread_mutex_unlock(&p->lock);
    if (from >= end)
        return true;
    char *host_header = NULL;
    char *direct = direct_url(p->url, &host_header);
    const char *url = direct ? direct : p->url;
    int conn = sceHttpCreateConnectionWithURL(http_template, url, 1);
    int req = conn >= 0 ? sceHttpCreateRequestWithURL(conn, METHOD_GET, url, 0) : -1;
    free(direct);
    bool ok = false;
    if (req >= 0) {
        add_headers(req, host_header, p->ua, p->ref);
        char range[64];
        snprintf(range, sizeof(range), "bytes=%" PRIu64 "-%" PRIu64, from, end - 1);
        sceHttpAddRequestHeader(req, "Range", range, HEADER_OVERWRITE);
        pthread_mutex_lock(&p->lock);
        p->reqs[w] = p->stop ? -1 : req;
        bool stop = p->stop;
        pthread_mutex_unlock(&p->lock);
        int status = 0;
        if (!stop && sceHttpSendRequest(req, NULL, 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0 &&
            (status == 206 || (status == 200 && from == 0))) {
            ok = true;
            while (from < end) {
                size_t want = end - from > (64u << 10) ? (64u << 10) : (size_t)(end - from);
                int n = sceHttpReadData(req, p->buf + from, want);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                from += (uint64_t)n;
                pthread_mutex_lock(&p->lock);
                p->filled[idx] += (uint32_t)n;
                p->fetched += (uint64_t)n;
                stop = p->stop;
                pthread_cond_broadcast(&p->cond);
                pthread_mutex_unlock(&p->lock);
                if (stop || par_cancelled(p)) {
                    ok = false;
                    break;
                }
            }
        }
        pthread_mutex_lock(&p->lock);
        p->reqs[w] = -1;
        pthread_mutex_unlock(&p->lock);
        sceHttpDeleteRequest(req);
    }
    if (conn >= 0)
        sceHttpDeleteConnection(conn);
    free(host_header);
    return ok;
}

struct par_worker {
    parallel *p;
    int w;
};

static void *par_run(void *arg)
{
    struct par_worker *pw = arg;
    parallel *p = pw->p;
    int w = pw->w;
    free(pw);
    for (;;) {
        pthread_mutex_lock(&p->lock);
        if (p->stop || p->next >= p->chunks) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        unsigned idx = p->next++;
        pthread_mutex_unlock(&p->lock);
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok && !p->stop && !par_cancelled(p); tries++)
            ok = par_fetch(p, w, idx);
        if (!ok) {
            pthread_mutex_lock(&p->lock);
            p->failed[idx] = true;
            pthread_cond_broadcast(&p->cond);
            pthread_mutex_unlock(&p->lock);
        }
    }
    return NULL;
}

static void par_free(parallel *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->stop = true;
    for (int w = 0; w < PAR_WORKERS; w++)
        if (p->reqs[w] >= 0)
            sceHttpAbortRequest(p->reqs[w]);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    free(p->buf);
    free(p->filled);
    free(p->failed);
    free(p->url);
    free(p->ua);
    free(p->ref);
    free(p);
}

static parallel *par_start(stream_t *access, uint64_t size, long serial)
{
    parallel *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->size = size;
    p->chunks = (unsigned)((size + PAR_CHUNK - 1) / PAR_CHUNK);
    p->buf = malloc(size);
    p->filled = calloc(p->chunks, sizeof(*p->filled));
    p->failed = calloc(p->chunks, sizeof(*p->failed));
    p->url = strdup(access->psz_url);
    p->ua = var_InheritString(access, "http-user-agent");
    p->ref = var_InheritString(access, "http-referrer");
    p->serial = serial;
    for (int w = 0; w < PAR_WORKERS; w++)
        p->reqs[w] = -1;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    if (!p->buf || !p->filled || !p->failed || !p->url) {
        par_free(p);
        return NULL;
    }
    int workers = p->chunks < PAR_WORKERS ? (int)p->chunks : PAR_WORKERS;
    for (int w = 0; w < workers; w++) {
        struct par_worker *pw = malloc(sizeof(*pw));
        if (!pw)
            break;
        pw->p = p;
        pw->w = w;
        if (pthread_create(&p->threads[p->nthreads], NULL, par_run, pw) != 0) {
            free(pw);
            break;
        }
        p->nthreads++;
    }
    if (!p->nthreads) {
        par_free(p);
        return NULL;
    }
    return p;
}

/* VLC's read from the pieces: what's in at `pos`, waiting for it if needed. */
static ssize_t par_read(stream_t *access, ps5http_sys *sys, void *buf, size_t len)
{
    parallel *p = sys->par;
    if (sys->pos >= p->size)
        return 0;
    unsigned idx = (unsigned)(sys->pos / PAR_CHUNK);
    uint64_t within = sys->pos % PAR_CHUNK;
    pthread_mutex_lock(&p->lock);
    while (p->filled[idx] <= within && !p->failed[idx] && !p->stop) {
        pthread_mutex_unlock(&p->lock);
        if (vlc_killed() || cancelled(sys))
            return 0;
        pthread_mutex_lock(&p->lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        if (p->filled[idx] <= within && !p->failed[idx])
            pthread_cond_timedwait(&p->cond, &p->lock, &ts);
    }
    size_t have = p->filled[idx] > within ? (size_t)(p->filled[idx] - within) : 0;
    bool failed = !have && p->failed[idx];
    pthread_mutex_unlock(&p->lock);
    if (failed) {
        PLOG("a piece at %" PRIu64 " couldn't be fetched", sys->pos);
        return 0;
    }
    if (len > have)
        len = have;
    memcpy(buf, p->buf + sys->pos, len);
    sys->pos += len;
    sys->read_bytes += len;
    (void)access;
    return (ssize_t)len;
}

/* Sends a GET from `offset`; fills status, size, type, redirect. */
static int send_request(stream_t *access, uint64_t offset)
{
    ps5http_sys *sys = access->p_sys;
    drop_request(sys);
    free(sys->redirect);
    sys->redirect = NULL;
    sys->eof = false;

    char *host_header = NULL;
    char *direct = direct_url(access->psz_url, &host_header);
    const char *url = direct ? direct : access->psz_url;
    sys->conn = sceHttpCreateConnectionWithURL(http_template, url, 1);
    if (sys->conn < 0) {
        PLOG("connection failed (%#x)", (unsigned)sys->conn);
        free(direct);
        free(host_header);
        return VLC_EGENERIC;
    }
    sys->req = sceHttpCreateRequestWithURL(sys->conn, METHOD_GET, url, 0);
    free(direct);
    if (sys->req < 0) {
        PLOG("request failed (%#x)", (unsigned)sys->req);
        free(host_header);
        drop_request(sys);
        return VLC_EGENERIC;
    }
    if (host_header)
        sceHttpAddRequestHeader(sys->req, "Host", host_header, HEADER_OVERWRITE);
    char *ua = var_InheritString(access, "http-user-agent");
    char *ref = var_InheritString(access, "http-referrer");
    if (ua && *ua)
        sceHttpAddRequestHeader(sys->req, "User-Agent", ua, HEADER_OVERWRITE);
    if (ref && *ref)
        sceHttpAddRequestHeader(sys->req, "Referer", ref, HEADER_OVERWRITE);
    free(ua);
    free(ref);
    sceHttpAddRequestHeader(sys->req, "Accept", "*/*", HEADER_OVERWRITE);
    /* never a cached copy: a live playlist read again came back the same
     * (BBC, 2026-10-13: 20 s on, the same ten segments), VLC ran out of
     * segments and the channel stopped */
    sceHttpAddRequestHeader(sys->req, "Cache-Control", "no-cache", HEADER_OVERWRITE);
    sceHttpAddRequestHeader(sys->req, "Pragma", "no-cache", HEADER_OVERWRITE);
    if (offset > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", offset);
        sceHttpAddRequestHeader(sys->req, "Range", range, HEADER_OVERWRITE);
    }

    vlc_interrupt_register(abort_on_interrupt, &sys->req);
    int r = sceHttpSendRequest(sys->req, NULL, 0);
    vlc_interrupt_unregister();
    if (r < 0) {
        /* 0x8095xxxx: the console's SSL turned this one server down (its
         * certificate, its ciphers). VLC's own http takes this link; the
         * service stays for the others. Anything else: the service itself. */
        bool ssl = ((unsigned)r & 0xFFFF0000u) == 0x80950000u;
        /* 0x8043xxxx: this server's connection (down, refused, timed out):
         * this link only too. Taken as the service's, one dead channel sent
         * every stream after it to VLC's own http (2026-10-13). */
        bool net = ((unsigned)r & 0xFFFF0000u) == 0x80430000u ||
                   ((unsigned)r & 0xFFFF0000u) == 0x80410000u; /* 0x8041: the network's errno */
        /* stopped on purpose (the player left, a search was typed over):
         * nothing wrong with anything */
        bool stopped = vlc_killed() || cancelled(sys);
        int in_row = stopped || ssl || net ? 0 : atomic_fetch_add(&service_failures, 1) + 1;
        PLOG("sending failed (%#x): %s", (unsigned)r,
             stopped ? "stopped"
             : ssl   ? "SSL refused this server, VLC's own http takes this link"
             : net   ? "connection failed, VLC's own http tries this link"
             : in_row >= 3 ? "third time in a row: VLC's own http is used from now on"
                           : "VLC's own http takes this link");
        if (in_row >= 3)
            service_refused = true;
        if (ssl)
            refuse_host(access->psz_url, true);
        /* https by name refused (the console's DNS): this one's https links
         * straight to VLC's own http from now on, no failed try first */
        if (net && !strncasecmp(access->psz_url, "https://", 8) &&
            ((unsigned)r & 0xFFFF0000u) == 0x80410000u && atomic_fetch_add(&https_name_refusals, 1) + 1 == 3) {
            vlc_mutex_lock(&refused_lock);
            https_refused = true;
            vlc_mutex_unlock(&refused_lock);
            PLOG("https refused 3 times by name (the console's DNS?): VLC's own http takes every https link");
        }
        free(host_header);
        drop_request(sys);
        return VLC_EGENERIC;
    }
    atomic_store(&service_failures, 0);
    int status = 0;
    sceHttpGetStatusCode(sys->req, &status);
    sys->status = status;
    if (host_header && (status == 400 || status == 403 || status == 404 || status == 421)) {
        /* the server wanted its name, not just its address (the Host header
         * didn't get through): no more address-only requests this session */
        PLOG("HTTP %d to %s by address: VLC's own http takes it, no more by address", status, host_header);
        atomic_store(&direct_broken, true);
        free(host_header);
        drop_request(sys);
        return VLC_EGENERIC;
    }
    if (host_header && !sys->logged_direct) {
        sys->logged_direct = true;
        PLOG("%s by the app's own name lookup", host_header);
    }
    free(host_header);
    host_header = NULL;

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
    if (sys->par)
        return par_read(access, sys, buf, len);
    if (sys->eof || sys->req < 0)
        return 0;
    if (cancelled(sys)) {
        /* the player left this video: no waiting for the rest of a slow
         * server's segment (the next video waited 10 s, 2026-10-18) */
        if (!sys->cancel_logged)
            PLOG("stopped by the player: connection ended");
        sys->cancel_logged = true;
        sys->eof = true;
        return 0;
    }
    /* A little at a time: VLC's prefetch asks for 16 MiB at the start, and a
     * live channel (no size, 4-8 Mbit/s) took 15-33 s to fill that before the
     * first picture (issue #11's log). */
    size_t most = sys->size && sys->can_seek ? ((size_t)1 << 20) : ((size_t)64 << 10);
    if (len > most)
        len = most;
    vlc_interrupt_register(abort_on_interrupt, &sys->req);
    mtime_t t0 = mdate();
    int n = sceHttpReadData(sys->req, buf, len);
    sys->read_wait += mdate() - t0;
    vlc_interrupt_unregister();
    if (n > 0)
        sys->read_bytes += (uint64_t)n;
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
    if (sys->par) {
        sys->pos = offset < sys->size ? offset : sys->size;
        return VLC_SUCCESS;
    }
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
    /* each download's speed: a live channel's segments must come faster than
     * they play (a 4 s piece in under 4 s), or it freezes */
    if (sys->read_bytes >= (256u << 10) && vlc_ps5_log_details && vlc_ps5_log_details()) {
        double secs = (mdate() - sys->opened_at) / 1e6, wait = sys->read_wait / 1e6;
        if (sys->par)
            PLOG("read %" PRIu64 " KiB in %.2f s (%.0f KiB/s; %d connections)", sys->read_bytes >> 10, secs,
                 secs > 0 ? (sys->read_bytes >> 10) / secs : 0.0, sys->par->nthreads);
        else
            PLOG("read %" PRIu64 " KiB in %.2f s (%.0f KiB/s; %.2f s waiting for the network)",
                 sys->read_bytes >> 10, secs, secs > 0 ? (sys->read_bytes >> 10) / secs : 0.0, wait);
    }
    par_free(sys->par);
    sys->par = NULL;
    drop_request(sys);
    free(sys->content_type);
    free(sys->redirect);
    free(sys);
}

static int Open(vlc_object_t *obj)
{
    stream_t *access = (stream_t *)obj;
    pthread_once(&service_once, start_service);
    if (http_template < 0 || service_refused || host_refused(access->psz_url))
        return VLC_EGENERIC; /* VLC's own http module takes over */

    ps5http_sys *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    sys->conn = sys->req = -1;
    access->p_sys = sys;
    sys->serial = (long)var_InheritInteger(access, "ps5http-serial");
    sys->opened_at = mdate();
    if (cancelled(sys)) {
        free(sys);
        access->p_sys = NULL;
        return VLC_EGENERIC; /* a segment for a video already stopped */
    }
    PLOG("open %s", access->psz_url);

    if (send_request(access, 0) != VLC_SUCCESS)
        goto error;
    if (sys->redirect) {
        /* VLC opens the new address (and probes the modules again). */
        char *to = vlc_uri_resolve(access->psz_url, sys->redirect);
        if (!to)
            to = strdup(sys->redirect);
        if (!to)
            goto error;
        PLOG("redirected (%d) to %s", sys->status, to);
        /* Not freed here: access_New() keeps the old address (to spot a
         * loop) and frees it itself. Freeing it too was a double free. */
        access->psz_url = to;
        Close(obj);
        access->p_sys = NULL;
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
    if (sys->status == 200 && sys->can_seek && sys->size >= PAR_MIN && sys->size <= PAR_MAX) {
        sys->par = par_start(access, sys->size, sys->serial);
        if (sys->par)
            drop_request(sys); /* the pieces have their own connections */
    }
    access->pf_read = Read;
    access->pf_block = NULL;
    access->pf_seek = Seek;
    access->pf_control = Control;
    return VLC_SUCCESS;

error:
    Close(obj);
    access->p_sys = NULL; /* the next module (VLC's own http) starts clean */
    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname("PS5 HTTP")
    set_description("HTTP/HTTPS through the console's network service")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access", 300)
    add_shortcut("http", "https")
    /* the player's number for the open (see vlc_ps5_stream_cancelled) */
    add_integer("ps5http-serial", 0, "Player open", NULL, true)
        change_private()
        change_safe()
    set_callbacks(Open, Close)
vlc_module_end()
