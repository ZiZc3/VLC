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
#include <chrono>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>

#include <vlc/vlc.h>
/* libvlc's player is a VLC object; a few options have no libvlc call (the
 * subtitle look, Auto deinterlace), so they're set as its variables, which
 * its video output and subtitle renderer inherit. */
#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_variables.h>
#include <vlc_url.h>
#include <vlc_stream.h>

#include "gen/video_frag.h"
#include "gen/video_vert.h"
#include "gfx.h"
#include "image.h"
#include "lang.h"
#include "platform.h"
#include "prefs.h"
#include "subconv.h"

namespace {

libvlc_instance_t *vlc;
libvlc_media_player_t *mp;
std::string current_path;
VideoInfo info;
int info_tries;
/* VLC set up new pictures (a stream changing quality or shape mid-way):
 * the size and shape are read again (issue #6: IPTV "zooms after a while") */
std::atomic<bool> info_stale{ false };

/* Decoding speed. A file the CPU can't decode in real time makes VLC drop
 * every late picture, so the screen freezes while the sound and the clock go
 * on. Fast decoding skips H.264/HEVC/VP9's loop filter (softer picture, much
 * less work); Auto switches to it when a file keeps losing pictures. */
DecodeMode decode_mode = DECODE_AUTO;
bool fast_now;             /* the current file was opened with fast decoding */
std::string fast_path;     /* the file Auto switched, so a reopen keeps it */
/* Hardware decoding (ps5/modules/ps5vdec.c): settings.txt hw_decode=0 turns
 * it off; a video the console's decoder gave up on reopens with FFmpeg. */
std::atomic<bool> vdec_failed{ false };
std::atomic<int> vdec_active{ 0 };   /* decoders open on the console's decoder */
/* how long a video takes to show its first picture (the log) */
double opened_at;
bool first_picture_wait;
std::string software_path;
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
/* Subtitle files beside a video on a share or a link, found in the background
 * once it plays (src: side_subs_find). */
struct SideSubs {
    std::mutex lock;
    std::string path;              /* the video they're for */
    std::vector<std::string> uris; /* best first */
    bool ready;
    int serial;                    /* bumped by every open: an old search is dropped */
} side;
std::vector<std::string> source_options; /* the open's login options, for the folder listing */
/* Subtitle files added by hand from a share or a link, read in the background:
 * player_tick puts them in (when it's still the same file). */
struct PendingSub {
    std::string path, ready; /* ready: what VLC gets, "" = couldn't be read */
    int serial;              /* side.serial when it was asked for */
};
std::mutex pending_lock;
std::vector<PendingSub> pending_subs;

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

/* A picture's colours, from VLC just before vmem's setup (patches/0014).
 * HDR (PQ or HLG, mostly BT.2020) is turned into SDR in the shader: its
 * brightness brought down to what a TV shows as white (tone mapping), its
 * colours brought into BT.709's. */
enum Transfer { TRC_SDR, TRC_PQ, TRC_HLG };
enum Matrix { MATRIX_AUTO, MATRIX_601, MATRIX_709, MATRIX_2020 };
struct Colour {
    int transfer = TRC_SDR;
    int matrix = MATRIX_AUTO;  /* YCbCr to RGB; AUTO: 709 for HD, 601 below */
    bool wide = false;         /* BT.2020 primaries */
    float peak = 1000;         /* the picture's brightest, in nits (HDR) */
    bool full_range = false;   /* 0-255 (VLC says so; NV12 has no "J" name) */
};
/* HDR's reference white (BT.2408): what shows as the TV's white in SDR */
const float SDR_WHITE = 203;

struct Format {
    uint32_t width, height;        /* buffer size, as VLC writes it */
    uint32_t pitches[PLANES], lines[PLANES];
    VkDeviceSize offsets[PLANES];
    VkDeviceSize size;
    bool ten_bit, full_range;
    /* NV12 / P010 (the console's hardware decoder): Y, then U and V
     * interleaved in one plane; P010 keeps its 10 bits at the top */
    bool two_planes;
    Colour colour;
};

std::mutex pic_lock;
std::condition_variable pic_freed;
Slot slots[SLOTS];
Format format;
int generation;        /* bumped by every vmem setup */
Colour next_colour;    /* vmem_colour's, for the setup right after it */
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
    bool ten_bit, full_range, two_planes;
    Colour colour;
    uint32_t buf_w, buf_h;
} planes;

VkSampler sampler;
VkDescriptorSetLayout set_layout;
VkDescriptorPool desc_pool;
VkDescriptorSet desc_set;
VkPipelineLayout pipe_layout;
VkPipeline pipeline;
VkPipeline pipeline_hdr;   /* in gfx.hdr_pass (HDR10 output) */

struct Push {
    float row0[4], row1[4], row2[4];
    float params[4];   /* sample scale, limited range, hue (radians), 360° (1) */
    float uv_scale[2];
    float pad[2];
    float adjust[4];   /* brightness, contrast, saturation, gamma exponent */
    float view[4];     /* 360°: yaw, pitch (radians), tan(fov / 2), screen aspect */
    float xform[4];    /* quarter turns clockwise, mirror, upside down, sharpen */
    float hdr[4];      /* transfer (Transfer), peak (nits), BT.2020 primaries (1), NV12/P010 (1) */
    float outp[4];     /* HDR10 output (1): PQ, BT.2020 into gfx.hdr_pass's 10-bit frame */
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
    /* A buffer no copy reads (never drawn, or its last copy finished) goes
     * now: when VLC fails to start a video it sets up again and again, and
     * 4K buffers left for later piled up until the memory ran out (#9). */
    uint64_t done = completed_frame.load();
    for (Slot &s : slots) {
        if (s.buffer && s.state != GPU && s.gpu_frame <= done) {
            vkDestroyBuffer(gfx.device, s.buffer, nullptr);
            vkFreeMemory(gfx.device, s.memory, nullptr);
        } else if (s.buffer) {
            garbage.push_back({ s.buffer, s.memory, after_frame });
        }
        s = Slot{};
    }
    shown = -1;
}

/* VLC's picture format just before setup (patches/0014): its colours. */
void vmem_colour(void *opaque, const video_format_t *fmt)
{
    (void)opaque;
    Colour c;
    if (fmt->transfer == TRANSFER_FUNC_SMPTE_ST2084)
        c.transfer = TRC_PQ;
    else if (fmt->transfer == TRANSFER_FUNC_HLG)
        c.transfer = TRC_HLG;
    switch (fmt->space) {
    case COLOR_SPACE_BT601: c.matrix = MATRIX_601; break;
    case COLOR_SPACE_BT709: c.matrix = MATRIX_709; break;
    case COLOR_SPACE_BT2020: c.matrix = MATRIX_2020; break;
    default: c.matrix = c.transfer != TRC_SDR ? MATRIX_2020 : MATRIX_AUTO; break;
    }
    c.full_range = fmt->b_color_range_full;
    c.wide = fmt->primaries == COLOR_PRIMARIES_BT2020 ||
             (fmt->primaries == COLOR_PRIMARIES_UNDEF && c.transfer != TRC_SDR);
    /* How bright it gets: the content's own light level, else the mastering
     * display's (in 1/10000 nits), else 1000 nits, the usual grade; HLG is
     * made for a 1000-nit TV. */
    if (c.transfer == TRC_PQ) {
        if (fmt->lighting.MaxCLL >= 100)
            c.peak = fmt->lighting.MaxCLL;
        else if (fmt->mastering.max_luminance >= 100 * 10000)
            c.peak = fmt->mastering.max_luminance / 10000.0f;
        c.peak = std::min(c.peak, 10000.0f);
    }
    fprintf(stderr, "player: colours: transfer %d primaries %d matrix %d%s; MaxCLL %u, mastering %u nits%s\n",
            (int)fmt->transfer, (int)fmt->primaries, (int)fmt->space,
            fmt->b_color_range_full ? " full range" : "", (unsigned)fmt->lighting.MaxCLL,
            (unsigned)(fmt->mastering.max_luminance / 10000),
            c.transfer == TRC_PQ ? " -> HDR10, tone mapped" : c.transfer == TRC_HLG ? " -> HLG, tone mapped" : "");
    std::lock_guard<std::mutex> g(pic_lock);
    next_colour = c;
}

