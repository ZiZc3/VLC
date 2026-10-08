/*
 * libc functions the SDK's stubs name but the console leaves unresolved for a
 * title: its GOT slot stays 0, and a call jumps there. xemu found them at run
 * time (xemu_ps5_early_init's import report); the link binds each standard
 * name to its xemu_ps5_ version here (ps5/build-title.sh --defsym), as
 * PS5_Vulkan's radv-link.sh binds others to the SDK platform's ps5_ ones. The
 * prefix keeps the title from exporting libc's names, which the title
 * converter refuses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Temporary files: an exclusive create on a random name, as POSIX has it. */
int xemu_ps5_mkstemp(char *template)
{
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static uint64_t state;
    state ^= (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^
             (uint64_t)(uintptr_t)template;
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = 0; i < 6; i++) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            template[len - 6 + i] = chars[(state >> 33) % (sizeof(chars) - 1)];
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

/* The PS5 has no terminals. */
int xemu_ps5_isatty(int fd)
{
    (void)fd;
    errno = ENOTTY;
    return 0;
}

/* VLC-PS5: named by the SDK's stubs, left unresolved by the console (the first
 * console run's import report). VLC calls it to guess every media item's type. */
static int lower_ascii(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

char *xemu_ps5_strcasestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && lower_ascii((unsigned char)*h) == lower_ascii((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

/* VLC-PS5: readv / writev through plain read / write. On the console readv
 * failed every VLC file read with ENOMEM (the third console run: "read error:
 * Cannot allocate memory"), while read() works (fonts, thumbnails cache). Big
 * requests (VLC's prefetch asks 16 MiB at once) go in chunks of at most 1 MiB,
 * halved if the kernel still answers ENOMEM. A short read ends the call, as
 * readv does for pipes and sockets. */
#define IO_CHUNK_MAX (1u << 20)
#define IO_CHUNK_MIN (16u << 10)

static size_t io_chunk = IO_CHUNK_MAX;

ssize_t xemu_ps5_readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        char *p = (char *)iov[i].iov_base;
        size_t left = iov[i].iov_len;
        while (left > 0) {
            size_t want = left < io_chunk ? left : io_chunk;
            ssize_t n = read(fd, p, want);
            if (n < 0) {
                if (errno == ENOMEM && io_chunk > IO_CHUNK_MIN) {
                    io_chunk /= 2;
                    continue;
                }
                if (errno == EINTR)
                    continue;
                return total > 0 ? total : -1;
            }
            total += n;
            if ((size_t)n < want)
                return total; /* end of file, or all a pipe/socket had */
            p += n;
            left -= (size_t)n;
        }
    }
    return total;
}

ssize_t xemu_ps5_writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        const char *p = (const char *)iov[i].iov_base;
        size_t left = iov[i].iov_len;
        while (left > 0) {
            size_t want = left < io_chunk ? left : io_chunk;
            ssize_t n = write(fd, p, want);
            if (n < 0) {
                if (errno == ENOMEM && io_chunk > IO_CHUNK_MIN) {
                    io_chunk /= 2;
                    continue;
                }
                if (errno == EINTR)
                    continue;
                return total > 0 ? total : -1;
            }
            total += n;
            if ((size_t)n < want)
                return total;
            p += n;
            left -= (size_t)n;
        }
    }
    return total;
}

/* No per-process creation mask to speak of: report the usual one. */
mode_t xemu_ps5_umask(mode_t mask)
{
    static mode_t current = 022;
    mode_t old = current;
    current = mask & 0777;
    return old;
}

long xemu_ps5_pathconf(const char *path, int name)
{
    (void)path;
    switch (name) {
    case _PC_NAME_MAX:
        return 255;
    case _PC_PATH_MAX:
        return 1024;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* Shell-style matching: *, ? and [...] (with ! or ^), and \ escapes. */
static bool match_bracket(const char **pattern, char c)
{
    const char *p = *pattern + 1;
    bool negate = *p == '!' || *p == '^';
    if (negate) {
        p++;
    }
    bool matched = false;
    bool first = true;
    while (*p && (first || *p != ']')) {
        first = false;
        char lo = *p++;
        char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            hi = p[1];
            p += 2;
        }
        if (c >= lo && c <= hi) {
            matched = true;
        }
    }
    *pattern = *p ? p + 1 : p;
    return matched != negate;
}

int xemu_ps5_fnmatch(const char *pattern, const char *string, int flags)
{
    (void)flags;
    while (*pattern) {
        switch (*pattern) {
        case '*':
            while (*pattern == '*') {
                pattern++;
            }
            if (!*pattern) {
                return 0;
            }
            for (; *string; string++) {
                if (xemu_ps5_fnmatch(pattern, string, flags) == 0) {
                    return 0;
                }
            }
            return FNM_NOMATCH;
        case '?':
            if (!*string) {
                return FNM_NOMATCH;
            }
            pattern++;
            string++;
            break;
        case '[':
            if (!*string || !match_bracket(&pattern, *string)) {
                return FNM_NOMATCH;
            }
            string++;
            break;
        case '\\':
            if (pattern[1]) {
                pattern++;
            }
            /* fall through */
        default:
            if (*pattern != *string) {
                return FNM_NOMATCH;
            }
            pattern++;
            string++;
        }
    }
    return *string ? FNM_NOMATCH : 0;
}

/* Name lookups: numeric addresses only (QEMU prints its own sockets). */
int xemu_ps5_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                         char *host, socklen_t hostlen, char *serv,
                         socklen_t servlen, int flags)
{
    (void)flags;
    const void *addr;
    int port;
    if (sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in)) {
        addr = &((const struct sockaddr_in *)sa)->sin_addr;
        port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
    } else if (sa->sa_family == AF_INET6 &&
               salen >= sizeof(struct sockaddr_in6)) {
        addr = &((const struct sockaddr_in6 *)sa)->sin6_addr;
        port = ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (host && hostlen && !inet_ntop(sa->sa_family, addr, host, hostlen)) {
        return EAI_OVERFLOW;
    }
    if (serv && servlen &&
        snprintf(serv, servlen, "%d", port) >= (int)servlen) {
        return EAI_OVERFLOW;
    }
    return 0;
}

const char *xemu_ps5_gai_strerror(int error)
{
    static char buf[48];
    snprintf(buf, sizeof(buf), "name resolution error %d", error);
    return buf;
}

struct hostent *xemu_ps5_gethostbyname(const char *name)
{
    (void)name; /* No h_errno on the PS5 either */
    return NULL;
}

/* Features the PS5 doesn't have. */
pid_t xemu_ps5_fork(void) { errno = ENOSYS; return -1; }
pid_t xemu_ps5_vfork(void) { errno = ENOSYS; return -1; }
pid_t xemu_ps5_setsid(void) { errno = ENOSYS; return -1; }
int xemu_ps5_chroot(const char *path) { (void)path; errno = ENOSYS; return -1; }
int xemu_ps5_symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;
    errno = ENOSYS;
    return -1;
}
int xemu_ps5_link(const char *target, const char *path)
{
    (void)target;
    (void)path;
    errno = ENOSYS;
    return -1;
}
ssize_t xemu_ps5_readlink(const char *path, char *buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL; /* "not a symbolic link": there are none */
    return -1;
}
/* The mounted filesystems: libdvdread and libbluray ask which one holds a disc
 * drive. The title has no disc drive of its own (ISO files and folders don't
 * ask), so: none. */
struct statfs;
int xemu_ps5_getfsstat(struct statfs *buf, long size, int mode)
{
    (void)buf;
    (void)size;
    (void)mode;
    return 0;
}
