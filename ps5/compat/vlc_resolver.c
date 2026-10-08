/*
 * getaddrinfo/freeaddrinfo for a PS5 title.
 *
 * The console routes a title's getaddrinfo to a module titles don't load; the
 * platform layer's stand-in refuses every lookup (EAI_FAIL), numeric addresses
 * included, so VLC couldn't reach any server. This one answers numeric
 * addresses itself, asks the local network for "name.local" (mDNS), and sends
 * other names to DNS over UDP: the router (a.b.c.1 of our own address), then
 * 1.1.1.1, then 8.8.8.8. Answers are kept for a minute.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- DNS over UDP ----------------------------------------------------------- */

static size_t put_name(uint8_t *p, size_t room, const char *name)
{
    size_t n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t len = dot ? (size_t)(dot - name) : strlen(name);
        if (len == 0 || len > 63 || n + len + 2 > room)
            return 0;
        p[n++] = (uint8_t)len;
        memcpy(p + n, name, len);
        n += len;
        name += len + (dot ? 1 : 0);
    }
    p[n++] = 0;
    return n;
}

/* Past a (possibly compressed) name; 0 if it runs off the end. */
static size_t skip_name(const uint8_t *msg, size_t len, size_t at)
{
    while (at < len) {
        uint8_t l = msg[at];
        if ((l & 0xC0) == 0xC0)
            return at + 2 <= len ? at + 2 : 0;
        if (l == 0)
            return at + 1;
        at += 1 + l;
    }
    return 0;
}

static bool ask(uint32_t server, uint16_t port, const char *name, bool mdns, struct in_addr *out)
{
    uint8_t q[512];
    uint16_t id = (uint16_t)(rand() & 0xFFFF);
    memset(q, 0, 12);
    q[0] = id >> 8;
    q[1] = id & 0xFF;
    q[2] = mdns ? 0x00 : 0x01; /* recursion desired (DNS) */
    q[5] = 1;                  /* one question */
    size_t n = put_name(q + 12, sizeof(q) - 16, name);
    if (!n)
        return false;
    n += 12;
    q[n++] = 0;
    q[n++] = 1;                    /* A */
    q[n++] = mdns ? 0x80 : 0x00;   /* mDNS: unicast answer please */
    q[n++] = 1;                    /* IN */

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = server;
    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        if (sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
            break;
        struct pollfd p = { s, POLLIN, 0 };
        while (!found && poll(&p, 1, mdns ? 800 : 1200) > 0) {
            uint8_t r[1500];
            ssize_t got = recvfrom(s, r, sizeof(r), 0, NULL, NULL);
            if (got < 12)
                break;
            if (!mdns && (r[0] != q[0] || r[1] != q[1]))
                continue; /* not our answer */
            unsigned qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
            size_t at = 12;
            for (unsigned i = 0; i < qd && at; i++) {
                at = skip_name(r, (size_t)got, at);
                if (at)
                    at += 4;
            }
            for (unsigned i = 0; i < an && at && at < (size_t)got; i++) {
                at = skip_name(r, (size_t)got, at);
                if (!at || at + 10 > (size_t)got)
                    break;
                unsigned type = (r[at] << 8) | r[at + 1];
                unsigned rdlen = (r[at + 8] << 8) | r[at + 9];
                at += 10;
                if (at + rdlen > (size_t)got)
                    break;
                if (type == 1 && rdlen == 4) {
                    memcpy(&out->s_addr, r + at, 4);
                    found = true;
                    break;
                }
                at += rdlen;
            }
        }
    }
    close(s);
    return found;
}

/* Our address on the network (a UDP "connect" picks the interface; nothing is sent). */
static uint32_t local_address(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 0;
    struct sockaddr_in to, me;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x01010101);
    socklen_t len = sizeof(me);
    uint32_t a = 0;
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 &&
        getsockname(s, (struct sockaddr *)&me, &len) == 0)
        a = me.sin_addr.s_addr;
    close(s);
    return a;
}

/* ---- a small cache ------------------------------------------------------------ */

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[128];
    struct in_addr addr;
    time_t until;
} cache[16];

