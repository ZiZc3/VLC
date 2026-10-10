/*
 * VLC-PS5's Vulkan: one device, the TV's swapchain (or an offscreen image on
 * the host test build), one render pass that video and UI both draw in.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>

#include "volk.h"

#define GFX_FRAMES 2 /* frames in flight */

struct Gfx {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceMemoryProperties memory;
    VkRenderPass render_pass;
    VkFormat format;
    uint32_t width, height, refresh_mhz;
    uint32_t image_count;
    bool offscreen;         /* host test build */
    uint32_t frame_slot;    /* 0..GFX_FRAMES-1, the frame being recorded */
    uint64_t frame_number;  /* frames presented so far */
    /* HDR10 output: the frame is drawn in a 10-bit image (video as PQ,
     * BT.2020, the UI layer over it) and copied into the framebuffer, which
     * VideoOut then shows as HDR10. */
    VkRenderPass hdr_pass;  /* the 10-bit frame's pass (null: no HDR output) */
    bool hdr;               /* HDR10 output on now */
    int display_range;      /* the output at start: 1 SDR, 2 HDR, other unknown */
};
extern Gfx gfx;

bool gfx_init();
void gfx_shutdown();
/* Waits for this slot's previous frame, acquires an image, begins the command
 * buffer (outside the render pass: record uploads first). Null if the frame
 * must be skipped. */
VkCommandBuffer gfx_begin_frame();
void gfx_begin_pass(VkCommandBuffer cmd);
/* Ends the pass, submits, presents (or saves the host screenshot). */
void gfx_end_frame(VkCommandBuffer cmd);
/* HDR10 output on or off (waits for the GPU); false if it couldn't change. */
bool gfx_set_hdr(bool on);
/* With HDR10 output on, a frame is: the UI pass (ImGui into its own layer),
 * then the HDR pass (the video, then gfx_draw_ui_layer), then gfx_end_frame. */
void gfx_begin_ui_pass(VkCommandBuffer cmd);
void gfx_begin_hdr_pass(VkCommandBuffer cmd);   /* ends the UI pass */
void gfx_draw_ui_layer(VkCommandBuffer cmd);
/* The fence of the frame recorded in this slot has passed: its resources are free. */
uint64_t gfx_completed_frame();

uint32_t gfx_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags);
bool gfx_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
/* mips: levels made and seen by the view (the caller fills them). */
bool gfx_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImage *image,
               VkDeviceMemory *memory, VkImageView *view, uint32_t mips = 1);
VkShaderModule gfx_shader(const uint32_t *code, size_t size);
/* Runs commands now and waits (set-up only). */
VkCommandBuffer gfx_one_shot_begin();
void gfx_one_shot_end(VkCommandBuffer cmd);
void gfx_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                 VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);