/* ---- colour sums (the shader's, on the CPU for screenshots) ---------------- */

struct YuvMatrix { float kr, kgu, kgv, kb; };

YuvMatrix yuv_matrix(int matrix, uint32_t visible_h)
{
    if (matrix == MATRIX_AUTO)
        matrix = visible_h >= 720 ? MATRIX_709 : MATRIX_601;
    if (matrix == MATRIX_2020)
        return { 1.4746f, -0.16455f, -0.57135f, 1.8814f };
    if (matrix == MATRIX_709)
        return { 1.5748f, -0.1873f, -0.4681f, 1.8556f };
    return { 1.402f, -0.344136f, -0.714136f, 1.772f };
}

/* SMPTE ST 2084 (PQ): signal <-> nits */
const float PQ_M1 = 0.1593017578125f, PQ_M2 = 78.84375f;
const float PQ_C1 = 0.8359375f, PQ_C2 = 18.8515625f, PQ_C3 = 18.6875f;

float pq_to_nits(float e)
{
    float p = powf(std::clamp(e, 0.0f, 1.0f), 1 / PQ_M2);
    return 10000 * powf(std::max(p - PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * p), 1 / PQ_M1);
}

float nits_to_pq(float nits)
{
    float y = powf(std::clamp(nits / 10000, 0.0f, 1.0f), PQ_M1);
    return powf((PQ_C1 + PQ_C2 * y) / (1 + PQ_C3 * y), PQ_M2);
}

/* rgb: the picture's signal (0..1) -> what an SDR TV should get (0..1) */
void hdr_to_sdr(float rgb[3], const Colour &c)
{
    float nits[3];
    if (c.transfer == TRC_PQ) {
        for (int i = 0; i < 3; i++)
            nits[i] = pq_to_nits(rgb[i]);
    } else {
        /* HLG: scene light, then the system gamma of a 1000-nit TV (BT.2100) */
        const float a = 0.17883277f, b = 0.28466892f, k = 0.55991073f;
        float s[3];
        for (int i = 0; i < 3; i++) {
            float e = std::clamp(rgb[i], 0.0f, 1.0f);
            s[i] = e <= 0.5f ? e * e / 3 : (expf((e - k) / a) + b) / 12;
        }
        float ys = std::max(0.2627f * s[0] + 0.6780f * s[1] + 0.0593f * s[2], 1e-6f);
        for (int i = 0; i < 3; i++)
            nits[i] = 1000 * powf(ys, 0.2f) * s[i];
    }
    /* BT.2390's roll-off on the brightest channel (keeps the hue): up to
     * about half of SDR white nothing changes, above it the highlights bend
     * down to fit under white. */
    float peak = c.transfer == TRC_PQ ? c.peak : 1000;
    float sig = std::max(nits[0], std::max(nits[1], nits[2]));
    if (sig > 0 && peak > SDR_WHITE) {
        float src = nits_to_pq(peak);
        float e1 = std::min(nits_to_pq(sig) / src, 1.0f);
        float maxl = nits_to_pq(SDR_WHITE) / src;
        float ks = 1.5f * maxl - 0.5f, e2 = e1;
        if (e1 > ks) {
            float t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
            e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * maxl;
        }
        float scale = pq_to_nits(e2 * src) / sig;
        for (int i = 0; i < 3; i++)
            nits[i] *= scale;
    }
    float lin[3] = { nits[0] / SDR_WHITE, nits[1] / SDR_WHITE, nits[2] / SDR_WHITE };
    if (c.wide) {
        /* BT.2020 -> BT.709 primaries (linear light) */
        float r = 1.6605f * lin[0] - 0.5876f * lin[1] - 0.0728f * lin[2];
        float g = -0.1246f * lin[0] + 1.1329f * lin[1] - 0.0083f * lin[2];
        float b = -0.0182f * lin[0] - 0.1006f * lin[1] + 1.1187f * lin[2];
        lin[0] = r; lin[1] = g; lin[2] = b;
    }
    /* back to a TV's signal (BT.1886, gamma 2.4) */
    for (int i = 0; i < 3; i++)
        rgb[i] = powf(std::clamp(lin[i], 0.0f, 1.0f), 1 / 2.4f);
}

