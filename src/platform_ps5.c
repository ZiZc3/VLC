/*
 * VLC-PS5 on the console. No jailbreak and no whitelist: everything lives in
 * the title's own folder, /app0, which a title can always write (as QUICK3 and
 * PS5_vkQuake do). Pieces from XPSemu (crash report, import report), QUICK3
 * (display mode, DualSense samples, audio port) and PS5SX2 (context layout).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APP_DIR "/app0"
#define LOG_PATH APP_DIR "/vlc-ps5.log"
/* Where the eboot is loaded, and a bound on its code for backtraces. */
#define EBOOT_BASE 0x400000ULL
#define EBOOT_END 0x10000000ULL

int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceSystemServiceParamGetInt(int id, int *value);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadRead(int32_t handle, void *samples, int32_t capacity);
int scePadClose(int32_t handle);

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                    uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *buf);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                  const char *name);

/* ---- start-up ---------------------------------------------------------- */

/* The app's folder. "/app0" inside the sandbox; once a jailbreak daemon lifts
 * the sandbox, paths start at the console's real root, where the same folder
 * has another name (see find_app_dir). */
static char app_dir[256] = APP_DIR;
static bool full_access;

void plat_notify(const char *message)
{
    /* The kernel's 0xc30-byte request with the text at 0x2d (PS5SX2). */
    static uint8_t request[0xc30];
    memset(request, 0, sizeof(request));
    snprintf((char *)request + 0x2d, 1024, "%s", message);
    sceKernelSendNotificationRequest(0, request, sizeof(request), 0);
}

static int crash_fd = -1, crash_file_fd = -1;

static void write_str(const char *s)
{
    /* Its own descriptors, opened at start-up: the first console crash's report
     * never reached the log through descriptor 2. */
    if (crash_fd >= 0)
        (void)!write(crash_fd, s, strlen(s));
    if (crash_file_fd >= 0)
        (void)!write(crash_file_fd, s, strlen(s));
}

/* The PS5's context is FreeBSD's with the machine context 0x30 bytes further
 * on: rbp at +0x88, rip at +0xe0, rsp at +0xf8. Look addresses up with
 * llvm-addr2line -f -C -e builds/llvm-pie-<sha>.elf 0x<eboot+>. */
static void crash_handler(int sig, siginfo_t *info, void *context)
{
    static volatile sig_atomic_t in_handler;
    char buf[200];
    if (in_handler)
        _exit(128 + sig);
    in_handler = 1;
    char crash_path[300];
    snprintf(crash_path, sizeof(crash_path), "%s/vlc-crash.txt", app_dir);
    crash_file_fd = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    const uint64_t *ctx = (const uint64_t *)context;
    uint64_t rip = ctx[0xe0 / 8], rsp = ctx[0xf8 / 8];
    snprintf(buf, sizeof(buf),
             "\n*** VLC-PS5 crashed: signal %d, fault address %p, thread %p\n"
             "*** rip %#lx (eboot+%#lx) rsp %#lx\n",
             sig, info->si_addr, (void *)pthread_self(), (unsigned long)rip,
             (unsigned long)(rip - EBOOT_BASE), (unsigned long)rsp);
    write_str(buf);
    char toast[128];
    snprintf(toast, sizeof(toast), "VLC-PS5 crashed: signal %d at eboot+%#lx", sig,
             (unsigned long)(rip - EBOOT_BASE));
    plat_notify(toast);
    if (rsp >= 0x100000 && (rsp & 7) == 0) {
        const uint64_t *sp = (const uint64_t *)rsp;
        for (int i = 0, found = 0; i < 256 && found < 24; i++) {
            if (sp[i] >= EBOOT_BASE && sp[i] < EBOOT_END) {
                snprintf(buf, sizeof(buf), "***   stack[%d] eboot+%#lx\n", i,
                         (unsigned long)(sp[i] - EBOOT_BASE));
                write_str(buf);
                found++;
            }
        }
    }
    /* The log lines still in the buffer (the last 200 ms), after the report:
     * the report is already safe if this blocks (a crash inside stdio). */
    write_str("*** log lines still buffered at the crash:\n");
    fflush(stderr);
    if (crash_fd >= 0)
        fsync(crash_fd);
    if (crash_file_fd >= 0)
        fsync(crash_file_fd);
    _exit(128 + sig);
}

