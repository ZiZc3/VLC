/*
 * VLC-PS5's player.
 *
 * Pictures: VLC's vmem output writes each picture straight into one of a few
 * host-visible Vulkan buffers (the PS5's memory is shared, so this is the only
 * CPU copy, VLC's own). A slot moves FREE -> WRITING (vmem lock) -> READY
 * (unlock) -> SHOWN (display: VLC's clock says "now") -> GPU (the render thread
 * copied it into the plane textures) -> FREE once that frame's fence passed.
 * The render thread draws the planes with shaders/video.frag.
 *
 * Sound: amem hands us 48 kHz S16 stereo with each block's play time. The
 * audio thread plays a block when VLC's clock reaches that time (silence
 * before, drops if late), so lip sync doesn't depend on how far ahead VLC
 * decodes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "player.h"

#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <math.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

#include <vlc/vlc.h>
/* libvlc's player is a VLC object; a few options have no libvlc call (the
 * subtitle look, Auto deinterlace), so they're set as its variables, which
 * its video output and subtitle renderer inherit. */
#include <vlc_common.h>
#include <vlc_variables.h>
#include <vlc_url.h>

#include "gen/video_frag.h"
#include "gen/video_vert.h"
#include "gfx.h"
#include "image.h"
#include "lang.h"
#include "platform.h"
#include "prefs.h"

