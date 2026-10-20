/*
 * VLC-PS5: mpv user shaders (FSRCNNX, Anime4K) on the video.
 *
 * ps5/mpv-shaders.py turned each pass of the shader files into a Vulkan
 * fragment shader and a row of src/gen/mpv_shaders.h: the texture it hooks,
 * the ones it reads (by name), the one it saves, its size and its condition
 * (mpv's reverse-Polish expressions). Here a set's passes are planned for
 * the picture's size (which run, how big each result is, which image each
 * name means at each step) and recorded every frame, as mpv runs them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "mpvshader.h"

#include <map>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "gen/fullscreen_vert.h"
#include "gen/mpv_shaders.h"
#include "gfx.h"

namespace {

const int MAX_BINDS = 16;

struct SetInfo {
    const MpvPassInfo *passes;
    int count;
    const char *name;
};
const SetInfo sets[MPV_SET_COUNT] = {
    { mpv_set_fsrcnnx, (int)(sizeof(mpv_set_fsrcnnx) / sizeof(*mpv_set_fsrcnnx)), "FSRCNNX x2 8" },
    { mpv_set_a4k_light, (int)(sizeof(mpv_set_a4k_light) / sizeof(*mpv_set_a4k_light)), "Anime4K M" },
    { mpv_set_a4k_heavy, (int)(sizeof(mpv_set_a4k_heavy) / sizeof(*mpv_set_a4k_heavy)), "Anime4K VL" },
    { mpv_set_a4k_medium, (int)(sizeof(mpv_set_a4k_medium) / sizeof(*mpv_set_a4k_medium)), "Anime4K M + VL" },
};

VkRenderPass render_pass;
VkSampler linear;
VkDescriptorSetLayout set_layout;
VkPipelineLayout layout;
VkDescriptorPool pool;
std::vector<VkPipeline> pipelines[MPV_SET_COUNT];   /* made on first use */
unsigned plan_id;

struct Push {
    float out_size[4];   /* this pass: width, height, 1/width, 1/height */
    float target[4];     /* the picture's final size */
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

struct Step {
    int pass;              /* index in the set */
    int image;             /* the result's image (in Plan::images) */
    VkDescriptorSet set;   /* the textures it reads */
};

/* One set planned for one input and target */
struct Plan {
    int set = -1;
    VkImageView in_view = VK_NULL_HANDLE;
    uint32_t in_w = 0, in_h = 0;
    int target_w = 0, target_h = 0;
    bool ok = false;
    std::vector<Image> images;
    std::vector<Step> steps;
    MpvImage out = {};
} plan;

void free_images()
{
    for (Image &im : plan.images) {
        if (im.fb)
            vkDestroyFramebuffer(gfx.device, im.fb, nullptr);
        if (im.view)
            vkDestroyImageView(gfx.device, im.view, nullptr);
        if (im.image)
            vkDestroyImage(gfx.device, im.image, nullptr);
        if (im.memory)
            vkFreeMemory(gfx.device, im.memory, nullptr);
    }
    plan.images.clear();
    plan.steps.clear();
    vkResetDescriptorPool(gfx.device, pool, 0);
}

/* A size: name -> (w, h); OUTPUT is the target */
struct Size {
    double w, h;
};

/* mpv's reverse-Polish expression ("LUMA.w 2 *", "OUTPUT.w MAIN.w / 1.2 >"):
 * numbers, NAME.w / NAME.h, + - * / > < = ! */
bool eval(const char *expr, const std::map<std::string, Size> &sizes, double *out)
{
    std::vector<double> st;
    const char *p = expr;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        std::string tok(p, e - p);
        p = e;
        if (tok.size() == 1 && strchr("+-*/><=", tok[0])) {
            if (st.size() < 2)
                return false;
            double b = st.back();
            st.pop_back();
            double a = st.back();
            st.pop_back();
            double r = tok[0] == '+' ? a + b : tok[0] == '-' ? a - b : tok[0] == '*' ? a * b
                     : tok[0] == '/' ? (b != 0 ? a / b : 0) : tok[0] == '>' ? (a > b)
                     : tok[0] == '<' ? (a < b) : (a == b);
            st.push_back(r);
        } else if (tok == "!") {
            if (st.empty())
                return false;
            st.back() = !st.back();
        } else if (tok.size() > 2 && (tok.compare(tok.size() - 2, 2, ".w") == 0 ||
                                      tok.compare(tok.size() - 2, 2, ".h") == 0)) {
            auto it = sizes.find(tok.substr(0, tok.size() - 2));
            if (it == sizes.end())
                return false;
            st.push_back(tok.back() == 'w' ? it->second.w : it->second.h);
        } else {
            char *end;
            double v = strtod(tok.c_str(), &end);
            if (*end)
                return false;
            st.push_back(v);
        }
    }
    if (st.size() != 1)
        return false;
    *out = st[0];
    return true;
}