/* " name " of each import left unresolved: optional calls check it first. */
static char unresolved[2048] = " ";

static bool import_resolved(const char *name)
{
    char key[160];
    snprintf(key, sizeof(key), " %s ", name);
    return strstr(unresolved, key) == NULL;
}

/* Every import the console left unresolved (a call would jump to 0), in one
 * report: imports.txt lists each GOT slot's offset and symbol. */
static void report_unresolved_imports(void)
{
    FILE *f = fopen(APP_DIR "/imports.txt", "r");
    if (!f) {
        fprintf(stderr, "VLC-PS5: can't read imports.txt, errno %d\n", errno);
        return;
    }
    char name[128], list[400] = "";
    unsigned long offset;
    int checked = 0, missing = 0;
    while (fscanf(f, "%lx %127s", &offset, name) == 2) {
        checked++;
        if (*(const uint64_t *)(EBOOT_BASE + offset) == 0) {
            fprintf(stderr, "VLC-PS5: unresolved import: %s\n", name);
            if (strlen(unresolved) + strlen(name) + 2 < sizeof(unresolved)) {
                strcat(unresolved, name);
                strcat(unresolved, " ");
            }
            if (missing++ < 10)
                snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s",
                         missing > 1 ? " " : "", name);
        }
    }
    fclose(f);
    fprintf(stderr, "VLC-PS5: %d of %d imports unresolved\n", missing, checked);
    if (missing) {
        char toast[512];
        snprintf(toast, sizeof(toast), "VLC-PS5: %d unresolved imports: %s", missing, list);
        plat_notify(toast);
    }
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* After the sandbox is lifted: where our folder is now. /app0 may still
 * resolve; otherwise the sandbox's own copy of it (/mnt/sandbox/<title>_NNN/app0,
 * the folder the daemon found our request in) or the folder
 * ShadowMountPlus launched us from. */
static void find_app_dir(void)
{
    if (exists(APP_DIR "/eboot.bin"))
        return;
    char path[256];
    DIR *d = opendir("/mnt/sandbox");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, VLC_PS5_TITLE_ID "_", sizeof(VLC_PS5_TITLE_ID)))
                continue;
            snprintf(path, sizeof(path), "/mnt/sandbox/%s/app0/eboot.bin", e->d_name);
            if (exists(path)) {
                snprintf(app_dir, sizeof(app_dir), "/mnt/sandbox/%s/app0", e->d_name);
                break;
            }
        }
        closedir(d);
    }
    if (!strcmp(app_dir, APP_DIR) && exists("/data/homebrew/" VLC_PS5_TITLE_ID "/eboot.bin"))
        snprintf(app_dir, sizeof(app_dir), "/data/homebrew/" VLC_PS5_TITLE_ID);
    if (!strcmp(app_dir, APP_DIR))
        fprintf(stderr, "VLC-PS5: full access, but our folder wasn't found again\n");
    else
        fprintf(stderr, "VLC-PS5: our folder is now %s\n", app_dir);
}

/* Optional full access: etaHEN, the Lapy JB daemon and the Ruffle/XPSemu
 * Helper all lift a title's sandbox when it writes {"PID":<pid>} to
 * /download0/etahen_jailbreak; they delete the file when done. With one
 * running we see all of /data and USB drives plugged in later; without one
 * nobody takes the request and VLC carries on in its sandbox, as before.
 * Waits at most a second, while the launch image is still up. */
#define JB_REQUEST "/download0/etahen_jailbreak"
#define JB_STAGED JB_REQUEST ".tmp"

/* Out of the sandbox: root (effective uid 0). /data alone says nothing: a
 * ShadowMountPlus title already sees it from inside its sandbox (console run
 * 9: /data visible, euid 1, and drives plugged in later stayed invisible). */
uid_t __real_geteuid(void); /* the kernel's (geteuid itself is wrapped: ps5/compat/libc_stubs.c) */

static bool lifted(void)
{
    return __real_geteuid() == 0;
}