unsigned vmem_setup(void **opaque, char *chroma, unsigned *width, unsigned *height,
                    unsigned *pitches, unsigned *lines)
{
    (void)opaque;
    /* The hardware decoder's NV12 / P010 are taken as they are (the shader
     * reads the interleaved chroma), everything else as planar 4:2:0.
     * VLCPS5_TWO_PLANES=1 (host tests) asks for NV12 / P010 from any decoder. */
    static const bool force_two = getenv("VLCPS5_TWO_PLANES") != nullptr;
    bool nv12 = !strncmp(chroma, "NV12", 4), p010 = !strncmp(chroma, "P010", 4);
    bool ten = p010 || !strncmp(chroma, "I0A", 3) || !strncmp(chroma, "I2A", 3) ||
               !strncmp(chroma, "I4A", 3);
    bool full = !strncmp(chroma, "J4", 2);
    bool two = nv12 || p010 || force_two;
    fprintf(stderr, "player: VLC offers %.4s %ux%u\n", chroma, *width, *height);
    memcpy(chroma, two ? (ten ? "P010" : "NV12") : ten ? "I0AL" : full ? "J420" : "I420", 4);

    std::lock_guard<std::mutex> g(pic_lock);
    free_slots_locked(gfx.frame_number + GFX_FRAMES + 1);
    Format f = {};
    f.width = *width;
    f.height = *height;
    f.ten_bit = ten;
    f.two_planes = two;
    f.colour = next_colour;
    f.full_range = full || f.colour.full_range;
    next_colour = Colour{};
    uint32_t bpp = ten ? 2 : 1;
    uint32_t cw = (*width + 1) / 2, ch = (*height + 1) / 2;
    /* Rows on 256-byte boundaries: what buffer-to-image copies like best. */
    f.pitches[0] = align_up(*width * bpp, 256);
    f.lines[0] = align_up(*height, 16);
    f.pitches[1] = f.pitches[2] = align_up(cw * bpp * (two ? 2 : 1), 256);
    f.lines[1] = f.lines[2] = align_up(ch, 16);
    if (two)
        f.pitches[2] = f.lines[2] = 0;
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
    info_stale = true;
    generation++;
    fprintf(stderr, "player: pictures %ux%u %s%s%s, %d x %llu KiB\n", f.width, f.height,
            ten ? "10-bit" : "8-bit", two ? (ten ? " P010" : " NV12") : "",
            f.full_range ? " full range" : "", SLOTS,
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
    if (first_picture_wait) {
        first_picture_wait = false;
        fprintf(stderr, "player: first picture %.2f s after the open\n", plat_time() - opened_at);
    }
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
size_t audio_queued_frames; /* frames in audio_queue not played yet (audio_lock) */

void audio_clear_locked()
{
    audio_queue.clear();
    audio_queued_frames = 0;
}
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
/* A video opened while the one before was still stopping: started after. */
libvlc_media_t *queued_media;

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
    /* At most ~4 s of sound queued (something is stuck past that; memory
     * stays bounded). Counted in time, not blocks: TrueHD sends 40-sample
     * blocks (0.8 ms), and 400 of them was a third of a second, less than VLC
     * queues ahead, so the blocks due next were thrown away: no sound. */
    audio_queued_frames += count;
    while (audio_queued_frames > 4 * AUDIO_RATE && !audio_queue.empty()) {
        AudioBlock &old = audio_queue.front();
        audio_queued_frames -= std::min(audio_queued_frames, old.samples.size() / old.channels - old.pos);
        audio_queue.pop_front();
    }
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
    audio_clear_locked();
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
                    if (skip > frames - b.pos)
                        skip = frames - b.pos;
                    b.pos += skip;
                    audio_queued_frames -= std::min(audio_queued_frames, skip);
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
                audio_queued_frames -= std::min(audio_queued_frames, n);
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
    v.hdr = format.colour.transfer;
    v.hardware = vdec_active > 0;
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
    /* the same for HDR10 output's 10-bit frame */
    if (r == VK_SUCCESS && gfx.hdr_pass) {
        gp.renderPass = gfx.hdr_pass;
        if (vkCreateGraphicsPipelines(gfx.device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_hdr) != VK_SUCCESS)
            pipeline_hdr = VK_NULL_HANDLE;
    }
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
    VkFormat vf2 = f.ten_bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;
    VkCommandBuffer cmd = gfx_one_shot_begin();
    for (int i = 0; i < PLANES; i++) {
        planes.w[i] = i ? (f.width + 1) / 2 : f.width;
        planes.h[i] = i ? (f.height + 1) / 2 : f.height;
        /* two planes: the second holds U and V; the third, unused, is a
         * dot the shader's binding still needs */
        if (f.two_planes && i == 2)
            planes.w[i] = planes.h[i] = 1;
        if (!gfx_image(planes.w[i], planes.h[i], f.two_planes && i == 1 ? vf2 : vf,
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
    planes.two_planes = f.two_planes;
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
int chapters_title = -2; /* a disc's chapters belong to its current title */

/* A slot's picture as RGB, cropped to what's visible: the shader's sums on
 * the CPU (its matrix, HDR tone mapped to SDR). */
struct Shot {
    std::vector<uint8_t> data;
    Format f;
    uint32_t w, h;
    std::string path;
};

void shot_main(Shot shot)
{
    const Format &f = shot.f;
    YuvMatrix m = yuv_matrix(f.colour.matrix, shot.h);
    bool hdr = f.colour.transfer != TRC_SDR;
    std::vector<uint8_t> rgb((size_t)shot.w * shot.h * 3);
    /* P010's 10 bits sit at the top of 16, I0AL's at the bottom */
    float ten_max = f.two_planes ? 65535.0f : 1023.0f;
    auto sample = [&](int plane, uint32_t x, uint32_t y) {
        const uint8_t *row = shot.data.data() + f.offsets[plane] + (size_t)y * f.pitches[plane];
        return f.ten_bit ? ((const uint16_t *)row)[x] / ten_max : row[x] / 255.0f;
    };
    for (uint32_t y = 0; y < shot.h; y++)
        for (uint32_t x = 0; x < shot.w; x++) {
            float Y = sample(0, x, y), U, V;
            if (f.two_planes) {
                U = sample(1, x / 2 * 2, y / 2) - 0.5f;
                V = sample(1, x / 2 * 2 + 1, y / 2) - 0.5f;
            } else {
                U = sample(1, x / 2, y / 2) - 0.5f;
                V = sample(2, x / 2, y / 2) - 0.5f;
            }
            if (!f.full_range) {
                Y = (Y - 16.0f / 255) * (255.0f / 219);
                U *= 255.0f / 224;
                V *= 255.0f / 224;
            }
            float c[3] = { Y + m.kr * V, Y + m.kgu * U + m.kgv * V, Y + m.kb * U };
            if (hdr)
                hdr_to_sdr(c, f.colour);
            uint8_t *o = &rgb[((size_t)y * shot.w + x) * 3];
            for (int i = 0; i < 3; i++)
                o[i] = (uint8_t)(c[i] < 0 ? 0 : c[i] > 1 ? 255 : c[i] * 255 + 0.5f);
        }
    bool ok = write_png_rgb(shot.path.c_str(), rgb.data(), shot.w, shot.h);
    fprintf(stderr, "player: screenshot %s %s\n", shot.path.c_str(), ok ? "saved" : "FAILED");
}

} // namespace

/* Picture copies split across a few threads (ps5vdec's out of the decoder,
 * vmem's into our buffers, patches/0016). A 4K 10-bit picture is 25 MiB; one
 * core copying it twice made 4K miss its frames on the console. */
namespace {
const int COPY_THREADS = 4;
struct CopyJob {
    uint8_t *dst;
    const uint8_t *src;
    size_t dst_pitch, src_pitch, bytes;
    unsigned rows;
    unsigned shift;   /* 16-bit samples moved up this many bits (10-bit at the bottom -> P010) */
    uint8_t *dst2;    /* split: interleaved 16-bit U,V -> dst (U) and dst2 (V) */
    size_t dst2_pitch;
};
struct CopyPool {
    std::mutex lock;
    std::condition_variable start, done;
    CopyJob job;
    unsigned serial, finished;
    bool started;
    std::mutex use;   /* one picture at a time */
};
/* Never destroyed: its threads wait on it to the end, and destroying a
 * condition variable they wait on hung the exit (host runs, glibc). */
CopyPool &copy_pool = *new CopyPool();

void copy_rows(const CopyJob &j, int part, int parts)
{
    unsigned from = j.rows * part / parts, to = j.rows * (part + 1) / parts;
    for (unsigned r = from; r < to; r++) {
        if (j.dst2) {
            uint16_t *u = (uint16_t *)(j.dst + r * j.dst_pitch);
            uint16_t *v = (uint16_t *)(j.dst2 + r * j.dst2_pitch);
            const uint16_t *s = (const uint16_t *)(j.src + r * j.src_pitch);
            for (size_t i = 0; i < j.bytes / 4; i++) {
                u[i] = s[2 * i];
                v[i] = s[2 * i + 1];
            }
            continue;
        }
        if (!j.shift) {
            memcpy(j.dst + r * j.dst_pitch, j.src + r * j.src_pitch, j.bytes);
            continue;
        }
        uint16_t *d = (uint16_t *)(j.dst + r * j.dst_pitch);
        const uint16_t *s = (const uint16_t *)(j.src + r * j.src_pitch);
        for (size_t i = 0; i < j.bytes / 2; i++)
            d[i] = (uint16_t)(s[i] << j.shift);
    }
}

void copy_worker(int part)
{
    unsigned seen = 0;
    for (;;) {
        CopyJob j;
        {
            std::unique_lock<std::mutex> g(copy_pool.lock);
            copy_pool.start.wait(g, [&] { return copy_pool.serial != seen; });
            seen = copy_pool.serial;
            j = copy_pool.job;
        }
        copy_rows(j, part, COPY_THREADS);
        std::lock_guard<std::mutex> g(copy_pool.lock);
        if (++copy_pool.finished == COPY_THREADS - 1)
            copy_pool.done.notify_one();
    }
}
} // namespace

static void copy_run(const CopyJob &j);

extern "C" void vlc_ps5_copy_plane_shift(uint8_t *dst, size_t dst_pitch, const uint8_t *src,
                                         size_t src_pitch, size_t bytes, unsigned rows,
                                         unsigned shift)
{
    copy_run({ dst, src, dst_pitch, src_pitch, bytes, rows, shift, nullptr, 0 });
}

/* Interleaved 16-bit chroma (U,V,U,V...) into two planes: the console's
 * decoder's 10-bit NV12-style output as VLC's planar I0AL, which VLC can draw
 * subtitles onto (onto P010 it can't). bytes: of each source row. */
extern "C" void vlc_ps5_split_plane16(uint8_t *dst_u, size_t u_pitch, uint8_t *dst_v, size_t v_pitch,
                                      const uint8_t *src, size_t src_pitch, size_t bytes,
                                      unsigned rows)
{
    copy_run({ dst_u, src, u_pitch, src_pitch, bytes, rows, 0, dst_v, v_pitch });
}

static void copy_run(const CopyJob &j)
{
    size_t rows = j.rows, bytes = j.bytes;
    if (rows * bytes < (1u << 20)) {   /* small: not worth waking anyone */
        copy_rows(j, 0, 1);
        return;
    }
    std::lock_guard<std::mutex> use(copy_pool.use);
    {
        std::lock_guard<std::mutex> g(copy_pool.lock);
        if (!copy_pool.started) {
            for (int i = 1; i < COPY_THREADS; i++)
                std::thread(copy_worker, i).detach();
            copy_pool.started = true;
        }
        copy_pool.job = j;
        copy_pool.finished = 0;
        copy_pool.serial++;
    }
    copy_pool.start.notify_all();
    copy_rows(j, 0, COPY_THREADS);   /* this thread does the first part */
    std::unique_lock<std::mutex> g(copy_pool.lock);
    copy_pool.done.wait(g, [] { return copy_pool.finished == COPY_THREADS - 1; });
}

extern "C" void vlc_ps5_copy_plane(uint8_t *dst, size_t dst_pitch, const uint8_t *src,
                                   size_t src_pitch, size_t bytes, unsigned rows)
{
    vlc_ps5_copy_plane_shift(dst, dst_pitch, src, src_pitch, bytes, rows, 0);
}

/* ps5/modules/ps5vdec.c, from VLC's decoder thread */
extern "C" void vlc_ps5_vdec_failed(void)
{
    vdec_failed = true;
}

extern "C" void vlc_ps5_vdec_active(int on)
{
    vdec_active += on ? 1 : -1;
    info_stale = true;
}

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
        /* No iconv on the console: subtitles are read as UTF-8 (VLC's CP1252
         * fallback only logged "cannot convert" for every file). */
        "--subsdec-encoding=UTF-8",
#endif
        "--ignore-config",
        "--no-video-title-show",
        /* Browse shows every file: VLC's listings (SMB, DLNA, ZIP/RAR) too, not
         * only local folders. VLC hid m3u, images, subtitles, txt... there. */
        "--ignore-filetypes=",
        /* Subtitle files beside a video: our own search, for every source
         * (side_subs_find); VLC's looked beside local files only. */
        "--no-sub-autodetect-file",
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
    var_Create(mp_object(), "vmem-colour", VLC_VAR_ADDRESS);
    var_SetAddress(mp_object(), "vmem-colour", (void *)vmem_colour);
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
    /* already stopping: nothing new can have started since (an open waits
     * for the stop), and waiting for it here froze the app */
    if (stopping)
        return;
    wait_for_stop();
    if (mp) {
        libvlc_media_player_t *p = mp;
        stopping = true;
        stopper = std::thread([p] {
            /* host tests: VLCPS5_SLOW_STOP=<ms> plays a stop stuck on a
             * network stream (the console's 9 s, run 12) */
            if (const char *slow = getenv("VLCPS5_SLOW_STOP"))
                usleep(atoi(slow) * 1000);
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
    return player_open_media(m, path, start_ms, options);
}

std::string file_uri(const std::string &path);

/* A subtitle file's address for VLC: a share's or a link's as it is, a
 * local path made a file:// URI. */
std::string sub_uri(const std::string &path)
{
    return path.find("://") != std::string::npos ? path : file_uri(path);
}

extern "C" input_thread_t *libvlc_get_input_thread(libvlc_media_player_t *);

/* The language a subtitle file is most likely in, for one that isn't UTF-8:
 * the subtitle language chosen, else the menus'. */
std::string sub_lang_hint()
{
    std::string s = player_sub_language();
    return s.empty() || s == "off" || s == "none" ? std::string(lang_code(lang_current())) : s;
}

/* A subtitle file on a share or a link, read through VLC as a child of the
 * playing input (so a share's login comes with it); at most 8 MiB. False
 * when it can't be opened (a link's guess that isn't there). */
bool read_remote_sub(const std::string &uri, std::string *out)
{
    input_thread_t *input = nullptr;
    /* the open may still be on its way */
    for (int i = 0; i < 50 && !(input = libvlc_get_input_thread(mp)); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!input)
        return false;
    stream_t *s = vlc_stream_NewURL((vlc_object_t *)input, uri.c_str());
    bool ok = s != nullptr;
    out->clear();
    if (s) {
        char buf[65536];
        ssize_t n;
        while ((n = vlc_stream_Read(s, buf, sizeof(buf))) > 0 && out->size() < (8u << 20))
            out->append(buf, (size_t)n);
        vlc_stream_Delete(s);
        ok = !out->empty();
    }
    vlc_object_release((vlc_object_t *)input);
    return ok;
}

/* A subtitle file ready for VLC: a local path, or the address of one on a
 * share or a link; one that isn't UTF-8 becomes a converted local copy
 * (the console's VLC has no iconv). "" when a remote one can't be read. */
std::string sub_ready(const std::string &path, const std::string &hint)
{
    if (path.find("://") == std::string::npos)
        return subconv_local(path, hint);
    std::string bytes;
    if (!read_remote_sub(path, &bytes))
        return "";
    std::string b = path.substr(0, path.find_first_of("?#"));
    char *name = vlc_uri_decode_duplicate(b.substr(b.rfind('/') + 1).c_str());
    std::string copy = subconv_bytes(bytes, name ? name : "subtitle.srt", hint);
    free(name);
    return copy.empty() ? path : copy;
}

/* ---- subtitles beside the video ------------------------------------------------
 * Ours, the same for every source: VLC's own search only looks beside local
 * files, and the ones its share listings attach weren't turned on. A
 * subtitle file goes with a video when its name starts with the video's
 * (without the extension) followed by nothing or a separator: "Film.srt",
 * "Film.en.srt", "Film - English.ass". Local folders and shares (SMB, FTP,
 * SFTP, NFS) are listed; a link (http, https) is asked for Film.srt. */

bool is_sub_name(const std::string &lower_name)
{
    static const char *const exts[] = { "srt", "ass", "ssa", "vtt", "sub", "idx", "smi", "sami", "ttml",
                                        "usf", "jss", "psb", "rt", "mpl2", "pjs", "stl", "dks" };
    size_t dot = lower_name.rfind('.');
    if (dot == std::string::npos)
        return false;
    for (const char *e : exts)
        if (lower_name.compare(dot + 1, std::string::npos, e) == 0)
            return true;
    return false;
}

std::string lowered(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

/* name (a file's) goes with the video named base (no extension), both lowercase */
bool sub_goes_with(const std::string &base, const std::string &name)
{
    if (base.empty() || name.size() <= base.size() || name.compare(0, base.size(), base) != 0 ||
        !is_sub_name(name))
        return false;
    char c = name[base.size()];
    return c == '.' || c == ' ' || c == '-' || c == '_' || c == '[' || c == '(';
}

/* The matches, best first: Film.srt, then the others by name; a VobSub .sub
 * goes only through its .idx. */
std::vector<std::string> sort_side_subs(const std::string &base, std::vector<std::string> names)
{
    std::vector<std::string> lower;
    for (const std::string &n : names)
        lower.push_back(lowered(n));
    std::vector<size_t> order;
    for (size_t i = 0; i < names.size(); i++) {
        const std::string &l = lower[i];
        if (l.size() > 4 && !l.compare(l.size() - 4, 4, ".sub") &&
            std::find(lower.begin(), lower.end(), l.substr(0, l.size() - 4) + ".idx") != lower.end())
            continue;
        order.push_back(i);
    }
    std::string exact = base + ".srt";
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        bool ea = lower[a] == exact, eb = lower[b] == exact;
        if (ea != eb)
            return ea;
        return lower[a] < lower[b];
    });
    std::vector<std::string> out;
    for (size_t i : order)
        out.push_back(names[i]);
    return out;
}

/* A local video: its folder and the usual subtitle folders inside it. */
std::vector<std::string> local_side_subs(const std::string &path)
{
    size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return {};
    std::string dir = path.substr(0, slash), file = path.substr(slash + 1);
    std::string base = lowered(file.substr(0, file.rfind('.')));
    std::vector<std::string> found;
    static const char *const subdirs[] = { "", "/Subs", "/subs", "/Subtitles", "/subtitles", "/Sub", "/sub" };
    for (const char *sd : subdirs) {
        std::string d = dir + sd;
        DIR *h = opendir(d.c_str());
        if (!h)
            continue;
        std::vector<std::string> names;
        while (struct dirent *e = readdir(h))
            if (e->d_name[0] != '.' && sub_goes_with(base, lowered(e->d_name)))
                names.push_back(e->d_name);
        closedir(h);
        for (const std::string &n : sort_side_subs(base, names)) {
            std::string full = d + "/" + n;
            if (std::find(found.begin(), found.end(), full) == found.end())
                found.push_back(full);
        }
    }
    return found;
}

bool url_scheme_is(const std::string &url, const char *scheme)
{
    size_t n = strlen(scheme);
    return url.size() > n + 3 && !strncasecmp(url.c_str(), scheme, n) && !url.compare(n, 3, "://");
}

/* A share or a link: in the background, then player_tick adds what it found. */
void side_subs_find(const std::string &url, int serial)
{
    std::string bare = url.substr(0, url.find_first_of("?#"));
    size_t slash = bare.rfind('/');
    if (slash == std::string::npos || slash < bare.find("://") + 3)
        return;
    std::string parent = bare.substr(0, slash + 1), last = bare.substr(slash + 1);
    char *dec = vlc_uri_decode_duplicate(last.c_str());
    std::string file = dec ? dec : last;
    free(dec);
    size_t dot = file.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return; /* a stream's address, not a file */
    std::string ext = lowered(file.substr(dot + 1));
    if (ext == "m3u8" || ext == "m3u" || ext == "mpd" || ext == "pls" || ext == "iso" || ext == "img" || is_sub_name(lowered(file)))
        return;
    std::string base = lowered(file.substr(0, dot));
    bool listable = url_scheme_is(url, "smb") || url_scheme_is(url, "ftp") || url_scheme_is(url, "ftps") ||
                    url_scheme_is(url, "sftp") || url_scheme_is(url, "nfs");
    bool link = url_scheme_is(url, "http") || url_scheme_is(url, "https");
    if (!listable && !link)
        return;
    std::vector<std::string> opts = source_options;
    /* its own reference: a quit during the listing (up to 8 s) mustn't free
     * VLC under it */
    libvlc_instance_t *inst = vlc;
    libvlc_retain(inst);
    std::string hint = sub_lang_hint();
    std::thread([=]() {
        std::vector<std::string> uris;
        if (link) {
            /* No folder to list: the one most links have beside them. */
            uris.push_back(bare.substr(0, bare.size() - (file.size() - dot)) + ".srt");
            (void)base;
        } else if (libvlc_media_t *m = libvlc_media_new_location(inst, parent.c_str())) {
            for (const std::string &o : opts)
                libvlc_media_add_option(m, o.c_str());
            libvlc_media_parse_with_options(m, libvlc_media_parse_network, 8000);
            for (int i = 0; i < 100 && libvlc_media_get_parsed_status(m) == 0; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::vector<std::string> names, mrls;
            if (libvlc_media_list_t *list = libvlc_media_subitems(m)) {
                libvlc_media_list_lock(list);
                for (int i = 0; i < libvlc_media_list_count(list); i++) {
                    libvlc_media_t *c = libvlc_media_list_item_at_index(list, i);
                    if (!c)
                        continue;
                    if (char *mrl = libvlc_media_get_mrl(c)) {
                        std::string u = mrl;
                        std::string b = u.substr(0, u.find_first_of("?#"));
                        char *n = vlc_uri_decode_duplicate(b.substr(b.rfind('/') + 1).c_str());
                        if (n && sub_goes_with(base, lowered(n))) {
                            names.push_back(n);
                            mrls.push_back(u);
                        }
                        free(n);
                        free(mrl);
                    }
                    libvlc_media_release(c);
                }
                libvlc_media_list_unlock(list);
                libvlc_media_list_release(list);
            }
            libvlc_media_release(m);
            for (const std::string &n : sort_side_subs(base, names))
                uris.push_back(mrls[std::find(names.begin(), names.end(), n) - names.begin()]);
        }
        /* read through VLC: one that isn't UTF-8 becomes a converted copy,
         * a link's guess that isn't there is dropped */
        std::vector<std::string> ready;
        for (const std::string &u : uris) {
            std::string r = sub_ready(u, hint);
            if (!r.empty())
                ready.push_back(r.find("://") == std::string::npos ? file_uri(r) : r);
        }
        uris.swap(ready);
        libvlc_release(inst);
        fprintf(stderr, "player: %zu subtitle file(s) beside %s\n", uris.size(), url.c_str());
        std::lock_guard<std::mutex> g(side.lock);
        if (side.serial != serial)
            return;
        side.path = url;
        side.uris = uris;
        side.ready = true;
    }).detach();
}

/* Found ones go in once the video plays: the first turned on (unless
 * subtitles are set to Off), the others listed in Tracks. */
void side_subs_apply()
{
    std::vector<std::string> uris;
    {
        std::lock_guard<std::mutex> g(side.lock);
        if (!side.ready || side.path != current_path)
            return;
        side.ready = false;
        uris = side.uris;
    }
    /* A media from a listing opened again still has the ones that loaded
     * last time (VLC keeps them on the item): not twice. */
    std::vector<std::string> have;
    if (libvlc_media_t *m = libvlc_media_player_get_media(mp)) {
        libvlc_media_slave_t **slaves = nullptr;
        unsigned n = libvlc_media_slaves_get(m, &slaves);
        for (unsigned i = 0; i < n; i++)
            if (slaves[i]->psz_uri)
                have.push_back(slaves[i]->psz_uri);
        libvlc_media_slaves_release(slaves, n);
        libvlc_media_release(m);
    }
    bool on = player_sub_language() != "off";
    for (size_t i = 0; i < uris.size(); i++) {
        if (std::find(have.begin(), have.end(), uris[i]) != have.end()) {
            fprintf(stderr, "player: subtitle beside the video %s: already there\n", uris[i].c_str());
            continue;
        }
        int r = libvlc_media_player_add_slave(mp, libvlc_media_slave_type_subtitle, uris[i].c_str(), on && i == 0);
        fprintf(stderr, "player: subtitle beside the video %s: %s\n", uris[i].c_str(), r == 0 ? "added" : "failed");
    }
}

static bool start_queued();

bool player_open_media(libvlc_media_t *m, const std::string &path, int64_t start_ms,
                       const std::vector<std::string> &options)
{
    /* A reopen of the same file (Auto's, Watch again) keeps its login. */
    if (!options.empty() || path != current_path)
        source_options = options;
    {
        std::lock_guard<std::mutex> g(side.lock);
        side.serial++;
        side.ready = false;
    }
    bool url = path.find("://") != std::string::npos;
    std::string local = path;
    if (url_scheme_is(path, "file")) {
        char *p = vlc_uri2path(path.c_str());
        local = p ? p : "";
        free(p);
        url = false;
    }
    if (url) {
        side_subs_find(path, side.serial);
    } else if (!local.empty()) {
        std::vector<std::string> subs = local_side_subs(local);
        bool on = player_sub_language() != "off";
        std::string hint = sub_lang_hint();
        for (size_t i = 0; i < subs.size(); i++) {
            /* 4 (the user's): VLC turns the first one on; 3: only listed */
            libvlc_media_slaves_add(m, libvlc_media_slave_type_subtitle, on && i == 0 ? 4 : 3,
                                    file_uri(sub_ready(subs[i], hint)).c_str());
            fprintf(stderr, "player: subtitle beside the video: %s\n", subs[i].c_str());
        }
    }
    if (path != kept.path) {
        /* another file: upright, no delays, its own tracks */
        picture.rotate = 0;
        picture.mirror = picture.upside_down = false;
        kept = { path, 0, 0, -2, -2, {} };
    } else {
        /* the same one again: what was set comes back once it plays */
        /* (the ones beside the video, named like it, are found again) */
        std::string base = path.substr(0, path.rfind('.'));
        for (const std::string &sub : kept.subs)
            if (sub_attach_all || sub.compare(0, base.size(), base) != 0)
                libvlc_media_slaves_add(m, libvlc_media_slave_type_subtitle, 4, sub_uri(sub).c_str());
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
    /* A Blu-ray checks the player's region (VLC's default is B, Europe): ours. */
    libvlc_media_add_option(m, (":bluray-region=" + player_bluray_region()).c_str());
    std::string alang = player_audio_language(), slang = player_sub_language();
    if (!alang.empty())
        libvlc_media_add_option(m, (":audio-language=" + alang).c_str());
    if (!slang.empty())
        libvlc_media_add_option(m, (":sub-language=" + (slang == "off" ? std::string("none") : slang)).c_str());
    chapters_cache.clear();
    chapters_tried = 0;
    chapters_title = -2;
    fast_now = decode_mode == DECODE_FAST || (decode_mode == DECODE_AUTO && path == fast_path);
    if (fast_now) {
        libvlc_media_add_option(m, ":avcodec-skiploopfilter=4");
        libvlc_media_add_option(m, ":avcodec-fast");
    }
    bool software = path == software_path || setting_int("hw_decode", 1) == 0;
    if (software)
        libvlc_media_add_option(m, ":no-ps5vdec");
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
    fprintf(stderr, "player: open %s at %lld ms%s%s\n", path.c_str(), (long long)start_ms,
            fast_now ? " (fast decoding)" : "", software ? " (software decoding)" : "");
    /* The video before may still be stopping: a stream stuck on its network
     * kept VLC's stop waiting 9 s, and waiting for it here froze the whole
     * app (console run 12). This one then starts from player_tick once the
     * stop is done. */
    if (queued_media)
        libvlc_media_release(queued_media);
    queued_media = m;
    if (stopping) {
        fprintf(stderr, "player: the video before is still stopping: this one starts after\n");
        return true;
    }
    return start_queued();
}

/* The video player_open_media queued, into VLC (once no stop is running). */
static bool start_queued()
{
    libvlc_media_t *m = queued_media;
    if (!m)
        return false;
    queued_media = nullptr;
    wait_for_stop();
    libvlc_media_player_set_media(mp, m);
    if (last_media)
        libvlc_media_release(last_media);
    last_media = m; /* our reference: kept for a live pause */
    {
        std::lock_guard<std::mutex> g(pic_lock);
        opened_at = plat_time();
        first_picture_wait = true;
    }
    /* the health check counts from the real start, not the queueing */
    if (health.next < plat_time() + 3)
        health.next = plat_time() + 3;
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

bool add_ready_subtitle(const std::string &path, const std::string &ready);

/* The file playing is a disc: an image (.iso, .img), dvd:// or bluray://, or
 * anything VLC sees several titles in (a VIDEO_TS / BDMV folder). */
bool playing_disc_now()
{
    std::string bare = lowered(current_path.substr(0, current_path.find('?')));
    if (bare.size() > 4 && (!bare.compare(bare.size() - 4, 4, ".iso") || !bare.compare(bare.size() - 4, 4, ".img")))
        return true;
    if (url_scheme_is(bare, "dvd") || url_scheme_is(bare, "bluray"))
        return true;
    return mp && libvlc_media_player_get_title_count(mp) > 1;
}

PlayerEvent player_tick()
{
    double t = plat_time();
    std::vector<PendingSub> done;
    {
        std::lock_guard<std::mutex> g(pending_lock);
        done.swap(pending_subs);
    }
    for (const PendingSub &p : done)
        if (mp && p.serial == side.serial)
            add_ready_subtitle(p.path, p.ready);
    /* a video waiting for the one before to finish stopping */
    if (queued_media && !stopping) {
        fprintf(stderr, "player: the video before stopped: starting %s\n", current_path.c_str());
        start_queued();
    }
    if (!mp || t < health.next)
        return PLAYER_EVENT_NONE;
    health.next = t + 1;
    if (state() == libvlc_Playing)
        side_subs_apply();
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
                audio_clear_locked();
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
    if (vdec_failed.exchange(false) && !current_path.empty()) {
        /* the console's decoder gave up on this video: FFmpeg, same place */
        fprintf(stderr, "player: reopening %s without hardware decoding\n", current_path.c_str());
        restore_audio = libvlc_audio_get_track(mp);
        restore_spu = libvlc_video_get_spu(mp);
        software_path = current_path;
        int64_t at = libvlc_media_player_get_time(mp);
        std::string path = current_path;
        libvlc_media_player_stop(mp);
        {
            std::lock_guard<std::mutex> g(audio_lock);
            audio_clear_locked();
        }
        player_open(path, at > 0 ? at : 0);
        return PLAYER_EVENT_NONE;
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
    /* Not on a disc: its menus and still screens show few pictures by design
     * (a Blu-ray menu looked like "can't keep up" every second), and Auto's
     * reopen would throw the menus away. */
    if (playing_disc_now())
        playing = false;
    bool bad = playing && decoded > 0 &&
               (lost > shown || shown < fps * 0.5f);
    health.bad_seconds = bad ? health.bad_seconds + 1 : 0;
    if (bad)
        fprintf(stderr, "player: can't keep up: %d decoded, %d shown, %d lost in 1 s (%.2f fps file)\n",
                decoded, shown, lost, fps);
    if (health.bad_seconds < 3)
        return PLAYER_EVENT_NONE;
    health.bad_seconds = 0;
    /* fast decoding is FFmpeg's: nothing to gain on the console's decoder */
    if (!fast_now && decode_mode == DECODE_AUTO && vdec_active == 0) {
        fprintf(stderr, "player: switching %s to fast decoding\n", current_path.c_str());
        restore_audio = libvlc_audio_get_track(mp);
        restore_spu = libvlc_video_get_spu(mp);
        fast_path = current_path;
        int64_t at = libvlc_media_player_get_time(mp);
        std::string path = current_path;
        libvlc_media_player_stop(mp);
        {
            std::lock_guard<std::mutex> g(audio_lock);
            audio_clear_locked();
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
    if (queued_media) {   /* stopped before it even started */
        libvlc_media_release(queued_media);
        queued_media = nullptr;
    }
    stop_in_background();
    std::lock_guard<std::mutex> g(audio_lock);
    audio_clear_locked();
    audio_paused = false;
}

static libvlc_state_t state()
{
    return mp ? libvlc_media_player_get_state(mp) : libvlc_NothingSpecial;
}

bool player_active()
{
    libvlc_state_t s = state();
    return live_paused || queued_media || s == libvlc_Opening || s == libvlc_Buffering ||
           s == libvlc_Playing || s == libvlc_Paused;
}

bool player_ended()
{
    if (queued_media)
        return false;   /* the next one is about to start */
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

/* libvlc's own (lib/media_player_internal.h): the input, held. */
extern "C" input_thread_t *libvlc_get_input_thread(libvlc_media_player_t *);

bool player_in_menu()
{
    /* Read four times a second: the title list is copied out on every call. */
    static double next;
    static bool menu;
    double now = plat_time();
    if (now < next)
        return menu;
    next = now + 0.25;
    menu = false;
    if (!mp)
        return menu;
    /* A DVD says when menu buttons are on screen ("highlight", set by VLC's
     * dvdnav). A DVD's intro or warning screen is in its menu part too but has
     * no buttons: there ✕ must pause, not press a button nobody sees. */
    if (input_thread_t *input = libvlc_get_input_thread(mp)) {
        vlc_object_t *in = (vlc_object_t *)input; /* input_thread_t is opaque here */
        /* A Blu-ray says when its menu is on screen, in any title
         * (patches/0010); the "Top Menu" title alone missed most discs. Asked
         * first: VLC's subtitle unit puts a "highlight" on every input with
         * a video output, so that one alone doesn't mean a DVD. */
        bool bd = var_Type(in, "bluray-menu-open") != 0;
        bool dvd = !bd && var_Type(in, "highlight") != 0;
        if (dvd)
            menu = var_GetBool(in, "highlight");
        else if (bd)
            menu = var_GetBool(in, "bluray-menu-open");
        vlc_object_release(in);
        static int logged = -1;
        if ((dvd || bd) && logged != (int)menu) {
            logged = menu;
            fprintf(stderr, "player: %s menu %s\n", dvd ? "DVD" : "Blu-ray", menu ? "on screen" : "gone");
        }
        if (dvd || bd)
            return menu;
    }
    /* A Blu-ray played without menus: its menu titles. */
    int cur = libvlc_media_player_get_title(mp);
    libvlc_title_description_t **titles = nullptr;
    int n = libvlc_media_player_get_full_title_descriptions(mp, &titles);
    if (n > 0 && cur >= 0 && cur < n && titles[cur])
        /* menu only: a Blu-ray marks its movie "interactive" too (pop-up
         * menus), and ✕ must still pause that */
        menu = (titles[cur]->i_flags & libvlc_title_menu) != 0;
    if (n > 0)
        libvlc_title_descriptions_release(titles, (unsigned)n);
    return menu;
}

/* The disc's main menu (a DVD's root menu, a Blu-ray's Top Menu) or a
 * Blu-ray's pop-up menu, over the film. */
void player_disc_menu(bool popup)
{
    if (!mp)
        return;
    if (input_thread_t *input = libvlc_get_input_thread(mp)) {
        vlc_object_t *in = (vlc_object_t *)input;
        const char *var = popup ? "menu-popup" : "menu-title";
        if (var_Type(in, var) != 0)
            var_TriggerCallback(in, var);
        fprintf(stderr, "player: disc %s\n", popup ? "pop-up menu" : "menu");
        vlc_object_release(in);
    }
}

bool player_disc_has_popup()
{
    bool popup = false;
    if (mp)
        if (input_thread_t *input = libvlc_get_input_thread(mp)) {
            vlc_object_t *in = (vlc_object_t *)input;
            popup = var_Type(in, "bluray-popup") != 0 && var_GetBool(in, "bluray-popup");
            vlc_object_release(in);
        }
    return popup;
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
        /* back to the live point: queued like an open, so a stop still
         * stuck on the network doesn't freeze the app */
        live_paused = false;
        if (last_media) {
            libvlc_media_retain(last_media);
            if (queued_media)
                libvlc_media_release(queued_media);
            queued_media = last_media;
            if (!stopping)
                start_queued();
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
        audio_clear_locked();
        return;
    }
    bool want = !player_paused();
    fprintf(stderr, "player: %s\n", want ? "pause" : "play");
    libvlc_media_player_set_pause(mp, want ? 1 : 0);
    if (!want) {
        /* the second that plays on from a pause starts slowly: not a
         * "can't keep up" (the count starts again after it) */
        health.primed = false;
        health.next = plat_time() + 2;
    }
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
    size_t queued, queued_ms;
    {
        std::lock_guard<std::mutex> g(audio_lock);
        queued = audio_queue.size();
        queued_ms = audio_queued_frames * 1000 / AUDIO_RATE;
    }
    libvlc_media_stats_t s = {};
    get_stats(&s);
    fprintf(stderr, "player: state %d, time %lld / %lld ms, slots [%s], audio blocks %zu (%zu ms), frame %llu done %llu, "
            "pictures +%d decoded +%d shown +%d lost, input %.0f kbit/s%s\n",
            (int)state(), (long long)player_time(), (long long)player_length(), slots_text, queued, queued_ms,
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

bool player_is_subtitle_name(const std::string &name)
{
    return is_sub_name(lowered(name));
}

bool player_add_subtitle(const std::string &path)
{
    if (!mp)
        return false;
    /* A full path or an address only: VLC makes a relative one full with
     * getcwd(), which crashed on the console. */
    if (path.empty() || (path[0] != '/' && path.find("://") == std::string::npos)) {
        fprintf(stderr, "player: not a subtitle file address: \"%s\"\n", path.c_str());
        return false;
    }
    if (path.find("://") != std::string::npos) {
        /* On a share or a link: read (and made UTF-8) in the background, put
         * in by player_tick; read here, the picture froze ~0.6 s. */
        int serial = side.serial;
        std::string hint = sub_lang_hint();
        libvlc_instance_t *inst = vlc;
        libvlc_retain(inst);
        std::thread([=]() {
            std::string ready = sub_ready(path, hint);
            libvlc_release(inst);
            std::lock_guard<std::mutex> g(pending_lock);
            pending_subs.push_back({ path, ready, serial });
        }).detach();
        return true;
    }
    /* not UTF-8: a converted copy */
    return add_ready_subtitle(path, sub_ready(path, sub_lang_hint()));
}

/* A subtitle file read and made ready: into the file playing. */
bool add_ready_subtitle(const std::string &path, const std::string &ready)
{
    if (ready.empty()) {
        fprintf(stderr, "player: subtitle file %s: can't be read\n", path.c_str());
        return false;
    }
    sub_check_count = player_subtitle_tracks().size();
    sub_check_at = plat_time() + 4;
    int r = libvlc_media_player_add_slave(mp, libvlc_media_slave_type_subtitle, sub_uri(ready).c_str(), true);
    fprintf(stderr, "player: subtitle file %s: %s\n", path.c_str(), r == 0 ? "added" : "failed");
    /* the file VLC got is the one a reopen attaches again */
    if (r == 0 && std::find(kept.subs.begin(), kept.subs.end(), ready) == kept.subs.end())
        kept.subs.push_back(ready);
    /* VLC jumps to put the new track in step and decodes its way back (a
     * console log: 73 decoded, 11 shown 4 s after): not "can't keep up" */
    health.primed = false;
    health.next = plat_time() + 6;
    return r == 0;
}

const VideoInfo &player_video_info()
{
    if (info_stale.exchange(false) && info.width) {
        fprintf(stderr, "player: picture format changed: size and shape read again\n");
        info = VideoInfo{};
        info_tries = 0;
    }
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
                    f.ten_bit == planes.ten_bit && f.full_range == planes.full_range &&
                    f.two_planes == planes.two_planes;
        if (!same && !make_planes(f))
            return;
        planes.generation = gen;
        planes.colour = f.colour;
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
    for (int i = 0; i < (f.two_planes ? 2 : PLANES); i++) {
        uint32_t texel = bpp * (f.two_planes && i == 1 ? 2 : 1);
        gfx_barrier(cmd, planes.image[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {};
        region.bufferOffset = f.offsets[i];
        region.bufferRowLength = f.pitches[i] / texel;
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

bool video_wants_hdr_output()
{
    /* settings.txt hdr_output: 0 never, 1 when the console's output is HDR
     * (the default), 2 always (a TV the console doesn't report as HDR) */
    static const int mode = setting_int("hdr_output", 1);
    if (mode == 0 || !planes.has_picture || planes.colour.transfer == TRC_SDR || !player_active())
        return false;
    return mode == 2 || gfx.display_range == 2;
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
    /* The matrix VLC names; if it doesn't, BT.709 for HD and BT.601 for SD. */
    uint32_t vis_h = v.height ? v.height : planes.buf_h;
    YuvMatrix m = yuv_matrix(planes.colour.matrix, vis_h);
    float r0[4] = { 1, 0, m.kr, 0 }, r1[4] = { 1, m.kgu, m.kgv, 0 }, r2[4] = { 1, m.kb, 0, 0 };
    memcpy(p.row0, r0, 16); memcpy(p.row1, r1, 16); memcpy(p.row2, r2, 16);
    p.hdr[0] = (float)planes.colour.transfer;
    p.hdr[1] = planes.colour.transfer == TRC_PQ ? planes.colour.peak : 1000;
    p.hdr[2] = planes.colour.wide ? 1.0f : 0.0f;
    /* I0AL keeps 10 bits at the bottom of 16, P010 at the top */
    p.params[0] = planes.ten_bit && !planes.two_planes ? 65535.0f / 1023.0f : 1.0f;
    p.hdr[3] = planes.two_planes ? 1.0f : 0.0f;
    p.outp[0] = gfx.hdr && pipeline_hdr ? 1.0f : 0.0f;
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
    /* The whole buffer is picture: vmem's pictures fill the size it set up
     * (1024x560 arrives stretched to 1024x578), so nothing is cut; the
     * rectangle has the picture's real shape. */
    p.uv_scale[0] = 1;
    p.uv_scale[1] = 1;
    VkViewport vp = { x, y, w, h, 0, 1 };
    /* The scissor stays on screen ("Fill" makes the rectangle larger than it). */
    float sx0 = x < 0 ? 0 : x, sy0 = y < 0 ? 0 : y;
    float sx1 = x + w > gfx.width ? gfx.width : x + w, sy1 = y + h > gfx.height ? gfx.height : y + h;
    VkRect2D sc = { { (int32_t)sx0, (int32_t)sy0 },
                    { (uint32_t)(sx1 - sx0 + 0.5f), (uint32_t)(sy1 - sy0 + 0.5f) } };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx.hdr && pipeline_hdr ? pipeline_hdr : pipeline);
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

/* The title is a disc menu (a DVD's "DVD Menu" lists Resume, Root, Title...
 * as chapters: not chapters of anything). */
static bool title_is_menu(int title)
{
    libvlc_title_description_t **titles = nullptr;
    int n = libvlc_media_player_get_full_title_descriptions(mp, &titles);
    bool menu = n > 0 && title >= 0 && title < n && titles[title] && (titles[title]->i_flags & libvlc_title_menu);
    if (n > 0)
        libvlc_title_descriptions_release(titles, (unsigned)n);
    return menu;
}

std::vector<Chapter> player_chapters()
{
    if (!mp)
        return chapters_cache;
    /* A disc moves between titles (menu, intro, the film): each has its own
     * chapters, so the list is read again when the title changes. */
    int title = libvlc_media_player_get_title(mp);
    if (title != chapters_title) {
        chapters_cache.clear();
        chapters_tried = 0;
        chapters_title = title;
    }
    /* Known once the file is open; asked again at most once a second until then. */
    if (!chapters_cache.empty() || plat_time() < chapters_tried + 1)
        return chapters_cache;
    chapters_tried = plat_time();
    if (title_is_menu(title))
        return chapters_cache;
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
    /* the whole buffer: the picture fills it (see video_draw) */
    shot.w = shot.f.width;
    shot.h = shot.f.height;
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

/* A, B or C: the saved choice, else the region of the interface's language
 * (A: the Americas, Japan, Korea, South-East Asia, Taiwan; B: Europe, the
 * Middle East, Africa, Oceania; C: China, Russia, India). */
std::string player_bluray_region()
{
    std::string r = pref_str("bd_region", "");
    if (r == "A" || r == "B" || r == "C")
        return r;
    std::string code = lang_code(lang_current());
    if (code == "zh-CN" || code == "ru")
        return "C";
    if (code == "en" || code == "es" || code == "pt-BR" || code == "ja" || code == "ko" || code == "zh-TW" || code == "id")
        return "A";
    return "B";
}

void player_set_bluray_region(const std::string &region)
{
    pref_set("bd_region", region);
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