namespace {

libvlc_instance_t *vlc;
libvlc_media_player_t *mp;
std::string current_path;
VideoInfo info;
int info_tries;

/* Decoding speed. A file the CPU can't decode in real time makes VLC drop
 * every late picture, so the screen freezes while the sound and the clock go
 * on. Fast decoding skips H.264/HEVC/VP9's loop filter (softer picture, much
 * less work); Auto switches to it when a file keeps losing pictures. */
DecodeMode decode_mode = DECODE_AUTO;
bool fast_now;             /* the current file was opened with fast decoding */
std::string fast_path;     /* the file Auto switched, so a reopen keeps it */
struct Health {
    double next;           /* next check (plat_time) */
    int bad_seconds;
    bool gave_up;          /* already fast and still too slow: said so once */
    bool primed;           /* last holds a real count (not the opening's total) */
    libvlc_media_stats_t last;
} health;
libvlc_media_stats_t debug_last;
int restore_audio = -2, restore_spu = -2; /* tracks to pick again after a reopen */

/* What was changed on the file playing: kept when the same file opens again
 * (Watch again, Loop, a restart), forgotten for another one. */
struct Kept {
    std::string path;
    int64_t audio_delay, spu_delay;   /* ms */
    int audio, spu;                   /* -2: not picked */
    std::vector<std::string> subs;    /* subtitle files added */
};
Kept kept = { "", 0, 0, -2, -2, {} };
bool restore_delays;
/* A subtitle file added while playing: its track should show up within a
 * few seconds; if VLC couldn't take it that way, the file reopens where it
 * was with the subtitle attached from the start. */
double sub_check_at;
size_t sub_check_count;
bool sub_attach_all;   /* the reopen: every added file, wherever it is */

/* ---- pictures -------------------------------------------------------------- */

enum SlotState { FREE, WRITING, READY, SHOWN, GPU };
const int SLOTS = 5;
const int PLANES = 3;

struct Slot {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    SlotState state;
    uint64_t gpu_frame; /* the frame whose fence frees it */
};

struct Format {
    uint32_t width, height;        /* buffer size, as VLC writes it */
    uint32_t pitches[PLANES], lines[PLANES];
    VkDeviceSize offsets[PLANES];
    VkDeviceSize size;
    bool ten_bit, full_range;
};

std::mutex pic_lock;
std::condition_variable pic_freed;
Slot slots[SLOTS];
Format format;
int generation;        /* bumped by every vmem setup */
int shown = -1;        /* the slot VLC displayed last, not uploaded yet */
std::atomic<uint64_t> completed_frame{ 0 };

struct Garbage {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint64_t after_frame;
};
std::vector<Garbage> garbage;

/* Render thread's side. */
struct Planes {
    VkImage image[PLANES];
    VkDeviceMemory memory[PLANES];
    VkImageView view[PLANES];
    uint32_t w[PLANES], h[PLANES];
    int generation = -1;
    bool has_picture;
    bool ten_bit, full_range;
    uint32_t buf_w, buf_h;
} planes;

VkSampler sampler;
VkDescriptorSetLayout set_layout;
VkDescriptorPool desc_pool;
VkDescriptorSet desc_set;
VkPipelineLayout pipe_layout;
VkPipeline pipeline;

struct Push {
    float row0[4], row1[4], row2[4];
    float params[4];   /* sample scale, limited range, hue (radians), 360° (1) */
    float uv_scale[2];
    float pad[2];
    float adjust[4];   /* brightness, contrast, saturation, gamma exponent */
    float view[4];     /* 360°: yaw, pitch (radians), tan(fov / 2), screen aspect */
    float xform[4];    /* quarter turns clockwise, mirror, upside down, sharpen */
};

/* where a 360° video is looked at */
float view_yaw, view_pitch, view_fov = 90;

PictureAdjust picture;
int last_slot = -1; /* the slot last copied to the screen (screenshots) */

uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void free_slots_locked(uint64_t after_frame)
{
    for (Slot &s : slots) {
        if (s.buffer)
            garbage.push_back({ s.buffer, s.memory, after_frame });
        s = Slot{};
    }
    shown = -1;
}

unsigned vmem_setup(void **opaque, char *chroma, unsigned *width, unsigned *height,
                    unsigned *pitches, unsigned *lines)
{
    (void)opaque;
    bool ten = !strncmp(chroma, "I0A", 3) || !strncmp(chroma, "P010", 4) ||
               !strncmp(chroma, "I2A", 3) || !strncmp(chroma, "I4A", 3);
    bool full = !strncmp(chroma, "J4", 2);
    fprintf(stderr, "player: VLC offers %.4s %ux%u\n", chroma, *width, *height);
    memcpy(chroma, ten ? "I0AL" : full ? "J420" : "I420", 4);

    std::lock_guard<std::mutex> g(pic_lock);
    free_slots_locked(gfx.frame_number + GFX_FRAMES + 1);
    Format f = {};
    f.width = *width;
    f.height = *height;
    f.ten_bit = ten;
    f.full_range = full;
    uint32_t bpp = ten ? 2 : 1;
    uint32_t cw = (*width + 1) / 2, ch = (*height + 1) / 2;
    /* Rows on 256-byte boundaries: what buffer-to-image copies like best. */
    f.pitches[0] = align_up(*width * bpp, 256);
    f.lines[0] = align_up(*height, 16);
    f.pitches[1] = f.pitches[2] = align_up(cw * bpp, 256);
    f.lines[1] = f.lines[2] = align_up(ch, 16);
    VkDeviceSize off = 0;
    for (int i = 0; i < PLANES; i++) {
        f.offsets[i] = off;
        off += (VkDeviceSize)f.pitches[i] * f.lines[i];
        off = (off + 255) & ~(VkDeviceSize)255;
        pitches[i] = f.pitches[i];
        lines[i] = f.lines[i];
    }
    f.size = off;
    for (Slot &s : slots) {
        void *mapped = nullptr;
        if (!gfx_buffer(f.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &s.buffer, &s.memory, &mapped)) {
            fprintf(stderr, "player: no memory for pictures\n");
            return 0;
        }
        s.mapped = (uint8_t *)mapped;
        s.state = FREE;
    }
    format = f;
    generation++;
    fprintf(stderr, "player: pictures %ux%u %s%s, %d x %llu KiB\n", f.width, f.height,
            ten ? "10-bit" : "8-bit", full ? " full range" : "", SLOTS,
            (unsigned long long)(f.size / 1024));
    return SLOTS;
}

void vmem_cleanup(void *opaque)
{
    (void)opaque;
    std::lock_guard<std::mutex> g(pic_lock);
    free_slots_locked(gfx.frame_number + GFX_FRAMES + 1);
}

void reclaim_locked()
{
    uint64_t done = completed_frame.load();
    for (Slot &s : slots)
        if (s.state == GPU && s.gpu_frame <= done)
            s.state = FREE;
}

void *vmem_lock(void *opaque, void **out)
{
    (void)opaque;
    std::unique_lock<std::mutex> g(pic_lock);
    int found = -1;
    for (int tries = 0; found < 0 && tries < 20; tries++) {
        reclaim_locked();
        for (int i = 0; i < SLOTS; i++)
            if (slots[i].state == FREE && slots[i].buffer) {
                found = i;
                break;
            }
        if (found < 0)
            pic_freed.wait_for(g, std::chrono::milliseconds(5));
    }
    if (found < 0) {
        /* The screen isn't taking pictures (paused render, slow frame): reuse
         * the oldest ready one rather than block VLC. */
        for (int i = 0; i < SLOTS; i++)
            if (slots[i].state == READY) {
                found = i;
                break;
            }
    }
    if (found < 0 || !slots[found].buffer) {
        /* Shouldn't happen; give VLC somewhere harmless to write. */
        static uint8_t *scratch;
        static VkDeviceSize scratch_size;
        if (scratch_size < format.size) {
            free(scratch);
            scratch = (uint8_t *)malloc(format.size);
            scratch_size = format.size;
        }
        for (int i = 0; i < PLANES; i++)
            out[i] = scratch + format.offsets[i];
        return (void *)(intptr_t)-1;
    }
    Slot &s = slots[found];
    s.state = WRITING;
    for (int i = 0; i < PLANES; i++)
        out[i] = s.mapped + format.offsets[i];
    return (void *)(intptr_t)found;
}

void vmem_unlock(void *opaque, void *picture, void *const *p)
{
    (void)opaque; (void)p;
    int i = (int)(intptr_t)picture;
    std::lock_guard<std::mutex> g(pic_lock);
    if (i >= 0 && i < SLOTS && slots[i].state == WRITING)
        slots[i].state = READY;
}

void vmem_display(void *opaque, void *picture)
{
    (void)opaque;
    int i = (int)(intptr_t)picture;
    std::lock_guard<std::mutex> g(pic_lock);
    if (i < 0 || i >= SLOTS || slots[i].state != READY)
        return;
    if (shown >= 0 && shown != i && slots[shown].state == SHOWN)
        slots[shown].state = FREE; /* never drawn: the screen was slower */
    slots[i].state = SHOWN;
    shown = i;
}

/* ---- sound ------------------------------------------------------------------- */

struct AudioBlock {
    std::vector<int16_t> samples; /* interleaved, `channels` per frame */
    int channels;                 /* 2, or 6 (L R Lr Rr C LFE) / 8 (L R Lm Rm Lr Rr C LFE): VLC's order */
    size_t pos;                   /* frames already played */
    int64_t pts;                  /* libvlc_clock() time of its first frame */
};

std::mutex audio_lock;
std::deque<AudioBlock> audio_queue;
bool audio_paused;
float audio_gain = 1.0f; /* VLC's (mute) */
float user_gain = 1.0f;  /* the app's volume */
int vlc_channels = 2;    /* what VLC was asked for (amem_setup) */
bool night_mode;
std::atomic<int> tone_channel{ -1 }; /* speaker test: a console channel 0-7, or -1 */
std::atomic<bool> audio_running{ false };
std::thread audio_thread;
/* How long after plat_audio_output() returns its grain is heard: the grain
 * being played plus the one queued. */
const int64_t AUDIO_LATENCY_US = 2 * AUDIO_GRAIN * 1000000LL / AUDIO_RATE;

/* Per file, when VLC starts the sound: stereo, or with Surround on and a
 * multichannel file, 6 or 8 channels for the console's 8-channel port. */
int amem_setup(void **opaque, char *format, unsigned *rate, unsigned *channels)
{
    (void)opaque;
    memcpy(format, "S16N", 4);
    *rate = AUDIO_RATE;
    unsigned from = *channels, want = 2;
    if (from > 2 && pref_int("audio_out", 0) == 1 && plat_audio_open_surround())
        want = from >= 7 ? 8 : 6;
    *channels = want;
    std::lock_guard<std::mutex> g(audio_lock);
    vlc_channels = (int)want;
    fprintf(stderr, "player: sound has %u channels, playing %u\n", from, want);
    return 0;
}

/* True while a stop runs in the background: what VLC still sends is dropped. */
std::atomic<bool> stopping;

void amem_play(void *opaque, const void *samples, unsigned count, int64_t pts)
{
    (void)opaque;
    if (stopping)
        return;
    AudioBlock b;
    std::lock_guard<std::mutex> g(audio_lock);
    b.channels = vlc_channels;
    b.samples.assign((const int16_t *)samples, (const int16_t *)samples + (size_t)count * b.channels);
    b.pos = 0;
    b.pts = pts;
    if (audio_queue.size() > 400) /* ~4 s: something is stuck; keep memory bounded */
        audio_queue.pop_front();
    audio_queue.push_back(std::move(b));
}

void amem_pause(void *opaque, int64_t pts)
{
    (void)opaque; (void)pts;
    std::lock_guard<std::mutex> g(audio_lock);
    audio_paused = true;
}

void amem_resume(void *opaque, int64_t pts)
{
    (void)opaque; (void)pts;
    std::lock_guard<std::mutex> g(audio_lock);
    audio_paused = false;
}

void amem_flush(void *opaque, int64_t pts)
{
    (void)opaque; (void)pts;
    std::lock_guard<std::mutex> g(audio_lock);
    audio_queue.clear();
}

void amem_drain(void *opaque)
{
    (void)opaque;
    for (int i = 0; i < 200; i++) {
        {
            std::lock_guard<std::mutex> g(audio_lock);
            if (audio_queue.empty())
                return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void amem_volume(void *opaque, float volume, bool mute)
{
    (void)opaque;
    std::lock_guard<std::mutex> g(audio_lock);
    audio_gain = mute ? 0.0f : volume;
}

/* One frame from VLC's order into the output's: the console's 8 channels are
 * L R C LFE Ls Rs Lb Rb. 5.1's rears are its surrounds; 7.1's sides are the
 * surrounds and its rears the backs. Surround into stereo (between files):
 * the usual downmix. */
void map_frame(const int16_t *in, int in_ch, float *out, int out_ch, float k)
{
    if (out_ch == 2) {
        if (in_ch == 2) {
            out[0] += in[0] * k;
            out[1] += in[1] * k;
        } else {
            int c = in_ch == 6 ? 4 : 6, sl = 2, sr = 3;
            out[0] += (in[0] + 0.707f * in[c] + 0.707f * in[sl]) * k * 0.6f;
            out[1] += (in[1] + 0.707f * in[c] + 0.707f * in[sr]) * k * 0.6f;
        }
        return;
    }
    if (in_ch == 2) {
        out[0] += in[0] * k;
        out[1] += in[1] * k;
    } else if (in_ch == 6) {
        out[0] += in[0] * k; out[1] += in[1] * k;
        out[2] += in[4] * k; out[3] += in[5] * k;
        out[4] += in[2] * k; out[5] += in[3] * k;
    } else {
        out[0] += in[0] * k; out[1] += in[1] * k;
        out[2] += in[6] * k; out[3] += in[7] * k;
        out[4] += in[2] * k; out[5] += in[3] * k;
        out[6] += in[4] * k; out[7] += in[5] * k;
    }
}

/* Night mode: quiet the loud parts (4:1 above -18 dBFS), then lift
 * everything, so voices stay clear at a low volume. */
void compress(float *buf, int frames, int ch)
{
    static float env;
    const float atk = 1 - expf(-1.0f / (0.005f * AUDIO_RATE));
    const float rel = 1 - expf(-1.0f / (0.25f * AUDIO_RATE));
    const float thr = 0.125f * 32768, makeup = 2.0f;
    for (int f = 0; f < frames; f++) {
        float *x = buf + f * ch, peak = 0;
        for (int c = 0; c < ch; c++)
            peak = std::max(peak, fabsf(x[c]));
        env += (peak - env) * (peak > env ? atk : rel);
        float g = env > thr ? thr * powf(env / thr, 0.25f) / env : 1.0f;
        for (int c = 0; c < ch; c++)
            x[c] *= g * makeup;
    }
}

/* The last samples sent to the speakers (mono), for the visualizer. */
float vis_ring[4096];
std::atomic<unsigned> vis_pos;

void audio_main()
{
    static float mix[AUDIO_GRAIN * 8];
    static int16_t out[AUDIO_GRAIN * 8];
    double tone_phase = 0;
    while (audio_running) {
        int out_ch = 2;
        bool night;
        int tone = tone_channel.load();
        {
            std::lock_guard<std::mutex> g(audio_lock);
            night = night_mode;
            if (tone >= 0 || (!audio_queue.empty() && audio_queue.front().channels > 2))
                out_ch = 8;
            memset(mix, 0, sizeof(float) * AUDIO_GRAIN * out_ch);
            size_t filled = 0;
            float k = audio_gain * user_gain;
            /* The time this grain will be heard. */
            int64_t heard = libvlc_clock() + AUDIO_LATENCY_US;
            while (!audio_paused && filled < AUDIO_GRAIN && !audio_queue.empty()) {
                AudioBlock &b = audio_queue.front();
                size_t frames = b.samples.size() / b.channels;
                int64_t at = b.pts + (int64_t)b.pos * 1000000 / AUDIO_RATE;
                int64_t slot_time = heard + (int64_t)filled * 1000000 / AUDIO_RATE;
                if (b.pos == 0 && at > slot_time + 15000) {
                    /* Early: silence until it is due. */
                    size_t wait = (size_t)((at - slot_time) * AUDIO_RATE / 1000000);
                    filled += wait < AUDIO_GRAIN - filled ? wait : AUDIO_GRAIN - filled;
                    break;
                }
                if (at < slot_time - 60000) {
                    /* Late by more than 60 ms (after a stall): drop to catch up. */
                    size_t skip = (size_t)((slot_time - at) * AUDIO_RATE / 1000000);
                    b.pos += skip;
                    if (b.pos >= frames)
                        audio_queue.pop_front();
                    continue;
                }
                size_t n = frames - b.pos;
                if (n > AUDIO_GRAIN - filled)
                    n = AUDIO_GRAIN - filled;
                for (size_t f = 0; f < n; f++)
                    map_frame(&b.samples[(b.pos + f) * b.channels], b.channels,
                              &mix[(filled + f) * out_ch], out_ch, k);
                filled += n;
                b.pos += n;
                if (b.pos >= frames)
                    audio_queue.pop_front();
            }
        }
        if (tone >= 0 && tone < out_ch) {
            /* Speaker test: pink-ish beeps on one channel (LFE: a low hum). */
            double hz = tone == 3 ? 55 : 440;
            for (int f = 0; f < AUDIO_GRAIN; f++) {
                tone_phase += 2 * 3.14159265358979 * hz / AUDIO_RATE;
                mix[f * out_ch + tone] += (float)(sin(tone_phase) * 0.25 * 32767);
            }
        }
        if (night)
            compress(mix, AUDIO_GRAIN, out_ch);
        {
            unsigned at = vis_pos.load(std::memory_order_relaxed);
            for (int f = 0; f < AUDIO_GRAIN; f++)
                vis_ring[(at + f) & 4095] = (mix[f * out_ch] + mix[f * out_ch + 1]) * (0.5f / 32768.0f);
            vis_pos.store(at + AUDIO_GRAIN, std::memory_order_relaxed);
        }
        for (int i = 0; i < AUDIO_GRAIN * out_ch; i++) {
            float v = mix[i];
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (out_ch == 8)
            plat_audio_output_surround(out);
        else
            plat_audio_output(out);
    }
}

/* ---- libvlc ------------------------------------------------------------------- */

void read_info()
{
    unsigned w = 0, h = 0;
    if (libvlc_video_get_size(mp, 0, &w, &h) != 0 || !w || !h)
        return;
    VideoInfo v = {};
    v.width = w;
    v.height = h;
    v.aspect = (float)w / h;
    libvlc_media_t *m = libvlc_media_player_get_media(mp);
    if (m) {
        libvlc_media_track_t **tracks;
        unsigned n = libvlc_media_tracks_get(m, &tracks);
        bool have_video = false, have_audio = false;
        for (unsigned i = 0; i < n; i++) {
            libvlc_media_track_t *t = tracks[i];
            const char *d = libvlc_media_get_codec_description(t->i_type, t->i_codec);
            if (t->i_type == libvlc_track_video && !have_video) {
                have_video = true;
                v.codec = d ? d : "";
                if (t->video->i_frame_rate_num && t->video->i_frame_rate_den)
                    v.fps = (float)t->video->i_frame_rate_num / t->video->i_frame_rate_den;
                if (t->video->i_sar_num && t->video->i_sar_den)
                    v.aspect = (float)w * t->video->i_sar_num / ((float)h * t->video->i_sar_den);
                v.spherical = t->video->i_projection == libvlc_video_projection_equirectangular;
            } else if (t->i_type == libvlc_track_audio && !have_audio) {
                have_audio = true;
                v.audio_codec = d ? d : "";
                v.audio_channels = t->audio->i_channels;
            }
        }
        libvlc_media_tracks_release(tracks, n);
        libvlc_media_release(m);
    }
    std::lock_guard<std::mutex> g(pic_lock);
    v.ten_bit = format.ten_bit;
    info = v;
}

std::vector<Track> tracks_from(libvlc_track_description_t *list)
{
    std::vector<Track> out;
    for (libvlc_track_description_t *t = list; t; t = t->p_next) {
        /* VLC names a track without a title "Track 1": said in the menus' language */
        std::string name = t->psz_name ? t->psz_name : "";
        if (name.compare(0, 6, "Track ") == 0 && name.size() > 6 && isdigit((unsigned char)name[6]))
            name = trf("Track %d", atoi(name.c_str() + 6));
        out.push_back({ t->i_id, name.empty() ? std::string(tr("Track")) : name });
    }
    if (list)
        libvlc_track_description_list_release(list);
    return out;
}

bool create_pipeline()
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (vkCreateSampler(gfx.device, &si, nullptr, &sampler) != VK_SUCCESS)
        return false;
    VkDescriptorSetLayoutBinding b[PLANES] = {};
    for (int i = 0; i < PLANES; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = PLANES;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(gfx.device, &li, nullptr, &set_layout) != VK_SUCCESS)
        return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PLANES };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(gfx.device, &pi, nullptr, &desc_pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    if (vkAllocateDescriptorSets(gfx.device, &ai, &desc_set) != VK_SUCCESS)
        return false;
    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(Push) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(gfx.device, &pl, nullptr, &pipe_layout) != VK_SUCCESS)
        return false;

    VkShaderModule vs = gfx_shader(spv_video_vert, sizeof(spv_video_vert));
    VkShaderModule fs = gfx_shader(spv_video_frag, sizeof(spv_video_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
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
    gp.layout = pipe_layout;
    gp.renderPass = gfx.render_pass;
    VkResult r = vkCreateGraphicsPipelines(gfx.device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
    vkDestroyShaderModule(gfx.device, vs, nullptr);
    vkDestroyShaderModule(gfx.device, fs, nullptr);
    return r == VK_SUCCESS;
}

void destroy_planes()
{
    for (int i = 0; i < PLANES; i++) {
        if (planes.view[i])
            vkDestroyImageView(gfx.device, planes.view[i], nullptr);
        if (planes.image[i])
            vkDestroyImage(gfx.device, planes.image[i], nullptr);
        if (planes.memory[i])
            vkFreeMemory(gfx.device, planes.memory[i], nullptr);
        planes.view[i] = VK_NULL_HANDLE;
        planes.image[i] = VK_NULL_HANDLE;
        planes.memory[i] = VK_NULL_HANDLE;
    }
}

/* New format from VLC: plane textures to match (render thread). */
bool make_planes(const Format &f)
{
    vkDeviceWaitIdle(gfx.device); /* the old textures may still be read */
    destroy_planes();
    VkFormat vf = f.ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    VkCommandBuffer cmd = gfx_one_shot_begin();
    for (int i = 0; i < PLANES; i++) {
        planes.w[i] = i ? (f.width + 1) / 2 : f.width;
        planes.h[i] = i ? (f.height + 1) / 2 : f.height;
        if (!gfx_image(planes.w[i], planes.h[i], vf,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       &planes.image[i], &planes.memory[i], &planes.view[i])) {
            gfx_one_shot_end(cmd);
            return false;
        }
        gfx_barrier(cmd, planes.image[i], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    gfx_one_shot_end(cmd);
    VkDescriptorImageInfo ii[PLANES];
    VkWriteDescriptorSet w[PLANES] = {};
    for (int i = 0; i < PLANES; i++) {
        ii[i] = { sampler, planes.view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = desc_set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(gfx.device, PLANES, w, 0, nullptr);
    planes.ten_bit = f.ten_bit;
    planes.full_range = f.full_range;
    planes.buf_w = f.width;
    planes.buf_h = f.height;
    planes.has_picture = false;
    return true;
}

bool log_debug;

/* settings.txt in the app folder: "key=value" lines. */
int setting_int(const char *key, int fallback)
{
    std::string path = std::string(plat_data_dir()) + "/settings.txt";
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return fallback;
    char line[256];
    int value = fallback;
    size_t n = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == '=')
            value = atoi(line + n + 1);
    fclose(f);
    return value;
}

void vlc_log(void *data, int level, const libvlc_log_t *ctx, const char *fmt, va_list args)
{
    (void)data;
    /* Normally errors only (a warning flood, "picture is too late", costs
     * nothing on screen now but fills the log); debug=1 in settings.txt: all. */
    if (level < LIBVLC_ERROR && !log_debug)
        return;
    static const char *const names[] = { "debug", "", "info", "warning", "error" };
    const char *module = nullptr, *file = nullptr;
    unsigned line = 0;
    libvlc_log_get_context(ctx, &module, &file, &line);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    /* The console logger's own option, asked for by others; it isn't linked. */
    if (!strcmp(msg, "option quiet does not exist"))
        return;
    fprintf(stderr, "vlc %s %s: %s\n", module ? module : "?", level <= 4 ? names[level] : "",
            msg);
}

/* How many cores really run at once: VLC decodes on one thread per core, so
 * if the console kept our threads on fewer cores than it has, this shows it. */
void cpu_probe()
{
    const int THREADS = 8;
    auto work = [] {
        volatile uint64_t x = 1;
        for (int i = 0; i < 4000000; i++)
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    };
    double t0 = plat_time();
    work();
    double one = plat_time() - t0;
    t0 = plat_time();
    std::thread th[THREADS];
    for (std::thread &t : th)
        t = std::thread(work);
    for (std::thread &t : th)
        t.join();
    double all = plat_time() - t0;
    fprintf(stderr, "cpu: one thread %.1f ms, %d at once %.1f ms: about %.1f cores at work "
            "(hardware_concurrency %u)\n", one * 1000, THREADS, all * 1000,
            all > 0 ? THREADS * one / all : 0, std::thread::hardware_concurrency());
}

void save_decode_mode()
{
    std::string path = std::string(plat_data_dir()) + "/decode.txt";
    if (FILE *f = fopen(path.c_str(), "w")) {
        fprintf(f, "%d\n", (int)decode_mode);
        fclose(f);
    }
}

void load_decode_mode()
{
    std::string path = std::string(plat_data_dir()) + "/decode.txt";
    if (FILE *f = fopen(path.c_str(), "r")) {
        int m = 0;
        if (fscanf(f, "%d", &m) == 1 && m >= DECODE_AUTO && m <= DECODE_FULL)
            decode_mode = (DecodeMode)m;
        fclose(f);
    }
}

bool get_stats(libvlc_media_stats_t *s)
{
    libvlc_media_t *m = mp ? libvlc_media_player_get_media(mp) : nullptr;
    if (!m)
        return false;
    bool ok = libvlc_media_get_stats(m, s) != 0;
    libvlc_media_release(m);
    return ok;
}

vlc_object_t *mp_object()
{
    return (vlc_object_t *)mp;
}

void set_mp_int(const char *name, int64_t value)
{
    var_SetInteger(mp_object(), name, value);
}

const uint32_t sub_rgb[SUB_COLOURS] = { 0xffffff, 0xffe14a, 0x6fe7ff, 0x8cff7a };

void apply_sub_style(const SubStyle &st)
{
    set_mp_int("sub-text-scale", st.size);
    set_mp_int("freetype-color", sub_rgb[st.colour]);
    set_mp_int("freetype-background-color", 0x000000);
    set_mp_int("freetype-background-opacity", st.box ? 170 : 0);
}

void apply_deinterlace(int mode)
{
    if (mode == 1) {
        libvlc_video_set_deinterlace(mp, "yadif");
    } else if (mode == 2) {
        libvlc_video_set_deinterlace(mp, nullptr);
    } else {
        /* Auto: VLC's default, from the next video (libvlc has no call for it). */
        set_mp_int("deinterlace", -1);
    }
}

void apply_eq(int preset)
{
    if (preset < 0 || preset >= (int)libvlc_audio_equalizer_get_preset_count()) {
        libvlc_media_player_set_equalizer(mp, nullptr);
        return;
    }
    libvlc_equalizer_t *eq = libvlc_audio_equalizer_new_from_preset(preset);
    if (eq) {
        libvlc_media_player_set_equalizer(mp, eq);
        libvlc_audio_equalizer_release(eq);
    }
}

void apply_saved_choices()
{
    static const char *const ints[] = { "sub-text-scale", "freetype-color",
                                        "freetype-background-color", "freetype-background-opacity" };
    for (const char *n : ints)
        var_Create(mp_object(), n, VLC_VAR_INTEGER | VLC_VAR_DOINHERIT);
    apply_sub_style(player_sub_style());
    apply_deinterlace(player_deinterlace());
    apply_eq(player_eq());
    picture = player_picture();
}

std::vector<Chapter> chapters_cache;
double chapters_tried;

/* A slot's picture as RGB, cropped to what's visible: the shader's sums on
 * the CPU (BT.709 for HD, 601 below). */
struct Shot {
    std::vector<uint8_t> data;
    Format f;
    uint32_t w, h;
    std::string path;
};

void shot_main(Shot shot)
{
    const Format &f = shot.f;
    bool hd = shot.h >= 720;
    float kr = hd ? 1.5748f : 1.402f, kgu = hd ? -0.1873f : -0.344136f,
          kgv = hd ? -0.4681f : -0.714136f, kb = hd ? 1.8556f : 1.772f;
    std::vector<uint8_t> rgb((size_t)shot.w * shot.h * 3);
    auto sample = [&](int plane, uint32_t x, uint32_t y) {
        const uint8_t *row = shot.data.data() + f.offsets[plane] + (size_t)y * f.pitches[plane];
        return f.ten_bit ? ((const uint16_t *)row)[x] / 1023.0f : row[x] / 255.0f;
    };
    for (uint32_t y = 0; y < shot.h; y++)
        for (uint32_t x = 0; x < shot.w; x++) {
            float Y = sample(0, x, y), U = sample(1, x / 2, y / 2) - 0.5f, V = sample(2, x / 2, y / 2) - 0.5f;
            if (!f.full_range) {
                Y = (Y - 16.0f / 255) * (255.0f / 219);
                U *= 255.0f / 224;
                V *= 255.0f / 224;
            }
            float c[3] = { Y + kr * V, Y + kgu * U + kgv * V, Y + kb * U };
            uint8_t *o = &rgb[((size_t)y * shot.w + x) * 3];
            for (int i = 0; i < 3; i++)
                o[i] = (uint8_t)(c[i] < 0 ? 0 : c[i] > 1 ? 255 : c[i] * 255 + 0.5f);
        }
    bool ok = write_png_rgb(shot.path.c_str(), rgb.data(), shot.w, shot.h);
    fprintf(stderr, "player: screenshot %s %s\n", shot.path.c_str(), ok ? "saved" : "FAILED");
}

} // namespace

bool player_init()
{
    if (!create_pipeline()) {
        fprintf(stderr, "player: video pipeline failed\n");
        return false;
    }
    cpu_probe();
    load_decode_mode();
    log_debug = setting_int("debug", 0) != 0 || setting_int("vlc_debug", 0) != 0 || pref_int("debug", 0) != 0;
    /* The subtitle fonts from the app's folder, wherever it is now (it isn't
     * /app0 after the sandbox was lifted). */
    std::string font = "--freetype-font=" + std::string(plat_data_dir()) + "/fonts/subtitle.ttf";
    std::string mono = "--freetype-monofont=" + std::string(plat_data_dir()) + "/fonts/mono.ttf";
    /* HTTPS: Mozilla's CA list shipped in certs/ (the console has no store). */
    std::string certs = "--gnutls-dir-trust=" + std::string(plat_data_dir()) + "/certs";
    /* libass: the Noto fallbacks, for families an .ass file names but doesn't carry */
    std::string ass_fonts = "--ssa-fontsdir=" + std::string(plat_data_dir()) + "/fonts/fallback";
    const char *args[] = {
        font.c_str(),
        mono.c_str(),
        certs.c_str(),
        ass_fonts.c_str(),
        "--no-gnutls-system-trust",
#ifndef VLCPS5_HOST
        /* HLS / DASH segments through the access modules: ps5http (the
         * console's HTTP service), not the adaptive module's own sockets. */
        "--adaptive-use-access",
#endif
        "--ignore-config",
        "--no-video-title-show",
        "--stats",                /* decoded/shown/lost pictures, for player_tick */
        "--aout=adummy",          /* replaced by our callbacks (World 1) */
        "--avcodec-threads=0",    /* one per core */
        "--freetype-rel-fontsize=16",
        "--freetype-outline-thickness=4",
        /* FFmpeg's own messages follow VLC's verbosity and go straight to the
         * log: silent normally (VLC's errors still reach vlc_log). */
        log_debug ? "--verbose=2" : "--verbose=-1",
    };
    fprintf(stderr, "player: libvlc_new\n");
    vlc = libvlc_new(sizeof(args) / sizeof(*args), args);
    if (!vlc) {
        fprintf(stderr, "player: libvlc_new failed: %s\n", libvlc_errmsg());
        return false;
    }
    /* VLC's messages through our own callback: VLC's console logger crashed on
     * the console (its putc_unlocked macro reads FILE internals the console's
     * libc lays out differently), so it isn't linked in. */
    libvlc_log_set(vlc, vlc_log, nullptr);
    fprintf(stderr, "player: libvlc_media_player_new\n");
    libvlc_set_user_agent(vlc, "VLC media player", "VLC/3.0.24 PS5");
    mp = libvlc_media_player_new(vlc);
    fprintf(stderr, "player: callbacks\n");
    libvlc_video_set_callbacks(mp, vmem_lock, vmem_unlock, vmem_display, nullptr);
    libvlc_video_set_format_callbacks(mp, vmem_setup, vmem_cleanup);
    libvlc_audio_set_callbacks(mp, amem_play, amem_pause, amem_resume, amem_flush, amem_drain,
                               nullptr);
    libvlc_audio_set_volume_callback(mp, amem_volume);
    apply_saved_choices();
    libvlc_audio_set_format_callbacks(mp, amem_setup, nullptr);
    night_mode = pref_int("night", 0) != 0;
    fprintf(stderr, "player: audio port\n");
    if (plat_audio_open()) {
        audio_running = true;
        audio_thread = std::thread(audio_main);
    }
    fprintf(stderr, "player: libvlc %s ready\n", libvlc_get_version());
    return true;
}

static libvlc_media_t *last_media;
static bool live_paused;
/* A pause asked for and not done yet: VLC can take seconds to pause a stream
 * (HLS finishing a segment), and in between the screen said "playing" and
 * the sound went on, so it took presses. The screen and the sound follow the
 * press at once; VLC catches up (given up after 8 s). */
static int pause_wanted = -1;
static double pause_wanted_at;

/* Stopping waits for VLC's input thread to let go of the file or connection:
 * for a network stream that took 200-400 ms on the console (run 16), and
 * on the interface's thread the screen stalled black when leaving a channel.
 * The stop runs in the background; opening something waits for it first. */
static std::thread stopper;

static void wait_for_stop()
{
    if (stopper.joinable())
        stopper.join();
}

static void stop_in_background()
{
    wait_for_stop();
    if (mp) {
        libvlc_media_player_t *p = mp;
        stopping = true;
        stopper = std::thread([p] {
            libvlc_media_player_stop(p);
            stopping = false;
        });
    }
}

void player_shutdown()
{
    wait_for_stop();
    if (mp) {
        libvlc_media_player_stop(mp);
        libvlc_media_player_release(mp);
        mp = nullptr;
    }
    if (last_media) {
        libvlc_media_release(last_media);
        last_media = nullptr;
    }
    audio_running = false;
    if (audio_thread.joinable())
        audio_thread.join();
    if (vlc)
        libvlc_release(vlc);
    vlc = nullptr;
}

libvlc_instance_t *player_vlc()
{
    return vlc;
}

static libvlc_state_t state();

/* Live streams (radio, TV) mostly can't pause in VLC. Pause stops them and
 * play opens them again, at the live point: last_media is kept for that. */

bool player_open(const std::string &path, int64_t start_ms, const std::vector<std::string> &options)
{
    /* A file, or a network link (http://, https://, udp://, rtp://, ftp://, smb://...). */
    bool url = path.find("://") != std::string::npos;
    libvlc_media_t *m = url ? libvlc_media_new_location(vlc, path.c_str())
                            : libvlc_media_new_path(vlc, path.c_str());
    if (!m)
        return false;
    for (const std::string &o : options)
        libvlc_media_add_option(m, o.c_str());
    return player_open_media(m, path, start_ms);
}

std::string file_uri(const std::string &path);

bool player_open_media(libvlc_media_t *m, const std::string &path, int64_t start_ms)
{
    if (path != kept.path) {
        /* another file: upright, no delays, its own tracks */
        picture.rotate = 0;
        picture.mirror = picture.upside_down = false;
        kept = { path, 0, 0, -2, -2, {} };
    } else {
        /* the same one again: what was set comes back once it plays */
        /* (VLC finds the ones beside the video, named like it, by itself) */
        std::string base = path.substr(0, path.rfind('.'));
        for (const std::string &sub : kept.subs)
            if (sub_attach_all || sub.compare(0, base.size(), base) != 0)
                libvlc_media_slaves_add(m, libvlc_media_slave_type_subtitle, 4, file_uri(sub).c_str());
        if (kept.audio != -2)
            restore_audio = kept.audio;
        if (kept.spu != -2)
            restore_spu = kept.spu;
        if (restore_audio == -2 && restore_spu != -2)
            restore_audio = -3; /* "nothing to pick" but the subtitles still are */
        restore_delays = kept.audio_delay || kept.spu_delay;
    }
    if (start_ms > 0) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":start-time=%.3f", start_ms / 1000.0);
        libvlc_media_add_option(m, opt);
    }
    /* Seeks land on the keyframe before the target instead of decoding every
     * picture from it to the exact time unseen: with seconds between keyframes
     * (PS5 recordings, streaming encodes) at 60 fps, that hidden work froze the
     * picture after every jump on the console. */
    libvlc_media_add_option(m, ":input-fast-seek");
    std::string alang = player_audio_language(), slang = player_sub_language();
    if (!alang.empty())
        libvlc_media_add_option(m, (":audio-language=" + alang).c_str());
    if (!slang.empty())
        libvlc_media_add_option(m, (":sub-language=" + (slang == "off" ? std::string("none") : slang)).c_str());
    chapters_cache.clear();
    chapters_tried = 0;
    fast_now = decode_mode == DECODE_FAST || (decode_mode == DECODE_AUTO && path == fast_path);
    if (fast_now) {
        libvlc_media_add_option(m, ":avcodec-skiploopfilter=4");
        libvlc_media_add_option(m, ":avcodec-fast");
    }
    wait_for_stop();
    libvlc_media_player_set_media(mp, m);
    if (last_media)
        libvlc_media_release(last_media);
    last_media = m; /* our reference: kept for a live pause */
    live_paused = false;
    pause_wanted = -1;
    info = VideoInfo{};
    info_tries = 0;
    current_path = path;
    health = Health{};
    /* Opening and the first second drop pictures anyway; a resume also seeks
     * and buffers first (run 19: one false "can't keep up" right after it). */
    health.next = plat_time() + (start_ms > 0 ? 5 : 3);
    debug_last = libvlc_media_stats_t{};
    fprintf(stderr, "player: open %s at %lld ms%s\n", path.c_str(), (long long)start_ms,
            fast_now ? " (fast decoding)" : "");
    return libvlc_media_player_play(mp) == 0;
}

DecodeMode player_decode_mode()
{
    return decode_mode;
}

void player_set_decode_mode(DecodeMode mode)
{
    decode_mode = mode;
    save_decode_mode();
}

bool player_fast_decoding()
{
    return fast_now;
}

PlayerEvent player_tick()
{
    double t = plat_time();
    if (!mp || t < health.next)
        return PLAYER_EVENT_NONE;
    health.next = t + 1;
    if (sub_check_at > 0 && t >= sub_check_at && state() == libvlc_Playing) {
        sub_check_at = 0;
        size_t now_count = player_subtitle_tracks().size();
        if (now_count <= sub_check_count && !kept.subs.empty()) {
            fprintf(stderr, "player: the subtitle didn't come up (%zu tracks): reopening with it\n", now_count);
            int64_t at = libvlc_media_player_get_time(mp);
            std::string path = current_path;
            restore_spu = 1000; /* the last subtitle track: picked below once known */
            libvlc_media_player_stop(mp);
            {
                std::lock_guard<std::mutex> g(audio_lock);
                audio_queue.clear();
            }
            sub_attach_all = true;
            player_open(path, at > 0 ? at : 0);
            sub_attach_all = false;
            return PLAYER_EVENT_NONE;
        }
    }
    if (restore_spu == 1000 && state() == libvlc_Playing) {
        std::vector<Track> subs = player_subtitle_tracks();
        restore_spu = subs.empty() ? -2 : subs.back().id;
        if (restore_audio == -2)
            restore_audio = -3;
    }
    if (restore_audio != -2 && state() == libvlc_Playing) {
        /* After a reopen (Auto's, or the same file again): the tracks that
         * were picked before it. */
        if (restore_audio >= -1)
            libvlc_audio_set_track(mp, restore_audio);
        if (restore_spu >= -1)
            libvlc_video_set_spu(mp, restore_spu);
        restore_audio = restore_spu = -2;
    }
    if (restore_delays && state() == libvlc_Playing) {
        libvlc_audio_set_delay(mp, kept.audio_delay * 1000);
        libvlc_video_set_spu_delay(mp, kept.spu_delay * 1000);
        restore_delays = false;
    }
    libvlc_media_stats_t s;
    if (!get_stats(&s))
        return PLAYER_EVENT_NONE;
    int shown = s.i_displayed_pictures - health.last.i_displayed_pictures;
    int lost = s.i_lost_pictures - health.last.i_lost_pictures;
    int decoded = s.i_decoded_video - health.last.i_decoded_video;
    health.last = s;
    /* The first count is everything since the open, not one second: it only
     * sets where the next one starts from. */
    if (!health.primed) {
        health.primed = true;
        return PLAYER_EVENT_NONE;
    }
    float fps = info.fps > 0 ? info.fps : 24;
    /* Struggling: most pictures of a second lost, or far fewer shown than the
     * file has. Not while paused, fast-forwarding or with no video at all. */
    bool playing = state() == libvlc_Playing && libvlc_media_player_get_rate(mp) <= 1.01f;
    bool bad = playing && decoded > 0 &&
               (lost > shown || shown < fps * 0.5f);
    health.bad_seconds = bad ? health.bad_seconds + 1 : 0;
    if (bad)
        fprintf(stderr, "player: can't keep up: %d decoded, %d shown, %d lost in 1 s (%.2f fps file)\n",
                decoded, shown, lost, fps);
    if (health.bad_seconds < 3)
        return PLAYER_EVENT_NONE;
    health.bad_seconds = 0;
    if (!fast_now && decode_mode == DECODE_AUTO) {
        fprintf(stderr, "player: switching %s to fast decoding\n", current_path.c_str());
        restore_audio = libvlc_audio_get_track(mp);
        restore_spu = libvlc_video_get_spu(mp);
        fast_path = current_path;
        int64_t at = libvlc_media_player_get_time(mp);
        std::string path = current_path;
        libvlc_media_player_stop(mp);
        {
            std::lock_guard<std::mutex> g(audio_lock);
            audio_queue.clear();
        }
        player_open(path, at > 0 ? at : 0);
        return PLAYER_EVENT_WENT_FAST;
    }
    if (!health.gave_up) {
        health.gave_up = true;
        return PLAYER_EVENT_TOO_SLOW;
    }
    return PLAYER_EVENT_NONE;
}

void player_stop()
{
    live_paused = false;
    pause_wanted = -1;
    stop_in_background();
    std::lock_guard<std::mutex> g(audio_lock);
    audio_queue.clear();
    audio_paused = false;
}

static libvlc_state_t state()
{
    return mp ? libvlc_media_player_get_state(mp) : libvlc_NothingSpecial;
}

bool player_active()
{
    libvlc_state_t s = state();
    return live_paused || s == libvlc_Opening || s == libvlc_Buffering || s == libvlc_Playing ||
           s == libvlc_Paused;
}

bool player_ended()
{
    libvlc_state_t s = state();
    return s == libvlc_Ended || s == libvlc_Error;
}

bool player_failed()
{
    return state() == libvlc_Error;
}

bool player_buffering()
{
    libvlc_state_t s = state();
    return s == libvlc_Opening || (s == libvlc_Buffering && !video_has_picture());
}

void player_navigate(int action)
{
    static const libvlc_navigate_mode_t modes[] = { libvlc_navigate_activate, libvlc_navigate_up,
                                                    libvlc_navigate_down, libvlc_navigate_left,
                                                    libvlc_navigate_right };
    if (mp && action >= 0 && action < 5)
        libvlc_media_player_navigate(mp, modes[action]);
}

bool player_paused()
{
    if (live_paused)
        return true;
    bool real = state() == libvlc_Paused;
    if (pause_wanted >= 0) {
        if (real == (pause_wanted == 1) || plat_time() - pause_wanted_at > 8)
            pause_wanted = -1;
        else
            return pause_wanted == 1;
    }
    return real;
}

void player_toggle_pause()
{
    if (!mp)
        return;
    if (live_paused) {
        /* back to the live point */
        live_paused = false;
        wait_for_stop();
        if (last_media) {
            libvlc_media_player_set_media(mp, last_media);
            libvlc_media_player_play(mp);
        }
        return;
    }
    libvlc_state_t s = state();
    /* A live stream, or anything still opening (VLC ignores a pause then):
     * stop it now (the sound stops at once), keep the screen. */
    if (s == libvlc_Opening ||
        (!libvlc_media_player_can_pause(mp) && (s == libvlc_Playing || s == libvlc_Buffering))) {
        stop_in_background();
        live_paused = true;
        std::lock_guard<std::mutex> g(audio_lock);
        audio_queue.clear();
        return;
    }
    bool want = !player_paused();
    libvlc_media_player_set_pause(mp, want ? 1 : 0);
    pause_wanted = want ? 1 : 0;
    pause_wanted_at = plat_time();
    std::lock_guard<std::mutex> g(audio_lock);
    audio_paused = want; /* the sound stops (or comes back) now */
}

int64_t player_time()
{
    return mp ? libvlc_media_player_get_time(mp) : 0;
}

int64_t player_length()
{
    int64_t l = mp ? libvlc_media_player_get_length(mp) : 0;
    return l > 0 ? l : 0;
}

void player_seek(int64_t ms)
{
    if (!mp)
        return;
    int64_t len = player_length();
    if (ms < 0)
        ms = 0;
    if (len > 0 && ms > len - 500)
        ms = len - 500;
    libvlc_media_player_set_time(mp, ms);
}

/* The app's own volume, applied in the audio thread: with our audio callbacks
 * VLC doesn't report its volume back (it read 0%). 0..200%, as VLC's. */
int user_volume = 100;

int player_volume()
{
    return user_volume;
}

void player_set_volume(int volume)
{
    user_volume = volume < 0 ? 0 : volume > 200 ? 200 : volume;
    std::lock_guard<std::mutex> g(audio_lock);
    user_gain = user_volume / 100.0f;
}

float player_rate()
{
    return mp ? libvlc_media_player_get_rate(mp) : 1.0f;
}

void player_set_rate(float rate)
{
    if (mp)
        libvlc_media_player_set_rate(mp, rate);
}

std::vector<Track> player_audio_tracks()
{
    return tracks_from(mp ? libvlc_audio_get_track_description(mp) : nullptr);
}

int player_audio_track()
{
    return mp ? libvlc_audio_get_track(mp) : -1;
}

void player_set_audio_track(int id)
{
    if (mp)
        libvlc_audio_set_track(mp, id);
    kept.audio = id;
}

std::vector<Track> player_subtitle_tracks()
{
    return tracks_from(mp ? libvlc_video_get_spu_description(mp) : nullptr);
}

int player_subtitle_track()
{
    return mp ? libvlc_video_get_spu(mp) : -1;
}

void player_set_subtitle_track(int id)
{
    if (mp)
        libvlc_video_set_spu(mp, id);
    kept.spu = id;
}

void player_debug_line()
{
    static const char *const names[] = { "free", "writing", "ready", "shown", "gpu" };
    char slots_text[128] = "";
    {
        std::lock_guard<std::mutex> g(pic_lock);
        for (const Slot &s : slots)
            snprintf(slots_text + strlen(slots_text), sizeof(slots_text) - strlen(slots_text), "%s ",
                     s.buffer ? names[s.state] : "-");
    }
    size_t queued;
    {
        std::lock_guard<std::mutex> g(audio_lock);
        queued = audio_queue.size();
    }
    libvlc_media_stats_t s = {};
    get_stats(&s);
    fprintf(stderr, "player: state %d, time %lld / %lld ms, slots [%s], audio blocks %zu, frame %llu done %llu, "
            "pictures +%d decoded +%d shown +%d lost, input %.0f kbit/s%s\n",
            (int)state(), (long long)player_time(), (long long)player_length(), slots_text, queued,
            (unsigned long long)gfx.frame_number, (unsigned long long)completed_frame.load(),
            s.i_decoded_video - debug_last.i_decoded_video,
            s.i_displayed_pictures - debug_last.i_displayed_pictures,
            s.i_lost_pictures - debug_last.i_lost_pictures, s.f_demux_bitrate * 8000,
            fast_now ? ", fast decoding" : "");
    debug_last = s;
}

/* A file:// URI: unreserved characters and '/' as they are, the rest %XX. */
std::string file_uri(const std::string &path)
{
    /* VLC's own: a full path (a relative one is made full), each part
     * escaped as its file access reads it back */
    char *u = vlc_path2uri(path.c_str(), "file");
    std::string uri = u ? u : "";
    free(u);
    return uri;
}

bool player_add_subtitle(const std::string &path)
{
    if (!mp)
        return false;
    sub_check_count = player_subtitle_tracks().size();
    sub_check_at = plat_time() + 4;
    int r = libvlc_media_player_add_slave(mp, libvlc_media_slave_type_subtitle, file_uri(path).c_str(), true);
    fprintf(stderr, "player: subtitle file %s: %s\n", path.c_str(), r == 0 ? "added" : "failed");
    if (r == 0 && std::find(kept.subs.begin(), kept.subs.end(), path) == kept.subs.end())
        kept.subs.push_back(path);
    return r == 0;
}

const VideoInfo &player_video_info()
{
    if (!info.width && mp && info_tries++ % 15 == 0)
        read_info();
    return info;
}

void video_upload(VkCommandBuffer cmd)
{
    completed_frame = gfx_completed_frame();
    Format f;
    int gen, slot = -1;
    {
        std::lock_guard<std::mutex> g(pic_lock);
        reclaim_locked();
        /* Old buffers whose last copy has finished. */
        for (size_t i = 0; i < garbage.size();) {
            if (garbage[i].after_frame <= completed_frame) {
                vkDestroyBuffer(gfx.device, garbage[i].buffer, nullptr);
                vkFreeMemory(gfx.device, garbage[i].memory, nullptr);
                garbage[i] = garbage.back();
                garbage.pop_back();
            } else {
                i++;
            }
        }
        f = format;
        gen = generation;
        if (shown >= 0 && slots[shown].state == SHOWN)
            slot = shown;
    }
    pic_freed.notify_all();
    if (gen != planes.generation && gen > 0) {
        /* The same shape again (the file restarted, Watch again, a loop):
         * the textures stay, and so does the last picture until the next
         * one comes; remade, the video blinked black. */
        bool same = planes.image[0] && f.width == planes.buf_w && f.height == planes.buf_h &&
                    f.ten_bit == planes.ten_bit && f.full_range == planes.full_range;
        if (!same && !make_planes(f))
            return;
        planes.generation = gen;
    }
    if (slot < 0 || planes.generation != gen)
        return;
    VkBuffer buffer;
    {
        std::lock_guard<std::mutex> g(pic_lock);
        if (slots[slot].state != SHOWN)
            return;
        buffer = slots[slot].buffer;
        slots[slot].state = GPU;
        slots[slot].gpu_frame = gfx.frame_number + 1;
        if (shown == slot)
            shown = -1;
        last_slot = slot;
    }
    uint32_t bpp = f.ten_bit ? 2 : 1;
    for (int i = 0; i < PLANES; i++) {
        gfx_barrier(cmd, planes.image[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {};
        region.bufferOffset = f.offsets[i];
        region.bufferRowLength = f.pitches[i] / bpp;
        region.bufferImageHeight = f.lines[i];
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { planes.w[i], planes.h[i], 1 };
        vkCmdCopyBufferToImage(cmd, buffer, planes.image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        gfx_barrier(cmd, planes.image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    }
    planes.has_picture = true;
}

bool video_has_picture()
{
    return planes.has_picture;
}

void video_forget_picture()
{
    planes.has_picture = false;
}

void video_draw(VkCommandBuffer cmd, float x, float y, float w, float h)
{
    if (!planes.has_picture)
        return;
    const VideoInfo &v = player_video_info();
    Push p = {};
    /* BT.709 for HD, BT.601 for SD (VLC 3's callbacks don't say which). */
    uint32_t vis_h = v.height ? v.height : planes.buf_h;
    if (vis_h >= 720) {
        float r0[4] = { 1, 0, 1.5748f, 0 }, r1[4] = { 1, -0.1873f, -0.4681f, 0 },
              r2[4] = { 1, 1.8556f, 0, 0 };
        memcpy(p.row0, r0, 16); memcpy(p.row1, r1, 16); memcpy(p.row2, r2, 16);
    } else {
        float r0[4] = { 1, 0, 1.402f, 0 }, r1[4] = { 1, -0.344136f, -0.714136f, 0 },
              r2[4] = { 1, 1.772f, 0, 0 };
        memcpy(p.row0, r0, 16); memcpy(p.row1, r1, 16); memcpy(p.row2, r2, 16);
    }
    p.params[0] = planes.ten_bit ? 65535.0f / 1023.0f : 1.0f;
    p.params[1] = planes.full_range ? 0.0f : 1.0f;
    p.params[2] = picture.hue * 3.14159265f / 180;
    p.adjust[0] = picture.brightness / 200.0f;
    p.adjust[1] = 1 + picture.contrast / 100.0f;
    p.adjust[2] = 1 + picture.saturation / 100.0f;
    p.adjust[3] = exp2f(-picture.gamma / 100.0f);
    p.xform[0] = (float)picture.rotate;
    p.xform[1] = picture.mirror ? 1.0f : 0.0f;
    p.xform[2] = picture.upside_down ? 1.0f : 0.0f;
    p.xform[3] = picture.sharpen / 25.0f; /* up to 4x the edges */
    if (v.spherical) {
        p.params[3] = 1;
        p.view[0] = view_yaw * 3.14159265f / 180;
        p.view[1] = view_pitch * 3.14159265f / 180;
        p.view[2] = tanf(view_fov * 3.14159265f / 360);
        p.view[3] = h > 0 ? w / h : 16.0f / 9;
    }
    uint32_t vis_w = v.width && v.width <= planes.buf_w ? v.width : planes.buf_w;
    vis_h = v.height && v.height <= planes.buf_h ? v.height : planes.buf_h;
    p.uv_scale[0] = (float)vis_w / planes.buf_w;
    p.uv_scale[1] = (float)vis_h / planes.buf_h;
    VkViewport vp = { x, y, w, h, 0, 1 };
    /* The scissor stays on screen ("Fill" makes the rectangle larger than it). */
    float sx0 = x < 0 ? 0 : x, sy0 = y < 0 ? 0 : y;
    float sx1 = x + w > gfx.width ? gfx.width : x + w, sy1 = y + h > gfx.height ? gfx.height : y + h;
    VkRect2D sc = { { (int32_t)sx0, (int32_t)sy0 },
                    { (uint32_t)(sx1 - sx0 + 0.5f), (uint32_t)(sy1 - sy0 + 0.5f) } };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &desc_set, 0,
                            nullptr);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void player_view(float yaw, float pitch, float fov)
{
    view_yaw = yaw;
    view_pitch = pitch;
    view_fov = fov;
}

int64_t player_audio_delay()
{
    return mp ? libvlc_audio_get_delay(mp) / 1000 : 0;
}

void player_set_audio_delay(int64_t ms)
{
    if (mp)
        libvlc_audio_set_delay(mp, ms * 1000);
    kept.audio_delay = ms;
}

int64_t player_subtitle_delay()
{
    return mp ? libvlc_video_get_spu_delay(mp) / 1000 : 0;
}

void player_set_subtitle_delay(int64_t ms)
{
    if (mp)
        libvlc_video_set_spu_delay(mp, ms * 1000);
    kept.spu_delay = ms;
}

std::vector<Chapter> player_chapters()
{
    /* Known once the file is open; asked again at most once a second until then. */
    if (!mp || !chapters_cache.empty() || plat_time() < chapters_tried + 1)
        return chapters_cache;
    chapters_tried = plat_time();
    libvlc_chapter_description_t **d = nullptr;
    int n = libvlc_media_player_get_full_chapter_descriptions(mp, -1, &d);
    for (int i = 0; i < n; i++) {
        char fallback[32];
        snprintf(fallback, sizeof(fallback), tr("Chapter %d"), i + 1);
        chapters_cache.push_back({ d[i]->i_time_offset,
                                   d[i]->psz_name && *d[i]->psz_name ? d[i]->psz_name : fallback });
    }
    if (n > 0)
        libvlc_chapter_descriptions_release(d, n);
    /* One "chapter" is the whole file, not a chapter list. */
    if (chapters_cache.size() == 1)
        chapters_cache.clear();
    return chapters_cache;
}

int player_chapter()
{
    return mp ? libvlc_media_player_get_chapter(mp) : -1;
}

void player_set_chapter(int index)
{
    if (mp)
        libvlc_media_player_set_chapter(mp, index);
}

void player_next_frame()
{
    if (mp)
        libvlc_media_player_next_frame(mp);
}

bool player_screenshot(const std::string &path)
{
    Shot shot;
    {
        std::lock_guard<std::mutex> g(pic_lock);
        if (last_slot < 0 || !slots[last_slot].buffer || planes.generation != generation)
            return false;
        shot.f = format;
        shot.data.assign(slots[last_slot].mapped, slots[last_slot].mapped + format.size);
    }
    shot.w = info.width && info.width <= shot.f.width ? info.width : shot.f.width;
    shot.h = info.height && info.height <= shot.f.height ? info.height : shot.f.height;
    shot.path = path;
    std::thread(shot_main, std::move(shot)).detach();
    return true;
}

int player_eq_presets()
{
    return (int)libvlc_audio_equalizer_get_preset_count();
}

const char *player_eq_name(int preset)
{
    if (preset < 0)
        return "Off";
    const char *n = libvlc_audio_equalizer_get_preset_name(preset);
    return n ? n : "?";
}

int player_eq()
{
    return pref_int("eq", -1);
}

void player_set_eq(int preset)
{
    pref_set("eq", preset);
    if (mp)
        apply_eq(preset);
}

int player_deinterlace()
{
    return pref_int("deinterlace", 0);
}

void player_set_deinterlace(int mode)
{
    pref_set("deinterlace", mode);
    if (mp)
        apply_deinterlace(mode);
}

const char *const sub_colour_names[SUB_COLOURS] = { "White", "Yellow", "Cyan", "Green" };

SubStyle player_sub_style()
{
    SubStyle s;
    s.size = pref_int("sub_size", 100);
    s.colour = pref_int("sub_colour", SUB_WHITE);
    if (s.colour < 0 || s.colour >= SUB_COLOURS)
        s.colour = SUB_WHITE;
    s.box = pref_int("sub_box", 0) != 0;
    return s;
}

void player_set_sub_style(const SubStyle &s)
{
    pref_set("sub_size", s.size);
    pref_set("sub_colour", s.colour);
    pref_set("sub_box", s.box ? 1 : 0);
    if (mp)
        apply_sub_style(s);
}

std::string player_audio_language()
{
    return pref_str("lang_audio", "");
}

std::string player_sub_language()
{
    return pref_str("lang_sub", "");
}

void player_set_languages(const std::string &audio, const std::string &sub)
{
    pref_set("lang_audio", audio);
    pref_set("lang_sub", sub);
}

PictureAdjust player_picture()
{
    PictureAdjust p;
    p.brightness = pref_int("pic_brightness", 0);
    p.contrast = pref_int("pic_contrast", 0);
    p.saturation = pref_int("pic_saturation", 0);
    p.gamma = pref_int("pic_gamma", 0);
    p.hue = pref_int("pic_hue", 0);
    p.sharpen = pref_int("pic_sharpen", 0);
    /* turns and flips are for the file playing, not remembered */
    p.rotate = picture.rotate;
    p.mirror = picture.mirror;
    p.upside_down = picture.upside_down;
    return p;
}

int player_rotation()
{
    return picture.rotate;
}

void player_set_picture(const PictureAdjust &p)
{
    picture = p;
    pref_set("pic_brightness", p.brightness);
    pref_set("pic_contrast", p.contrast);
    pref_set("pic_saturation", p.saturation);
    pref_set("pic_gamma", p.gamma);
    pref_set("pic_hue", p.hue);
    pref_set("pic_sharpen", p.sharpen);
}

int player_audio_output()
{
    return pref_int("audio_out", 0);
}

void player_set_audio_output(int mode)
{
    pref_set("audio_out", mode);
}

bool player_night_mode()
{
    std::lock_guard<std::mutex> g(audio_lock);
    return night_mode;
}

void player_set_night_mode(bool on)
{
    pref_set("night", on ? 1 : 0);
    std::lock_guard<std::mutex> g(audio_lock);
    night_mode = on;
}

bool player_speaker_test(int channel)
{
    if (channel >= 0 && !plat_audio_open_surround())
        return false;
    tone_channel = channel;
    return true;
}

bool player_debug_log()
{
    return log_debug;
}

void player_set_debug_log(bool on)
{
    log_debug = on;
}

/* Goertzel at log-spaced frequencies over the last 1024 samples (Hann window):
 * a few thousand multiplications a frame, no FFT needed for 32 bars. */
void player_spectrum(float *bands, int n)
{
    const int N = 1024;
    static float win[N];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < N; i++)
            win[i] = 0.5f - 0.5f * cosf(2 * 3.14159265f * i / (N - 1));
        ready = true;
    }
    float x[N];
    unsigned end = vis_pos.load(std::memory_order_relaxed);
    for (int i = 0; i < N; i++)
        x[i] = vis_ring[(end - N + i) & 4095] * win[i];
    for (int b = 0; b < n; b++) {
        float hz = 45.0f * powf(16000.0f / 45.0f, (float)b / std::max(1, n - 1));
        float w = 2 * 3.14159265f * hz / AUDIO_RATE, c = 2 * cosf(w);
        float s1 = 0, s2 = 0;
        for (int i = 0; i < N; i++) {
            float s0 = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float mag = sqrtf(std::max(0.0f, s1 * s1 + s2 * s2 - c * s1 * s2)) / (N / 4);
        float db = 20 * log10f(mag + 1e-6f);
        bands[b] = std::min(1.0f, std::max(0.0f, (db + 54) / 54));
    }
}