static void request_full_access(void)
{
    if (lifted()) {
        full_access = true;
        fprintf(stderr, "VLC-PS5: full access already (euid %d)\n", (int)__real_geteuid());
        mkdir("/data/vlc", 0777);
        find_app_dir();
        return;
    }
    /* The Ruffle/XPSemu Helper only lifts titles on its allowlist and re-reads
     * /data/whitelist.txt on every request (console run 10: request taken,
     * nothing lifted). A ShadowMountPlus title sees /data: put ourselves on it. */
    if (exists("/data")) {
        bool listed = false;
        FILE *wl = fopen("/data/whitelist.txt", "r");
        if (wl) {
            char line[64];
            while (!listed && fgets(line, sizeof(line), wl))
                listed = strncmp(line, VLC_PS5_TITLE_ID, 9) == 0;
            fclose(wl);
        }
        if (!listed) {
            wl = fopen("/data/whitelist.txt", "a");
            bool ok = wl && fprintf(wl, "\n%s\n", VLC_PS5_TITLE_ID) > 0;
            if (wl)
                fclose(wl);
            fprintf(stderr, "VLC-PS5: added to /data/whitelist.txt: %s\n", ok ? "yes" : "can't write it");
        }
    }
    unlink(JB_REQUEST);
    unlink(JB_STAGED);
    char request[48];
    int len = snprintf(request, sizeof(request), "{\"PID\":%d}\n", (int)getpid());
    int fd = open(JB_STAGED, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        fprintf(stderr, "VLC-PS5: no full access: can't write the request (errno %d)\n", errno);
        return;
    }
    /* Whole, then renamed: a daemon never reads half a request. */
    bool ok = fchmod(fd, 0666) == 0 && write(fd, request, len) == len && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(JB_STAGED, JB_REQUEST) != 0) {
        fprintf(stderr, "VLC-PS5: no full access: request not written (errno %d)\n", errno);
        unlink(JB_STAGED);
        return;
    }
    double start = plat_time();
    while (exists(JB_REQUEST) && plat_time() - start < 1.0)
        usleep(20000);
    if (exists(JB_REQUEST)) {
        unlink(JB_REQUEST); /* nobody listening: don't leave it for a daemon started later */
        fprintf(stderr, "VLC-PS5: sandboxed (no jailbreak daemon answered in 1 s)\n");
        return;
    }
    /* Taken. Daemons set the effective uid (the real one can stay as it was)
     * and point our root at the console's: either says the sandbox is gone. */
    double taken = plat_time();
    while (!lifted() && plat_time() - taken < 4.0)
        usleep(20000);
    full_access = lifted();
    fprintf(stderr, "VLC-PS5: jailbreak request taken after %.0f ms, checked %.0f ms more: %s "
            "(uid %d, euid %d, /data %s)\n", (taken - start) * 1000, (plat_time() - taken) * 1000,
            full_access ? "full access" : "still sandboxed", (int)getuid(), (int)__real_geteuid(),
            exists("/data") ? "visible" : "not visible");
    if (full_access) {
        mkdir("/data/vlc", 0777); /* our folder on the console's storage, for FTP */
        find_app_dir();
    }
}

/* VLC's home, cache and temporary files, and the graphics driver's shader
 * caches: the app's folder (a title has no HOME, and /tmp isn't ours to
 * write). Set once our folder is known: with full access /app0 is gone, and
 * Mesa's cache then failed on it ("Failed to create /app0", run 19). The
 * shader caches make later starts skip compiling; they need no full access. */
static void use_app_dir(void)
{
    char path[300];
    setenv("HOME", app_dir, 1);
    snprintf(path, sizeof(path), "%s/cache", app_dir);
    mkdir(path, 0777);
    setenv("TMPDIR", path, 1);
    snprintf(path, sizeof(path), "%s/cache/shaders", app_dir);
    mkdir(path, 0777);
    setenv("PS5VK_SHADER_CACHE_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/cache/mesa", app_dir);
    mkdir(path, 0777);
    setenv("MESA_SHADER_CACHE_DIR", path, 1);
    fprintf(stderr, "VLC-PS5: shader caches in %s/cache\n", app_dir);
}

