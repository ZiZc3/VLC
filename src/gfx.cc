/*
 * VLC-PS5's Vulkan set-up and frame loop.
 *
 * On the console the image goes to the TV through VK_KHR_display (the
 * platform picks the mode) with FIFO presentation: vsync, no tearing. On the
 * host test build there is no surface: frames render into one offscreen image
 * and the script's screenshots are read back from it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gfx.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "platform.h"

#ifdef VLCPS5_HOST
#include <zlib.h>
#endif

Gfx gfx;

namespace {

struct Frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    uint64_t number; /* frame recorded in this slot last */
};

VkSurfaceKHR surface;
VkSwapchainKHR swapchain;
std::vector<VkImage> images;
std::vector<VkImageView> views;
std::vector<VkFramebuffer> framebuffers;
std::vector<VkSemaphore> rendered; /* per swapchain image */
Frame frames[GFX_FRAMES];
uint32_t image_index;
uint64_t completed;
VkCommandPool setup_pool;

/* Host test build: the offscreen target and its read-back buffer. */
VkDeviceMemory offscreen_memory, readback_memory;
VkBuffer readback;
void *readback_mapped;

#define CHECK(expr)                                                          \
    do {                                                                     \
        VkResult r_ = (expr);                                                \
        if (r_ != VK_SUCCESS) {                                              \
            fprintf(stderr, "gfx: %s failed: %d\n", #expr, r_);              \
            return false;                                                    \
        }                                                                    \
    } while (0)

bool create_instance()
{
    PFN_vkGetInstanceProcAddr loader = plat_vk_loader();
    if (!loader)
        return false;
    volkInitializeCustom(loader);
    uint32_t ext_count = 0;
    const char *const *exts = plat_instance_extensions(&ext_count);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VLC-PS5";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = ext_count;
    info.ppEnabledExtensionNames = exts;
    CHECK(vkCreateInstance(&info, nullptr, &gfx.instance));
    volkLoadInstance(gfx.instance);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gfx.instance, &count, nullptr);
    if (!count) {
        fprintf(stderr, "gfx: no GPU\n");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(gfx.instance, &count, gpus.data());
    gfx.gpu = gpus[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gfx.gpu, &props);
    fprintf(stderr, "gfx: %s, Vulkan %u.%u.%u\n", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(gfx.gpu, &gfx.memory);
    return true;
}

bool create_device()
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gfx.gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gfx.gpu, &count, families.data());
    gfx.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        if (!gfx.offscreen) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gfx.gpu, i, surface, &present);
            if (!present)
                continue;
        }
        gfx.queue_family = i;
        break;
    }
    if (gfx.queue_family == UINT32_MAX) {
        fprintf(stderr, "gfx: no graphics queue that can present\n");
        return false;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = gfx.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char *swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = gfx.offscreen ? 0 : 1;
    info.ppEnabledExtensionNames = &swapchain_ext;
    CHECK(vkCreateDevice(gfx.gpu, &info, nullptr, &gfx.device));
    volkLoadDevice(gfx.device);
    vkGetDeviceQueue(gfx.device, gfx.queue_family, 0, &gfx.queue);
    return true;
}

bool create_swapchain()
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gfx.gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gfx.gpu, surface, &count, formats.data());
    /* UNORM: the UI's colours and the video shader's output are already
     * gamma-encoded, as on a desktop compositor. */
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const VkSurfaceFormatKHR &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM ||
            f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            chosen = f;
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
                break;
        }
    gfx.format = chosen.format;
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gfx.gpu, surface, &caps);
    if (caps.currentExtent.width != UINT32_MAX) {
        gfx.width = caps.currentExtent.width;
        gfx.height = caps.currentExtent.height;
    }
    uint32_t want = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && want > caps.maxImageCount)
        want = caps.maxImageCount;
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = surface;
    info.minImageCount = want;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = { gfx.width, gfx.height };
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : (VkCompositeAlphaFlagBitsKHR)caps.supportedCompositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(gfx.device, &info, nullptr, &swapchain));
    vkGetSwapchainImagesKHR(gfx.device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(gfx.device, swapchain, &count, images.data());
    gfx.image_count = count;
    fprintf(stderr, "gfx: swapchain %ux%u, format %d, %u images, FIFO\n", gfx.width, gfx.height,
            gfx.format, count);
    return true;
}