static bool lookup(const char *name, struct in_addr *out)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < 16; i++)
        if (cache[i].until > now && !strcasecmp(cache[i].name, name)) {
            *out = cache[i].addr;
            pthread_mutex_unlock(&cache_lock);
            return true;
        }
    pthread_mutex_unlock(&cache_lock);

    size_t len = strlen(name);
    bool local = len > 6 && !strcasecmp(name + len - 6, ".local");
    bool ok = false;
    if (local)
        ok = ask(htonl(0xE00000FB), 5353, name, true, out); /* 224.0.0.251 */
    if (!ok) {
        uint32_t me = ntohl(local_address());
        uint32_t servers[3] = { me ? htonl((me & 0xFFFFFF00u) | 1) : 0, htonl(0x01010101), htonl(0x08080808) };
        for (int i = 0; i < 3 && !ok; i++)
            if (servers[i])
                ok = ask(servers[i], 53, name, false, out);
    }
    fprintf(stderr, "resolver: %s -> %s\n", name, ok ? inet_ntoa(*out) : "not found");
    if (ok) {
        pthread_mutex_lock(&cache_lock);
        int slot = 0;
        for (int i = 1; i < 16; i++)
            if (cache[i].until < cache[slot].until)
                slot = i;
        snprintf(cache[slot].name, sizeof(cache[slot].name), "%s", name);
        cache[slot].addr = *out;
        cache[slot].until = now + 60;
        pthread_mutex_unlock(&cache_lock);
    }
    return ok;
}

/* ---- getaddrinfo ---------------------------------------------------------------- */

static int service_port(const char *service, int *port)
{
    if (!service || !*service) {
        *port = 0;
        return 0;
    }
    char *end;
    long p = strtol(service, &end, 10);
    if (*end == '\0' && p >= 0 && p <= 65535) {
        *port = (int)p;
        return 0;
    }
    static const struct { const char *name; int port; } known[] = {
        { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "rtsp", 554 }, { "smb", 445 },
        { "microsoft-ds", 445 }, { "domain", 53 }, { "mms", 1755 },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcasecmp(service, known[i].name)) {
            *port = known[i].port;
            return 0;
        }
    return EAI_SERVICE;
}

/* One addrinfo with its address and name in the same allocation. */
static struct addrinfo *make(int family, const void *addr, int port, const struct addrinfo *hints,
                             const char *canon)
{
    size_t salen = family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    size_t clen = canon ? strlen(canon) + 1 : 0;
    struct addrinfo *ai = calloc(1, sizeof(*ai) + salen + clen);
    if (!ai)
        return NULL;
    struct sockaddr *sa = (struct sockaddr *)(ai + 1);
    if (family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)sa;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)port);
        memcpy(&s6->sin6_addr, addr, 16);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)sa;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)port);
        memcpy(&s4->sin_addr, addr, 4);
    }
#ifdef __FreeBSD__
    sa->sa_len = (uint8_t)salen;
#endif
    ai->ai_family = family;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol
                    : ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    ai->ai_addrlen = (socklen_t)salen;
    ai->ai_addr = sa;
    if (canon) {
        ai->ai_canonname = (char *)sa + salen;
        memcpy(ai->ai_canonname, canon, clen);
    }
    return ai;
}

int vlcps5_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **res)
{
    if (!res)
        return EAI_FAIL;
    *res = NULL;
    int port, err = service_port(service, &port);
    if (err)
        return err;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int flags = hints ? hints->ai_flags : 0;
    const char *canon = (flags & AI_CANONNAME) ? node : NULL;

    unsigned char a6[16];
    struct in_addr a4;
    if (!node || !*node) {
        /* bind to everything, or talk to ourselves */
        if (family == AF_INET6) {
            memset(a6, 0, 16);
            if (!(flags & AI_PASSIVE))
                a6[15] = 1;
            *res = make(AF_INET6, a6, port, hints, NULL);
        } else {
            a4.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
            *res = make(AF_INET, &a4, port, hints, NULL);
        }
        return *res ? 0 : EAI_MEMORY;
    }
    if (family != AF_INET6 && inet_pton(AF_INET, node, &a4) == 1) {
        *res = make(AF_INET, &a4, port, hints, canon);
        return *res ? 0 : EAI_MEMORY;
    }
    if (family != AF_INET && inet_pton(AF_INET6, node, a6) == 1) {
        *res = make(AF_INET6, a6, port, hints, canon);
        return *res ? 0 : EAI_MEMORY;
    }
    if (flags & AI_NUMERICHOST)
        return EAI_NONAME;
    if (family == AF_INET6)
        return EAI_FAMILY; /* names resolve to IPv4 here */
    if (!strcasecmp(node, "localhost")) {
        a4.s_addr = htonl(INADDR_LOOPBACK);
    } else if (!lookup(node, &a4)) {
        return EAI_NONAME;
    }
    *res = make(AF_INET, &a4, port, hints, canon);
    return *res ? 0 : EAI_MEMORY;
}

void vlcps5_freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai);
        ai = next;
    }
}