int plat_language(void)
{
    int lang = -1;
    /* parameter 1: the system language (English when the console doesn't
     * give the call) */
    if (!import_resolved("sceSystemServiceParamGetInt") || sceSystemServiceParamGetInt(1, &lang) != 0)
        return -1;
    return lang;
}

bool plat_full_access(void)
{
    return full_access;
}

/* The log is written by its own thread: unbuffered, one log line was many
 * small writes to the app's folder, and the main thread's lines (the stats
 * every 5 s while playing) froze the picture for 40-150 ms on the console.
 * Lines now collect in stdio's buffer and go to the file every 200 ms. */
static void *log_flusher(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(200000);
        fflush(stderr);
    }
    return NULL;
}

void plat_init(void)
{
    rename(LOG_PATH, APP_DIR "/vlc-ps5.old.log");
    /* Append mode, like the crash report's descriptor: with "w" the lines still
     * buffered at a crash were written over the start of the report. */
    if (freopen(LOG_PATH, "a", stderr)) {
        static char log_buffer[256 * 1024];
        setvbuf(stderr, log_buffer, _IOFBF, sizeof(log_buffer));
        pthread_t flusher;
        pthread_create(&flusher, NULL, log_flusher, NULL);
        /* A title may start without descriptors 1 and 2: the log goes on both. */
        if (fileno(stderr) != STDERR_FILENO)
            dup2(fileno(stderr), STDERR_FILENO);
        dup2(fileno(stderr), STDOUT_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
    } else {
        plat_notify("VLC-PS5: can't write /app0/vlc-ps5.log");
    }
    fprintf(stderr, "VLC-PS5: load address marker plat_init=%p\n", (void *)plat_init);
    crash_fd = open(LOG_PATH, O_WRONLY | O_APPEND);

    static uint8_t alt_stack[64 * 1024] __attribute__((aligned(16)));
    stack_t ss = { .ss_sp = alt_stack, .ss_size = sizeof(alt_stack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    /* A dropped network connection must not end the app. */
    signal(SIGPIPE, SIG_IGN);

    report_unresolved_imports();
    request_full_access();
    use_app_dir();
}

const char *plat_data_dir(void)
{
    return app_dir;
}

const char *const *plat_media_roots(void)
{
    /* Tried in order; the ones a title can't open are skipped. /data's
     * folders open only with full access. */
    static const char *const roots[] = {
        "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
        "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
        "/mnt/ext0", "/mnt/ext1", "/data/vlc", "/data/media", NULL,
    };
    return roots;
}

void plat_hide_splash(void)
{
    fprintf(stderr, "VLC-PS5: splash hidden (%#x)\n", sceSystemServiceHideSplashScreen());
}

void plat_quit(void)
{
    fflush(NULL);
    sceSystemServiceLoadExec("exit", NULL);
    _exit(0);
}

/* A title sees the drives that were mounted when it started (a USB drive
 * plugged in later isn't in its view; one pulled out does vanish). Loading
 * our own eboot again starts a fresh process, as PS4 homebrew relaunches. */
void plat_restart(void)
{
    fprintf(stderr, "VLC-PS5: restarting to see new drives\n");
    fflush(NULL);
    int rc = sceSystemServiceLoadExec(APP_DIR "/eboot.bin", NULL);
    fprintf(stderr, "VLC-PS5: restart refused (%#x)\n", rc);
}

double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- DualSense ------------------------------------------------------------ */

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t reserved_to_connected[66];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
} PadSample;
_Static_assert(sizeof(PadSample) == 120, "the console's pad samples are 120 bytes");
_Static_assert(offsetof(PadSample, connected) == 0x4c, "connected at 0x4c");
#define PAD_INTERCEPTED 0x80000000u

static int32_t pad_handle = -1, pad_user = -1;
static double pad_next_open;

static float stick_axis(uint8_t v)
{
    return (v - 127.5f) / 127.5f;
}

static void stick(uint8_t bx, uint8_t by, float *x, float *y)
{
    float fx = stick_axis(bx), fy = stick_axis(by);
    float len = sqrtf(fx * fx + fy * fy);
    const float dead = 0.18f;
    if (len < dead) {
        *x = *y = 0;
        return;
    }
    float scale = (len > 1 ? 1 : (len - dead) / (1 - dead)) / len;
    *x = fx * scale;
    *y = fy * scale;
}

/* The console doesn't give titles scePadSetVibration (run 10: unresolved);
 * no rumble until another way is found. */
void plat_pad_rumble(float strength, int ms)
{
    (void)strength;
    (void)ms;
}

bool plat_pad_read(PadState *state)
{
    static PadSample last;
    static bool have_last;
    memset(state, 0, sizeof(*state));
    double now = plat_time();
    if (pad_handle < 0 && now >= pad_next_open) {
        static bool initialized;
        pad_next_open = now + 1.0;
        if (!initialized) {
            fprintf(stderr, "VLC-PS5: user service %#x, pad %#x\n", sceUserServiceInitialize(NULL),
                    scePadInit());
            initialized = true;
        }
        if (pad_user >= 0 || sceUserServiceGetInitialUser(&pad_user) == 0) {
            pad_handle = scePadOpen(pad_user, 0, 0, NULL);
            fprintf(stderr, "VLC-PS5: pad user %d, handle %d\n", pad_user, pad_handle);
        } else {
            pad_user = -1;
        }
    }
    if (pad_handle < 0)
        return false;
    PadSample samples[64];
    int n = scePadRead(pad_handle, samples, 64);
    if (n < 0 || n > 64) {
        have_last = false;
        if (n < 0) {
            scePadClose(pad_handle);
            pad_handle = -1;
        }
        return false;
    }
    const PadSample *best = NULL;
    for (int i = 0; i < n; i++)
        if (!best || samples[i].timestamp_us > best->timestamp_us)
            best = &samples[i];
    if (best) {
        last = *best;
        have_last = true;
    }
    /* The shell has the pad (home screen, a system dialog): nothing is held. */
    if (!have_last || !last.connected || (last.buttons & PAD_INTERCEPTED))
        return false;
    state->connected = true;
    state->buttons = last.buttons & 0x00ffffffu;
    stick(last.lx, last.ly, &state->lx, &state->ly);
    stick(last.rx, last.ry, &state->rx, &state->ry);
    state->l2 = last.l2 / 255.0f;
    state->r2 = last.r2 / 255.0f;
    /* Touchpad (ScePadData's touch data at 0x34: count, then 8-byte points of
     * x, y at 0x3c; the pad is 1920x1080). */
    const uint8_t *raw = (const uint8_t *)&last;
    state->touches = raw[0x34] > 2 ? 2 : raw[0x34];
    if (state->touches) {
        uint16_t x, y;
        memcpy(&x, raw + 0x3c, 2);
        memcpy(&y, raw + 0x3e, 2);
        state->tx = x / 1919.0f;
        state->ty = y / 1079.0f;
    }
    if (state->l2 > 0.5f)
        state->buttons |= PAD_L2;
    if (state->r2 > 0.5f)
        state->buttons |= PAD_R2;
    return true;
}

/* ---- the TV through VK_KHR_display (QUICK3's mode choice) ----------------- */

PFN_vkGetInstanceProcAddr plat_vk_loader(void)
{
    return (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
}

const char *const *plat_instance_extensions(uint32_t *count)
{
    static const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                              VK_KHR_DISPLAY_EXTENSION_NAME };
    *count = 2;
    return extensions;
}

bool plat_create_surface(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR *surface,
                         uint32_t *width, uint32_t *height, uint32_t *refresh_mhz)
{
    uint32_t displays = 1;
    VkDisplayPropertiesKHR display;
    VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &displays, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || displays == 0) {
        fprintf(stderr, "VLC-PS5: no display (%d)\n", r);
        return false;
    }
    uint32_t planes = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &planes, NULL);
    VkDisplayPlanePropertiesKHR plane_props[16];
    planes = planes > 16 ? 16 : planes;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &planes, plane_props);
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < planes; i++) {
        if (plane_props[i].currentDisplay == VK_NULL_HANDLE ||
            plane_props[i].currentDisplay == display.display) {
            plane = i;
            break;
        }
    }
    if (plane == UINT32_MAX) {
        fprintf(stderr, "VLC-PS5: no plane for the display\n");
        return false;
    }
    uint32_t modes = 0;
    vkGetDisplayModePropertiesKHR(gpu, display.display, &modes, NULL);
    VkDisplayModePropertiesKHR props[32];
    modes = modes > 32 ? 32 : modes;
    vkGetDisplayModePropertiesKHR(gpu, display.display, &modes, props);
    if (modes == 0) {
        fprintf(stderr, "VLC-PS5: the display offers no mode\n");
        return false;
    }
    /* The largest mode at its 120 Hz (smooth menus; 24p films divide evenly).
     * Any mode the title sets makes the TV re-sync once at start, 60 Hz too
     * (tested), so the faster one. "refresh=60" in /app0/settings.txt picks 60. */
    uint32_t want_hz = 120;
    char settings_path[300];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.txt", app_dir);
    FILE *settings = fopen(settings_path, "r");
    if (settings) {
        char line[128];
        while (fgets(line, sizeof(line), settings))
            sscanf(line, "refresh=%u", &want_hz);
        fclose(settings);
    }
    uint32_t best = 0;
    for (uint32_t i = 0; i < modes; i++) {
        const VkDisplayModeParametersKHR *p = &props[i].parameters;
        const VkDisplayModeParametersKHR *b = &props[best].parameters;
        fprintf(stderr, "VLC-PS5: mode %u: %ux%u at %.2f Hz\n", i, p->visibleRegion.width,
                p->visibleRegion.height, p->refreshRate / 1000.0);
        uint64_t ap = (uint64_t)p->visibleRegion.width * p->visibleRegion.height;
        uint64_t ab = (uint64_t)b->visibleRegion.width * b->visibleRegion.height;
        int dp = abs((int)p->refreshRate - (int)want_hz * 1000);
        int db = abs((int)b->refreshRate - (int)want_hz * 1000);
        if (ap > ab || (ap == ab && dp < db))
            best = i;
    }
    const VkDisplayModeParametersKHR *p = &props[best].parameters;
    *width = p->visibleRegion.width;
    *height = p->visibleRegion.height;
    *refresh_mhz = p->refreshRate;
    VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = props[best].displayMode,
        .planeIndex = plane,
        .planeStackIndex = plane_props[plane].currentStackIndex,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = { *width, *height },
    };
    r = vkCreateDisplayPlaneSurfaceKHR(instance, &info, NULL, surface);
    fprintf(stderr, "VLC-PS5: display surface %ux%u at %.2f Hz, plane %u: %d\n", *width, *height,
            *refresh_mhz / 1000.0, plane, r);
    return r == VK_SUCCESS;
}

