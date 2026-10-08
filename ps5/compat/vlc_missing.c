/*
 * libc functions libvlccore calls that no PS5 system module exports for a
 * title. Found all at once by the title link (ps5/build-title.sh), not one
 * crash at a time. None of these names is in the SDK's stub libraries, so
 * defining them here exports nothing (the link hides libraries' symbols).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define __RUNETYPE_INTERNAL 1 /* plain __getCurrentRuneLocale() declaration */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <runetype.h>
#include <search.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <unistd.h>

/* ---- ctype: FreeBSD's inline isalpha()/tolower()/... read a rune locale ----
 * The app runs in the C locale, so one table built from ASCII rules serves,
 * with no data imported from a system module. */

static _RuneLocale c_locale;
static int c_locale_ready;

static void c_locale_init(void)
{
    for (int c = 0; c < _CACHED_RUNES; c++) {
        unsigned long t = 0;
        int upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z';
        int digit = c >= '0' && c <= '9';
        if (c < 0x20 || c == 0x7f)
            t |= _CTYPE_C;
        if (c == ' ' || (c >= '\t' && c <= '\r'))
            t |= _CTYPE_S;
        if (c == ' ' || c == '\t')
            t |= _CTYPE_B;
        if (upper)
            t |= _CTYPE_U | _CTYPE_A;
        if (lower)
            t |= _CTYPE_L | _CTYPE_A;
        if (digit)
            t |= _CTYPE_D | _CTYPE_N;
        if (digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            t |= _CTYPE_X;
        if (c > 0x20 && c < 0x7f)
            t |= _CTYPE_G | _CTYPE_R;
        if (c == ' ')
            t |= _CTYPE_R;
        if (c > 0x20 && c < 0x7f && !upper && !lower && !digit)
            t |= _CTYPE_P;
        if (c >= 0x20 && c < 0x7f)
            t |= _CTYPE_SW1;
        c_locale.__runetype[c] = t;
        c_locale.__maplower[c] = upper ? c + 32 : c;
        c_locale.__mapupper[c] = lower ? c - 32 : c;
    }
    c_locale_ready = 1;
}

const _RuneLocale *__getCurrentRuneLocale(void)
{
    if (!c_locale_ready) /* idempotent: a racing second init writes the same values */
        c_locale_init();
    return &c_locale;
}

/* Runes past the table: none are classified in the C locale. */
unsigned long ___runetype(__ct_rune_t c)
{
    (void)c;
    return 0;
}

__ct_rune_t ___tolower(__ct_rune_t c)
{
    return c;
}

__ct_rune_t ___toupper(__ct_rune_t c)
{
    return c;
}

/* ---- small ones ---- */

int ffsll(long long mask)
{
    return __builtin_ffsll(mask);
}

int mkostemp(char *template, int flags)
{
    int fd = mkstemp(template);
    if (fd >= 0 && (flags & O_CLOEXEC))
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* A read-ahead hint (the filesystem access plugin): advice only, so "done". */
int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}

/* SysV shared memory blocks (block_shm_Alloc) are an X11 path; never used here. */
int shmdt(const void *addr)
{
    (void)addr;
    errno = ENOSYS;
    return -1;
}

/* FreeBSD's stdio macros (getc, putc, fileno, feof, ...) read the FILE struct
 * directly unless __isthreaded is set, and the SDK's FILE layout isn't the
 * console libc's: a garbage pointer (VLC's console logger crashed on it). The
 * title link binds __isthreaded here (ps5/build-title.sh): always the real
 * functions. */
int vlc_ps5_isthreaded = 1;

/* libdvdread asks the fstab which device a mount point is (a DVD drive). A
 * title has no fstab and no drives: never found, the path is read as it is
 * (an ISO or a VIDEO_TS folder). */
struct fstab;
struct fstab *getfsfile(const char *name)
{
    (void)name;
    return 0;
}