bool make_pipelines(int set)
{
    if (!pipelines[set].empty())
        return true;
    VkShaderModule vs = gfx_shader(spv_fullscreen_vert, sizeof(spv_fullscreen_vert));
    if (!vs)
        return false;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att = {};
    att.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout;
    gp.renderPass = render_pass;
    bool ok = true;
    for (int i = 0; i < sets[set].count && ok; i++) {
        VkShaderModule fs = gfx_shader(sets[set].passes[i].spv, sets[set].passes[i].size);
        VkPipeline p = VK_NULL_HANDLE;
        stages[1].module = fs;
        ok = fs && vkCreateGraphicsPipelines(gfx.device, VK_NULL_HANDLE, 1, &gp, nullptr, &p) == VK_SUCCESS;
        if (fs)
            vkDestroyShaderModule(gfx.device, fs, nullptr);
        pipelines[set].push_back(p);
    }
    vkDestroyShaderModule(gfx.device, vs, nullptr);
    if (!ok) {
        fprintf(stderr, "mpv shaders: %s: a pipeline failed\n", sets[set].name);
        for (VkPipeline p : pipelines[set])
            if (p)
                vkDestroyPipeline(gfx.device, p, nullptr);
        pipelines[set].clear();
        return false;
    }
    fprintf(stderr, "mpv shaders: %s ready (%d passes)\n", sets[set].name, sets[set].count);
    return true;
}

/* Which passes run, their sizes, their textures: for this input and target.
 * Images are shared: a pass's result goes into an image of its size whose
 * last reader ran before it (Anime4K VL is 35 passes: an image each was
 * hundreds of MB at 1080p). */