bool create_offscreen()
{
    gfx.format = VK_FORMAT_B8G8R8A8_UNORM;
    gfx.image_count = 1;
    VkImage image;
    VkImageView view;
    if (!gfx_image(gfx.width, gfx.height, gfx.format,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &image,
                   &offscreen_memory, &view))
        return false;
    images.push_back(image);
    views.push_back(view);
    if (!gfx_buffer((VkDeviceSize)gfx.width * gfx.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &readback, &readback_memory, &readback_mapped))
        return false;
    fprintf(stderr, "gfx: offscreen %ux%u\n", gfx.width, gfx.height);
    return true;
}

bool create_targets()
{
    VkAttachmentDescription color = {};
    color.format = gfx.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = gfx.offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                      : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    /* The image is free to write once acquired; uploads before the pass are
     * visible to the fragment shaders that sample them. */
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].dstSubpass = 0;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    CHECK(vkCreateRenderPass(gfx.device, &rp, nullptr, &gfx.render_pass));

    if (!gfx.offscreen) {
        views.resize(images.size());
        for (size_t i = 0; i < images.size(); i++) {
            VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = gfx.format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            CHECK(vkCreateImageView(gfx.device, &vi, nullptr, &views[i]));
        }
    }
    framebuffers.resize(images.size());
    rendered.resize(images.size());
    for (size_t i = 0; i < images.size(); i++) {
        VkFramebufferCreateInfo fb = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb.renderPass = gfx.render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &views[i];
        fb.width = gfx.width;
        fb.height = gfx.height;
        fb.layers = 1;
        CHECK(vkCreateFramebuffer(gfx.device, &fb, nullptr, &framebuffers[i]));
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHECK(vkCreateSemaphore(gfx.device, &si, nullptr, &rendered[i]));
    }
    for (Frame &f : frames) {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = gfx.queue_family;
        CHECK(vkCreateCommandPool(gfx.device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(gfx.device, &ai, &f.cmd));
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(vkCreateFence(gfx.device, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHECK(vkCreateSemaphore(gfx.device, &si, nullptr, &f.acquired));
    }
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = gfx.queue_family;
    CHECK(vkCreateCommandPool(gfx.device, &pi, nullptr, &setup_pool));
    return true;
}

#ifdef VLCPS5_HOST
/* The offscreen image (BGRA) as an RGB PNG. */
void save_png(const char *path)
{
    uint32_t w = gfx.width, h = gfx.height;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    const uint8_t *src = (const uint8_t *)readback_mapped;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = src[i * 4 + 2];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4];
    }
    if (write_png_rgb(path, rgb.data(), w, h))
        fprintf(stderr, "gfx: screenshot %s\n", path);
    else
        perror(path);
}
#endif

} // namespace

bool gfx_init()
{
    if (!create_instance())
        return false;
    uint32_t w = 0, h = 0, hz = 0;
    gfx.offscreen = !plat_create_surface(gfx.instance, gfx.gpu, &surface, &w, &h, &hz);
    gfx.width = w;
    gfx.height = h;
    gfx.refresh_mhz = hz;
#ifndef VLCPS5_HOST
    if (gfx.offscreen) {
        fprintf(stderr, "gfx: no display surface\n");
        return false;
    }
#endif
    if (!create_device())
        return false;
    if (gfx.offscreen ? !create_offscreen() : !create_swapchain())
        return false;
    return create_targets();
}

void gfx_shutdown()
{
    if (gfx.device)
        vkDeviceWaitIdle(gfx.device);
}

