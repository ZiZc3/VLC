/*
 * VLC-PS5's Linux test build: the whole app with no TV and no controller.
 * gfx renders offscreen (any Vulkan driver; lavapipe in WSL), buttons come
 * from a script, sound is paced like SceAudioOut and thrown away.
 *
 *   VLCPS5_DATA=<dir>      the app folder (stands in for /app0); default ./appdata
 *   VLCPS5_SCRIPT=<file>   one step per line, at a frame number:
 *       <frame> press <button>         held for 3 frames (cross circle square
 *                                      triangle up down left right l1 r1 l2 r2
 *                                      options touchpad)
 *       <frame> hold <button> <frames>
 *       <frame> shot <file.png>        saves that frame
 *       <frame> quit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "platform.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_STEPS 512

typedef struct {
    uint64_t frame;
    enum { STEP_HOLD, STEP_SHOT, STEP_QUIT, STEP_STICK } kind;
    uint32_t button;
    float rx, ry;     /* STEP_STICK: the right stick, held for frames */
    uint64_t frames;
    char path[256];
} Step;

static Step steps[MAX_STEPS];
static int nsteps;
static uint64_t frame_now;
static uint64_t quit_frame = UINT64_MAX;

static uint32_t button_named(const char *name)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        { "cross", PAD_CROSS }, { "circle", PAD_CIRCLE }, { "square", PAD_SQUARE },
        { "triangle", PAD_TRIANGLE }, { "up", PAD_UP }, { "down", PAD_DOWN },
        { "left", PAD_LEFT }, { "right", PAD_RIGHT }, { "l1", PAD_L1 }, { "r1", PAD_R1 },
        { "l2", PAD_L2 }, { "r2", PAD_R2 }, { "options", PAD_OPTIONS },
        { "touchpad", PAD_TOUCHPAD }, { "l3", PAD_L3 }, { "r3", PAD_R3 },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i].name))
            return names[i].bit;
    fprintf(stderr, "script: unknown button %s\n", name);
    return 0;
}

static void load_script(void)
{
    const char *path = getenv("VLCPS5_SCRIPT");
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return;
    }
    char line[512];
    while (nsteps < MAX_STEPS && fgets(line, sizeof(line), f)) {
        unsigned long long frame, frames;
        char word[32], arg[256];
        Step *s = &steps[nsteps];
        if (line[0] == '#' || sscanf(line, "%llu %31s", &frame, word) != 2)
            continue;
        s->frame = frame;
        if (!strcmp(word, "press") && sscanf(line, "%*u %*s %255s", arg) == 1) {
            s->kind = STEP_HOLD;
            s->button = button_named(arg);
            s->frames = 3;
        } else if (!strcmp(word, "hold") && sscanf(line, "%*u %*s %255s %llu", arg, &frames) == 2) {
            s->kind = STEP_HOLD;
            s->button = button_named(arg);
            s->frames = frames;
        } else if (!strcmp(word, "stick") &&
                   sscanf(line, "%*u %*s %f %f %llu", &s->rx, &s->ry, &frames) == 3) {
            s->kind = STEP_STICK;
            s->frames = frames;
        } else if (!strcmp(word, "shot") && sscanf(line, "%*u %*s %255s", s->path) == 1) {
            s->kind = STEP_SHOT;
        } else if (!strcmp(word, "quit")) {
            s->kind = STEP_QUIT;
            quit_frame = frame;
        } else {
            continue;
        }
        nsteps++;
    }
    fclose(f);
    fprintf(stderr, "script: %d steps from %s\n", nsteps, path);
}

void plat_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    load_script();
}

const char *plat_data_dir(void)
{
    const char *dir = getenv("VLCPS5_DATA");
    return dir ? dir : "./appdata";
}

const char *const *plat_media_roots(void)
{
    static const char *const roots[] = { NULL };
    return roots;
}

void plat_notify(const char *message)
{
    fprintf(stderr, "[toast] %s\n", message);
}

void plat_hide_splash(void)
{
}

void plat_quit(void)
{
    fflush(NULL);
    exit(0);
}

void plat_restart(void)
{
    fprintf(stderr, "[restart] (test build: ignored)\n");
}

int plat_language(void)
{
    /* VLCPS5_LANG=<the console's number> to try one */
    const char *l = getenv("VLCPS5_LANG");
    return l ? atoi(l) : -1;
}

bool plat_full_access(void)
{
    return getenv("VLCPS5_FULL_ACCESS") != NULL; /* to see the full-access interface */
}

double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Called once per frame by the app (through plat_pad_read): the script's
 * clock is the frame count, so a run is the same on any machine. */
bool plat_pad_read(PadState *state)
{
    /* 60 frames a second, as the TV's vsync paces the console: script frame
     * numbers are then real time (frame 600 = 10 s in). */
    static double next;
    double t = plat_time();
    if (next > t)
        usleep((useconds_t)((next - t) * 1e6));
    next = (next > t ? next : t) + 1.0 / 60;
    memset(state, 0, sizeof(*state));
    state->connected = true;
    for (int i = 0; i < nsteps; i++) {
        const Step *s = &steps[i];
        if (s->kind == STEP_HOLD && frame_now >= s->frame && frame_now < s->frame + s->frames)
            state->buttons |= s->button;
        if (s->kind == STEP_STICK && frame_now >= s->frame && frame_now < s->frame + s->frames) {
            state->rx = s->rx;
            state->ry = s->ry;
        }
    }
    if (state->buttons & PAD_L2)
        state->l2 = 1;
    if (state->buttons & PAD_R2)
        state->r2 = 1;
    frame_now++;
    return true;
}

PFN_vkGetInstanceProcAddr plat_vk_loader(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "no libvulkan.so.1: %s\n", dlerror());
        return NULL;
    }
    return (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
}

const char *const *plat_instance_extensions(uint32_t *count)
{
    *count = 0;
    return NULL;
}

bool plat_create_surface(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR *surface,
                         uint32_t *width, uint32_t *height, uint32_t *refresh_mhz)
{
    (void)instance; (void)gpu; (void)surface;
    /* Offscreen at 1080p unless asked otherwise (VLCPS5_SIZE=3840x2160). */
    *width = 1920;
    *height = 1080;
    const char *size = getenv("VLCPS5_SIZE");
    if (size)
        sscanf(size, "%ux%u", width, height);
    *refresh_mhz = 60000;
    return false;
}

bool plat_audio_open(void)
{
    return true;
}

void plat_audio_output(const int16_t *frames)
{
    (void)frames;
    usleep(AUDIO_GRAIN * 1000000 / AUDIO_RATE);
}

void plat_pad_rumble(float strength, int ms)
{
    (void)strength;
    (void)ms;
}

bool plat_audio_open_surround(void)
{
    return true;
}

void plat_audio_output_surround(const int16_t *frames)
{
    plat_audio_output(frames);
}

const char *plat_screenshot_path(uint64_t frame)
{
    for (int i = 0; i < nsteps; i++)
        if (steps[i].kind == STEP_SHOT && steps[i].frame == frame)
            return steps[i].path;
    return NULL;
}

bool plat_script_done(uint64_t frame)
{
    return frame >= quit_frame;
}
