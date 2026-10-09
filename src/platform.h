/*
 * VLC-PS5: what the app needs from the machine it runs on.
 *
 * Two implementations:
 *   platform_ps5.c   the console: log in /app0 (no jailbreak), crash report,
 *                    import report, VK_KHR_display at the TV's mode, DualSense
 *                    through scePadRead, sound through SceAudioOut.
 *   platform_host.c  the Linux test build: no display (gfx renders offscreen and
 *                    saves screenshots), buttons from a script, sound discarded.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VLC_PS5_PLATFORM_H
#define VLC_PS5_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "volk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VLC_PS5_TITLE_ID "PPSA85300"
#define VLC_PS5_VERSION "0.3"

/* DualSense buttons (scePadRead's layout, as QUICK3 and PS5_vkQuake use it),
 * plus the triggers past half as buttons. */
enum {
    PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8,
    PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
    PAD_L1 = 0x400, PAD_R1 = 0x800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
    PAD_TOUCHPAD = 0x100000,
    PAD_L2 = 0x1000000, PAD_R2 = 0x2000000,
};

typedef struct {
    bool connected;
    uint32_t buttons;
    float lx, ly, rx, ry; /* -1..1, dead zone applied */
    float l2, r2;         /* 0..1 */
    int touches;          /* fingers on the touchpad (0-2) */
    float tx, ty;         /* the first one, 0..1 across and down */
} PadState;

/* Start-up: log file, crash handler, import report. Before anything else. */
void plat_init(void);
/* The app's own folder: media/, cache/, settings. "/app0" on the console. */
const char *plat_data_dir(void);
/* True when a jailbreak daemon (etaHEN, Lapy JB daemon, Helper) lifted the
 * sandbox at start-up: all of /data, and USB drives plugged in later. */
bool plat_full_access(void);
/* Extra folders to look for media in (USB drives, ...); NULL-terminated. */
const char *const *plat_media_roots(void);
void plat_notify(const char *message);
/* After the first frames are on screen: the system's launch image goes. */
void plat_hide_splash(void);
/* Leave the app (the console's shell takes over). */
void plat_quit(void);
/* Start the app again (a fresh process sees the drives mounted since). */
void plat_restart(void);
/* Seconds, monotonic. */
double plat_time(void);
/* The console's language (its system setting number: 0 Japanese, 1 English,
 * 2 French...), or -1 when unknown. */
int plat_language(void);

bool plat_pad_read(PadState *state);
/* A short buzz of the controller's small motor (0..1), for ms milliseconds. */
void plat_pad_rumble(float strength, int ms);

/* Vulkan: the driver's entry point, and the TV as a surface. On the host
 * plat_create_surface returns false and gfx renders offscreen instead. */
PFN_vkGetInstanceProcAddr plat_vk_loader(void);
const char *const *plat_instance_extensions(uint32_t *count);
bool plat_create_surface(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR *surface,
                         uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);

/* Sound: 48 kHz 16-bit stereo, AUDIO_GRAIN frames per call; blocks until the
 * previous grain has played, which paces the caller. */
#define AUDIO_RATE 48000
#define AUDIO_GRAIN 256
bool plat_audio_open(void);
void plat_audio_output(const int16_t *frames);
/* Surround: a second port, 8 channels in the console's order (L, R, C, LFE,
 * surround L, surround R, back L, back R), opened on first use. False if the
 * console refused it. */
bool plat_audio_open_surround(void);
void plat_audio_output_surround(const int16_t *frames);

/* Host test build only: should a screenshot of this frame be saved, and where. */
const char *plat_screenshot_path(uint64_t frame);
/* Host test build only: true once the script is done. */
bool plat_script_done(uint64_t frame);

#ifdef __cplusplus
}
#endif

#endif