VkCommandBuffer gfx_begin_frame()
{
    Frame &f = frames[gfx.frame_slot];
    vkWaitForFences(gfx.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (f.number > completed)
        completed = f.number;
    if (gfx.offscreen) {
        /* One image: the frame before must be done with it. */
        for (Frame &other : frames)
            vkWaitForFences(gfx.device, 1, &other.fence, VK_TRUE, UINT64_MAX);
        completed = gfx.frame_number;
        image_index = 0;
    } else {
        VkResult r = vkAcquireNextImageKHR(gfx.device, swapchain, UINT64_MAX, f.acquired,
                                           VK_NULL_HANDLE, &image_index);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "gfx: acquire failed: %d\n", r);
            return VK_NULL_HANDLE;
        }
    }
    vkResetFences(gfx.device, 1, &f.fence);
    vkResetCommandPool(gfx.device, f.pool, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    f.number = gfx.frame_number + 1;
    return f.cmd;
}

void gfx_begin_pass(VkCommandBuffer cmd)
{
    VkClearValue clear = {};
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = gfx.render_pass;
    rb.framebuffer = framebuffers[image_index];
    rb.renderArea.extent = { gfx.width, gfx.height };
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}

void gfx_end_frame(VkCommandBuffer cmd)
{
    Frame &f = frames[gfx.frame_slot];
    vkCmdEndRenderPass(cmd);
#ifdef VLCPS5_HOST
    const char *shot = plat_screenshot_path(gfx.frame_number);
    if (shot) {
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { gfx.width, gfx.height, 1 };
        vkCmdCopyImageToBuffer(cmd, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                               &region);
    }
#endif
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (!gfx.offscreen) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[image_index];
    }
    vkQueueSubmit(gfx.queue, 1, &si, f.fence);
    if (!gfx.offscreen) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[image_index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image_index;
        vkQueuePresentKHR(gfx.queue, &pi);
    }
#ifdef VLCPS5_HOST
    if (shot) {
        vkWaitForFences(gfx.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        save_png(shot);
    }
#endif
    gfx.frame_number++;
    gfx.frame_slot = (gfx.frame_slot + 1) % GFX_FRAMES;
}

uint64_t gfx_completed_frame()
{
    return completed;
}

uint32_t gfx_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < gfx.memory.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (gfx.memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

bool gfx_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                VkBuffer *buffer, VkDeviceMemory *memory, void **mapped)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(gfx.device, &bi, nullptr, buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gfx.device, *buffer, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gfx_memory_type(req.memoryTypeBits, flags);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "gfx: no memory type for a buffer\n");
        return false;
    }
    CHECK(vkAllocateMemory(gfx.device, &ai, nullptr, memory));
    CHECK(vkBindBufferMemory(gfx.device, *buffer, *memory, 0));
    if (mapped)
        CHECK(vkMapMemory(gfx.device, *memory, 0, VK_WHOLE_SIZE, 0, mapped));
    return true;
}

bool gfx_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImage *image,
               VkDeviceMemory *memory, VkImageView *view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(gfx.device, &ii, nullptr, image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gfx.device, *image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gfx_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = gfx_memory_type(req.memoryTypeBits, 0);
    CHECK(vkAllocateMemory(gfx.device, &ai, nullptr, memory));
    CHECK(vkBindImageMemory(gfx.device, *image, *memory, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHECK(vkCreateImageView(gfx.device, &vi, nullptr, view));
    return true;
}

VkShaderModule gfx_shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = size;
    si.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(gfx.device, &si, nullptr, &module) != VK_SUCCESS)
        fprintf(stderr, "gfx: shader module failed\n");
    return module;
}

VkCommandBuffer gfx_one_shot_begin()
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = setup_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(gfx.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void gfx_one_shot_end(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(gfx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(gfx.queue);
    vkFreeCommandBuffers(gfx.device, setup_pool, 1, &cmd);
}

void gfx_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                 VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}