bool make_plan(int set, const MpvImage &in, int target_w, int target_h)
{
    vkDeviceWaitIdle(gfx.device); /* the images before may still be read */
    free_images();
    plan_id++;
    plan.set = set;
    plan.in_view = in.view;
    plan.in_w = in.w;
    plan.in_h = in.h;
    plan.target_w = target_w;
    plan.target_h = target_h;
    plan.ok = false;
    const SetInfo &si = sets[set];
    const char *hook_name = si.passes[0].hook;

    /* 1: the passes that run, their sizes, and what each reads (-1 the
     * input, else the pass before whose result it is) */
    struct Logical {
        int pass;
        uint32_t w, h;
        int reads[MAX_BINDS];
        int last_use;
    };
    std::vector<Logical> lg;
    std::map<std::string, int> tex;
    std::map<std::string, Size> sizes;
    tex[hook_name] = -1;
    sizes[hook_name] = { (double)in.w, (double)in.h };
    sizes["OUTPUT"] = { (double)target_w, (double)target_h };
    int hooked = -1;
    Size hooked_size = { (double)in.w, (double)in.h };
    for (int i = 0; i < si.count; i++) {
        const MpvPassInfo &p = si.passes[i];
        sizes["HOOKED"] = hooked_size;
        double when = 1;
        if (*p.when && !eval(p.when, sizes, &when)) {
            fprintf(stderr, "mpv shaders: %s pass %d: can't read \"%s\"\n", si.name, i, p.when);
            return false;
        }
        if (!when)
            continue;
        double w = hooked_size.w, h = hooked_size.h;
        if ((*p.width && !eval(p.width, sizes, &w)) || (*p.height && !eval(p.height, sizes, &h))) {
            fprintf(stderr, "mpv shaders: %s pass %d: can't read its size\n", si.name, i);
            return false;
        }
        Logical l = { i, (uint32_t)lround(w), (uint32_t)lround(h), {}, 0 };
        if (l.w < 1 || l.h < 1 || l.w > 8192 || l.h > 8192)
            return false;
        int me = (int)lg.size();
        for (int b = 0; b < p.nbinds; b++) {
            int t;
            if (!strcmp(p.binds[b], "HOOKED")) {
                t = hooked;
            } else {
                auto it = tex.find(p.binds[b]);
                if (it == tex.end()) {
                    fprintf(stderr, "mpv shaders: %s pass %d reads %s, made by no pass before\n", si.name, i,
                            p.binds[b]);
                    return false;
                }
                t = it->second;
            }
            l.reads[b] = t;
            if (t >= 0)
                lg[t].last_use = me;
        }
        l.last_use = me;
        lg.push_back(l);
        /* the result: under its own name, or the hooked texture from now on */
        std::string name = *p.save ? p.save : hook_name;
        tex[name] = me;
        sizes[name] = { (double)l.w, (double)l.h };
        if (name == hook_name) {
            hooked = me;
            hooked_size = { (double)l.w, (double)l.h };
        }
    }
    if (hooked < 0) {
        fprintf(stderr, "mpv shaders: %s: not for this size (%ux%u to %dx%d)\n", si.name, in.w, in.h,
                target_w, target_h);
        return false;
    }
    lg[hooked].last_use = INT32_MAX; /* read after the set: never shared */

    /* 2: images, shared once their last reader has run */
    std::vector<int> busy_until;   /* per image: the last pass that reads it */
    std::vector<int> image_of(lg.size());
    for (size_t s = 0; s < lg.size(); s++) {
        int found = -1;
        for (size_t k = 0; k < plan.images.size(); k++)
            if (busy_until[k] < (int)s && plan.images[k].w == lg[s].w && plan.images[k].h == lg[s].h) {
                found = (int)k;
                break;
            }
        if (found < 0) {
            Image im;
            im.w = lg[s].w;
            im.h = lg[s].h;
            bool made = gfx_image(im.w, im.h, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &im.image,
                                  &im.memory, &im.view);
            if (made) {
                VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                fi.renderPass = render_pass;
                fi.attachmentCount = 1;
                fi.pAttachments = &im.view;
                fi.width = im.w;
                fi.height = im.h;
                fi.layers = 1;
                made = vkCreateFramebuffer(gfx.device, &fi, nullptr, &im.fb) == VK_SUCCESS;
            }
            plan.images.push_back(im);
            busy_until.push_back(0);
            if (!made) {
                fprintf(stderr, "mpv shaders: no memory for a %ux%u image\n", im.w, im.h);
                return false;
            }
            found = (int)plan.images.size() - 1;
        }
        busy_until[found] = lg[s].last_use;
        image_of[s] = found;
    }

    /* 3: each pass's textures */
    for (size_t s = 0; s < lg.size(); s++) {
        const MpvPassInfo &p = si.passes[lg[s].pass];
        Step st = { lg[s].pass, image_of[s], VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        if (vkAllocateDescriptorSets(gfx.device, &ai, &st.set) != VK_SUCCESS)
            return false;
        VkDescriptorImageInfo infos[MAX_BINDS];
        VkWriteDescriptorSet writes[MAX_BINDS] = {};
        for (int b = 0; b < p.nbinds; b++) {
            int t = lg[s].reads[b];
            infos[b] = { linear, t < 0 ? in.view : plan.images[image_of[t]].view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            writes[b] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[b].dstSet = st.set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &infos[b];
        }
        vkUpdateDescriptorSets(gfx.device, p.nbinds, writes, 0, nullptr);
        plan.steps.push_back(st);
    }
    const Image &last = plan.images[image_of[hooked]];
    plan.out = { last.view, last.w, last.h };
    plan.ok = true;
    double mb = 0;
    for (const Image &im : plan.images)
        mb += im.w * (double)im.h * 8 / (1024 * 1024);
    fprintf(stderr, "mpv shaders: %s, %ux%u to %ux%u, %d passes, %d images (%.0f MB)\n", si.name, in.w, in.h,
            plan.out.w, plan.out.h, (int)plan.steps.size(), (int)plan.images.size(), mb);
    return true;
}

} // namespace

bool mpv_init(VkRenderPass pass, VkSampler sampler)
{
    render_pass = pass;
    linear = sampler;
    VkDescriptorSetLayoutBinding b[MAX_BINDS] = {};
    for (int i = 0; i < MAX_BINDS; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = MAX_BINDS;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(gfx.device, &li, nullptr, &set_layout) != VK_SUCCESS)
        return false;
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(gfx.device, &pl, nullptr, &layout) != VK_SUCCESS)
        return false;
    /* enough for the biggest set: a set per pass, 16 textures each */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 * MAX_BINDS };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 64;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    return vkCreateDescriptorPool(gfx.device, &pi, nullptr, &pool) == VK_SUCCESS;
}

unsigned mpv_plan_id()
{
    return plan_id;
}

const char *mpv_set_name(int set)
{
    return set >= 0 && set < MPV_SET_COUNT ? sets[set].name : "";
}

bool mpv_record(VkCommandBuffer cmd, int set, const MpvImage &in, float target_w, float target_h, MpvImage *out)
{
    *out = in;
    if (set < 0 || set >= MPV_SET_COUNT || !pool || !make_pipelines(set))
        return false;
    int tw = (int)lroundf(target_w), th = (int)lroundf(target_h);
    if (plan.set != set || plan.in_view != in.view || plan.in_w != in.w || plan.in_h != in.h ||
        plan.target_w != tw || plan.target_h != th) {
        if (!make_plan(set, in, tw, th))
            plan.ok = false;
    }
    if (!plan.ok)
        return false;
    const SetInfo &si = sets[set];
    for (const Step &st : plan.steps) {
        const Image &im = plan.images[st.image];
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = render_pass;
        bi.framebuffer = im.fb;
        bi.renderArea = { { 0, 0 }, { im.w, im.h } };
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp = { 0, 0, (float)im.w, (float)im.h, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { im.w, im.h } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[set][st.pass]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &st.set, 0, nullptr);
        Push p = { { (float)im.w, (float)im.h, 1.0f / im.w, 1.0f / im.h }, { target_w, target_h, 0, 0 } };
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
    (void)si;
    *out = plan.out;
    return true;
}
