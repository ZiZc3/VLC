/*
 * VLC-PS5: mpv user shaders (FSRCNNX, Anime4K) on the video, converted at
 * build time by ps5/mpv-shaders.py (third_party/mpv-shaders keeps the
 * originals and their licences).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

/* the sets the player offers */
enum { MPV_FSRCNNX, MPV_A4K_LIGHT, MPV_A4K_HEAVY, MPV_A4K_MEDIUM, MPV_SET_COUNT };

struct MpvImage {
    VkImageView view;
    uint32_t w, h;
};

/* pass: a render pass for one RGBA16F colour image (left shader-readable);
 * sampler: linear, clamped. */
bool mpv_init(VkRenderPass pass, VkSampler sampler);

/* Records one set's passes on `in` (the texture its passes hook: LUMA for
 * FSRCNNX, MAIN for Anime4K), for a picture shown at target_w x target_h.
 * true: *out is the result (bigger than `in`); false: nothing ran (the
 * shader's own condition said no, or it failed) and *out = in. Outside a
 * render pass. A new size waits for the GPU once. */
bool mpv_record(VkCommandBuffer cmd, int set, const MpvImage &in, float target_w, float target_h, MpvImage *out);

/* changes each time the images are made again (a descriptor that reads
 * the result must be written again then, even if the view looks the same) */
unsigned mpv_plan_id();

/* the set's name, for the log */
const char *mpv_set_name(int set);