/* ---- sound ----------------------------------------------------------------- */

static int audio_port = -1;

bool plat_audio_open(void)
{
    int rc = sceAudioOutInit();
    /* System user, main port, 16-bit stereo (QUICK3, PS5_vkQuake, XPSemu). */
    audio_port = sceAudioOutOpen(0xff, 0, 0, AUDIO_GRAIN, AUDIO_RATE, 1);
    fprintf(stderr, "VLC-PS5: audio init %#x, port %#x\n", rc, audio_port);
    return audio_port >= 0;
}

void plat_audio_output(const int16_t *frames)
{
    if (audio_port >= 0)
        sceAudioOutOutput(audio_port, frames);
}

static int surround_port = -1;

bool plat_audio_open_surround(void)
{
    static bool tried;
    if (!tried) {
        tried = true;
        /* Format 2: 16-bit, 8 channels (L R C LFE Ls Rs Lb Rb). The system
         * mixes it down for a stereo TV. */
        surround_port = sceAudioOutOpen(0xff, 0, 0, AUDIO_GRAIN, AUDIO_RATE, 2);
        fprintf(stderr, "VLC-PS5: surround port %#x\n", surround_port);
    }
    return surround_port >= 0;
}

void plat_audio_output_surround(const int16_t *frames)
{
    if (surround_port >= 0)
        sceAudioOutOutput(surround_port, frames);
}

const char *plat_screenshot_path(uint64_t frame)
{
    (void)frame;
    return NULL;
}

bool plat_script_done(uint64_t frame)
{
    (void)frame;
    return false;
}
