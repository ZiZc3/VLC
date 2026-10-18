/*
 * ps5vdec: H.264, HEVC and VP9 (8 and 10-bit) up to 4K on the console's video
 * decoder (libSceVideodec2), ahead of FFmpeg. A 4K HEVC 10-bit film at
 * 70 Mbps is 6 to 11 pictures a second on the CPU; the decoder block plays it
 * in real time.
 *
 * What it needs (facts from the console's API, our own code):
 *   - libSceVideodec2 loaded (sysmodule 207) before the app's full access,
 *     which platform_ps5.c does at start; vlc_ps5_vdec_available() says so.
 *   - a compute queue (once), then per video: the decoder's memory (CPU,
 *     GPU, CPU+GPU), the access units and the output pictures in direct
 *     memory the GPU can reach.
 *   - access units in Annex B (start codes), parameter sets in front of the
 *     first one. VLC's h264/hevc packetizers already give that; avcC/hvcC
 *     (length-prefixed) input is converted here anyway.
 * Pictures come out in display order as NV12 (8-bit) or P010 (10-bit), plain
 * rows with a pitch: copied into VLC's picture, which the player's shader
 * reads as two planes.
 *
 * Anything it can't take (other codecs, H.264 High 10/4:2:2, bigger than
 * 4K, interlaced pictures it rejects) is left to FFmpeg; a decoder that
 * fails after starting asks the player to reopen the file without it
 * (vlc_ps5_vdec_failed), at the same time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define MODULE_NAME ps5vdec
#define MODULE_STRING "ps5vdec"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>

/* ---- the console's decoder (libSceVideodec2) ----------------------------- */

enum {
    VDEC_CODEC_AVC = 1,
    VDEC_CODEC_HEVC = 974921,
    VDEC_CODEC_VP9 = 2382845,
};

typedef struct {
    uint64_t this_size;
    uint32_t resource_type;       /* 1: decoding on a compute queue */
    uint32_t codec_type;
    uint32_t profile;
    uint32_t max_level;
    int32_t max_frame_width;
    int32_t max_frame_height;
    int32_t max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void *compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t cpu_thread_priority;
    bool optimize_progressive_video;
    bool check_memory_type;
    uint8_t reserved0;
    uint8_t reserved1;
    void *extra_config_info;
} VdecConfig;

typedef struct {
    uint64_t this_size;
    uint64_t cpu_memory_size;
    void *cpu_memory;
    uint64_t gpu_memory_size;
    void *gpu_memory;
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment;
    uint32_t reserved0;
} VdecMemory;

typedef struct {
    uint64_t this_size;
    void *au_data;
    uint64_t au_size;
    uint64_t pts_data;
    uint64_t dts_data;
    uint64_t attached_data;
} VdecInput;

typedef struct {
    uint64_t this_size;
    bool is_valid;
    bool is_error_frame;
    uint8_t picture_count;
    uint32_t codec_type;
    uint32_t frame_width;
    uint32_t frame_pitch;         /* in samples */
    uint32_t frame_height;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format;
    uint32_t frame_pitch_in_bytes;
} VdecOutput;

typedef struct {
    uint64_t this_size;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    bool is_accepted;
} VdecFrameBuffer;

typedef struct {
    uint64_t this_size;
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
} VdecComputeMemory;

typedef struct {
    uint64_t this_size;
    uint16_t compute_pipe_id;
    uint16_t compute_queue_id;
    bool check_memory_type;
    uint8_t reserved0;
    uint16_t reserved1;
} VdecComputeConfig;

/* The start every picture-information struct shares (AVC 0x78, HEVC 0xB8). */
typedef struct {
    uint64_t this_size;
    bool is_valid;
    uint64_t pts_data;
    uint64_t dts_data;
    uint64_t attached_data;
} VdecPictureHead;

_Static_assert(sizeof(VdecConfig) == 0x48, "decoder config");
_Static_assert(sizeof(VdecMemory) == 0x48, "decoder memory");
_Static_assert(sizeof(VdecInput) == 0x30, "input");
_Static_assert(sizeof(VdecOutput) == 0x38, "output");
_Static_assert(sizeof(VdecFrameBuffer) == 0x20, "frame buffer");
_Static_assert(sizeof(VdecComputeMemory) == 0x18, "compute memory");
_Static_assert(sizeof(VdecComputeConfig) == 0x10, "compute config");

int sceVideodec2QueryComputeMemoryInfo(VdecComputeMemory *info);
int sceVideodec2AllocateComputeQueue(const VdecComputeConfig *config, const VdecComputeMemory *memory,
                                     void **queue);
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig *config, VdecMemory *memory);
int sceVideodec2CreateDecoder(const VdecConfig *config, const VdecMemory *memory, void **decoder);
int sceVideodec2DeleteDecoder(void *decoder);
int sceVideodec2Decode(void *decoder, const VdecInput *input, VdecFrameBuffer *frame, VdecOutput *output);
int sceVideodec2Flush(void *decoder, VdecFrameBuffer *frame, VdecOutput *output);
int sceVideodec2Reset(void *decoder);
int sceVideodec2GetPictureInfo(const VdecOutput *output, void *first, void *second);

int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                  size_t alignment, int type, int64_t *start);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t start,
                             size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
int sceKernelReleaseFlexibleMemory(void *addr, size_t len);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelAvailableFlexibleMemorySize(size_t *available);

/* platform_ps5.c: the library loaded at start; the player: reopen without us,
 * and its copy split over threads */
extern int vlc_ps5_vdec_available(void) __attribute__((weak));
extern void vlc_ps5_copy_plane_shift(uint8_t *dst, size_t dst_pitch, const uint8_t *src,
                                     size_t src_pitch, size_t bytes, unsigned rows,
                                     unsigned shift) __attribute__((weak));
extern void vlc_ps5_split_plane16(uint8_t *dst_u, size_t u_pitch, uint8_t *dst_v, size_t v_pitch,
                                  const uint8_t *src, size_t src_pitch, size_t bytes,
                                  unsigned rows) __attribute__((weak));
extern void vlc_ps5_vdec_failed(void) __attribute__((weak));
extern void vlc_ps5_vdec_active(int on) __attribute__((weak));

#define PROT_CPU_RW 0x03
#define PROT_GPU_RW 0x30
#define DIRECT_TYPE 12            /* the type the decoder's memory is known to work with */
#define PAGE_16K ((size_t)0x4000)

#define VLOG(...) \
    do { \
        fprintf(stderr, "ps5vdec: [%.3f] ", mdate() / 1e6); \
        fprintf(stderr, __VA_ARGS__); \
        fflush(stderr); \
    } while (0)

/* ---- memory --------------------------------------------------------------- */

typedef struct {
    void *addr;
    size_t len;
    int64_t start;    /* direct memory; -1: flexible; -2: the decoders' CPU block */
} Mem;

static size_t round_16k(size_t v)
{
    return (v + PAGE_16K - 1) & ~(PAGE_16K - 1);
}

static bool mem_direct(Mem *m, size_t len, size_t align, int prot)
{
    m->len = round_16k(len);
    m->start = -1;
    m->addr = NULL;
    if (align < PAGE_16K)
        align = PAGE_16K;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m->len, align,
                                      DIRECT_TYPE, &m->start) != 0)
        return false;
    if (sceKernelMapDirectMemory(&m->addr, m->len, prot, 0, m->start, align) != 0) {
        sceKernelReleaseDirectMemory(m->start, m->len);
        m->start = -1;
        m->addr = NULL;
        return false;
    }
    return true;
}

static bool mem_flexible(Mem *m, size_t len)
{
    m->len = round_16k(len);
    m->start = -1;
    m->addr = NULL;
    return sceKernelMapFlexibleMemory(&m->addr, m->len, PROT_CPU_RW, 0) == 0;
}

/* The decoders' CPU memory: one block, made once and handed to one decoder
 * after another. Flexible memory given back with
 * sceKernelReleaseFlexibleMemory didn't come back (a 2026-10-13 log: 75
 * decoder attempts later the app had 0 MiB left and 4K went black; an older
 * session lost 600 MiB over eight 4K films), so this one is never given
 * back: it's reused. */
static vlc_mutex_t cpu_block_lock = VLC_STATIC_MUTEX;
static void *cpu_block;
static size_t cpu_block_len;
static bool cpu_block_busy;

static bool mem_cpu(Mem *m, size_t len)
{
    len = round_16k(len);
    vlc_mutex_lock(&cpu_block_lock);
    bool ok = false;
    if (!cpu_block_busy) {
        if (len > cpu_block_len) {
            /* bigger than before: a new block (the old one stays lost, once;
             * sized with room so this is rare: 96 MiB covers 4K HEVC's 76) */
            size_t want = len > ((size_t)96 << 20) ? len : ((size_t)96 << 20);
            void *addr = NULL;
            if (sceKernelMapFlexibleMemory(&addr, want, PROT_CPU_RW, 0) == 0) {
                if (cpu_block)
                    VLOG("CPU block grown %zu -> %zu KiB\n", cpu_block_len >> 10, want >> 10);
                cpu_block = addr;
                cpu_block_len = want;
            }
        }
        if (cpu_block && len <= cpu_block_len) {
            cpu_block_busy = true;
            m->addr = cpu_block;
            m->len = len;
            m->start = -2;
            ok = true;
        }
    }
    vlc_mutex_unlock(&cpu_block_lock);
    /* a second decoder at the same time (rare): its own, as before */
    return ok || mem_flexible(m, len);
}

static void mem_free(Mem *m)
{
    if (!m->addr)
        return;
    if (m->start == -2) {
        vlc_mutex_lock(&cpu_block_lock);
        cpu_block_busy = false;
        vlc_mutex_unlock(&cpu_block_lock);
    } else if (m->start >= 0) {
        sceKernelMunmap(m->addr, m->len);
        sceKernelReleaseDirectMemory(m->start, m->len);
    } else {
        sceKernelMunmap(m->addr, m->len); /* (released, flexible memory never came back) */
    }
    m->addr = NULL;
}

/* One compute queue for the app's life: the decoders share it. */
static vlc_mutex_t queue_lock = VLC_STATIC_MUTEX;
static void *compute_queue;
static Mem queue_mem;

static void *get_compute_queue(void)
{
    vlc_mutex_lock(&queue_lock);
    if (!compute_queue) {
        VdecComputeMemory info = { .this_size = sizeof(info) };
        int rc = sceVideodec2QueryComputeMemoryInfo(&info);
        if (rc == 0 && mem_direct(&queue_mem, info.cpu_gpu_memory_size, PAGE_16K,
                                  PROT_CPU_RW | PROT_GPU_RW)) {
            info.cpu_gpu_memory = queue_mem.addr;
            info.cpu_gpu_memory_size = queue_mem.len;
            VdecComputeConfig config = { .this_size = sizeof(config) };
            rc = sceVideodec2AllocateComputeQueue(&config, &info, &compute_queue);
            if (rc != 0) {
                compute_queue = NULL;
                mem_free(&queue_mem);
            }
        }
        VLOG("compute queue: %s (%#x, %" PRIu64 " KiB)\n", compute_queue ? "ready" : "FAILED",
             (unsigned)rc, (uint64_t)(info.cpu_gpu_memory_size >> 10));
    }
    void *queue = compute_queue;
    vlc_mutex_unlock(&queue_lock);
    return queue;
}

/* At start, before full access (platform_ps5.c): the compute queue made
 * after it failed with FATAL_STATE (0x811D0111, console run 12); decoders
 * made after it, on this queue, are fine. */
/* VP9's own compute queue: once an H.264 or HEVC decoder had run on the
 * shared one, every VP9 decoder made on it was refused (0x811D0100, the
 * first thing after a HEVC film in two logs of 2026-10-13), while VP9 made
 * first worked. Made at start too (a queue made later fails). */
static void *vp9_queue;
static Mem vp9_queue_mem;

static void *make_vp9_queue(void)
{
    VdecComputeMemory info = { .this_size = sizeof(info) };
    int rc = sceVideodec2QueryComputeMemoryInfo(&info);
    if (rc == 0 && mem_direct(&vp9_queue_mem, info.cpu_gpu_memory_size, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) {
        info.cpu_gpu_memory = vp9_queue_mem.addr;
        info.cpu_gpu_memory_size = vp9_queue_mem.len;
        VdecComputeConfig config = { .this_size = sizeof(config) };
        rc = sceVideodec2AllocateComputeQueue(&config, &info, &vp9_queue);
        if (rc != 0) {
            vp9_queue = NULL;
            mem_free(&vp9_queue_mem);
        }
    }
    VLOG("VP9 compute queue: %s (%#x)\n", vp9_queue ? "ready" : "FAILED (VP9 shares the other)", (unsigned)rc);
    return vp9_queue;
}

void vlc_ps5_vdec_prepare(void);
void vlc_ps5_vdec_prepare(void)
{
    get_compute_queue();
    vlc_mutex_lock(&queue_lock);
    if (compute_queue && !vp9_queue)
        make_vp9_queue();
    vlc_mutex_unlock(&queue_lock);
}

/* The decoder's memory answers for the usual configs, asked before full
 * access: after it, every H.264 and HEVC memory query came back 0x811D0200
 * (two console logs of 2026-10-13, the app with full access) while VP9's
 * worked, and the sandboxed tester's H.264 decoded fine. bring_up uses an
 * answer from here when the query is refused. */
typedef struct {
    uint32_t codec, profile, level, depth;
    int32_t w, h;
    VdecMemory mem;
    bool ok;
} CachedQuery;
static CachedQuery cached[12];
static unsigned cached_count;

static void config_for(VdecConfig *c, uint32_t codec, uint32_t profile, uint32_t level, uint32_t depth,
                       int32_t w, int32_t h)
{
    *c = (VdecConfig){ .this_size = sizeof(*c), .resource_type = 1, .codec_type = codec, .profile = profile,
                       .max_level = level, .max_frame_width = w, .max_frame_height = h,
                       .max_dpb_frame_count = -1, .decode_pipeline_depth = depth,
                       .compute_queue = compute_queue, .cpu_affinity_mask = 0x3f,
                       .cpu_thread_priority = 700, .optimize_progressive_video = true };
}

/* when: "before full access" (fills the cache) or "after full access" (logs only). */
void vlc_ps5_vdec_probe(const char *when);
void vlc_ps5_vdec_probe(const char *when)
{
    static const struct { uint32_t codec, profile, level, depth; int32_t w, h; } tries[] = {
        { VDEC_CODEC_AVC, 100, 52, 4, 1920, 1088 }, { VDEC_CODEC_AVC, 100, 52, 1, 1920, 1088 },
        { VDEC_CODEC_AVC, 100, 52, 4, 3840, 2176 }, { VDEC_CODEC_AVC, 100, 52, 1, 3840, 2176 },
        { VDEC_CODEC_HEVC, 2, 156, 4, 1920, 1088 }, { VDEC_CODEC_HEVC, 2, 156, 1, 1920, 1088 },
        { VDEC_CODEC_HEVC, 2, 156, 4, 3840, 2160 }, { VDEC_CODEC_HEVC, 2, 156, 1, 3840, 2160 },
        { VDEC_CODEC_HEVC, 2, 153, 4, 3840, 2160 }, { VDEC_CODEC_HEVC, 2, 153, 1, 3840, 2160 },
        { VDEC_CODEC_VP9, 0, 62, 4, 3840, 2160 }, { VDEC_CODEC_VP9, 2, 62, 1, 3840, 2160 },
    };
    if (!compute_queue)
        return;
    bool fill = strstr(when, "before") != NULL;
    if (fill) {
        /* does flexible memory given back come back? (logged: the rest of
         * the app frees its big blocks the same way) */
        size_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        const size_t test = (size_t)32 << 20;
        void *x = NULL;
        sceKernelAvailableFlexibleMemorySize(&a0);
        int r1 = sceKernelMapFlexibleMemory(&x, test, PROT_CPU_RW, 0);
        if (r1 == 0)
            memset(x, 1, test);
        sceKernelAvailableFlexibleMemorySize(&a1);
        int r2 = r1 == 0 ? sceKernelReleaseFlexibleMemory(x, test) : -1;
        sceKernelAvailableFlexibleMemorySize(&a2);
        int r3 = r1 == 0 ? sceKernelMunmap(x, test) : -1;
        sceKernelAvailableFlexibleMemorySize(&a3);
        void *y = NULL;
        int r4 = sceKernelMapFlexibleMemory(&y, test, PROT_CPU_RW, 0);
        if (r4 == 0) {
            memset(y, 1, test);
            sceKernelMunmap(y, test);
        }
        sceKernelAvailableFlexibleMemorySize(&a4);
        VLOG("flexible memory test (MiB left): start %zu, 32 taken %zu (%#x), released %zu (%#x), "
             "unmapped %zu (%#x), again + unmap only %zu (%#x)\n", a0 >> 20, a1 >> 20, (unsigned)r1, a2 >> 20,
             (unsigned)r2, a3 >> 20, (unsigned)r3, a4 >> 20, (unsigned)r4);
    }
    char line[512];
    size_t at = 0;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        VdecConfig c;
        config_for(&c, tries[i].codec, tries[i].profile, tries[i].level, tries[i].depth, tries[i].w, tries[i].h);
        VdecMemory m = { .this_size = sizeof(m) };
        int rc = sceVideodec2QueryDecoderMemoryInfo(&c, &m);
        at += snprintf(line + at, sizeof(line) - at, " %s%ux%d/%u:%#x",
                       tries[i].codec == VDEC_CODEC_AVC ? "avc" : tries[i].codec == VDEC_CODEC_HEVC ? "hevc" : "vp9",
                       (unsigned)tries[i].h, (int)tries[i].level, (unsigned)tries[i].depth, (unsigned)rc);
        if (at >= sizeof(line))
            at = sizeof(line) - 1;
        if (fill && cached_count < sizeof(cached) / sizeof(cached[0]))
            cached[cached_count++] = (CachedQuery){ tries[i].codec, tries[i].profile, tries[i].level,
                                                    tries[i].depth, tries[i].w, tries[i].h, m, rc == 0 };
    }
    VLOG("memory queries %s:%s\n", when, line);
}

/* A query answer from before full access for this config (or one with room
 * for a bigger picture): the config takes its size then. */
static bool cached_answer(VdecConfig *c, VdecMemory *mem)
{
    const CachedQuery *best = NULL;
    for (unsigned i = 0; i < cached_count; i++) {
        const CachedQuery *q = &cached[i];
        if (q->ok && q->codec == c->codec_type && q->profile == c->profile && q->level == c->max_level &&
            q->depth == c->decode_pipeline_depth && q->w >= c->max_frame_width && q->h >= c->max_frame_height &&
            (!best || (int64_t)q->w * q->h < (int64_t)best->w * best->h))
            best = q;
    }
    if (!best)
        return false;
    c->max_frame_width = best->w;
    c->max_frame_height = best->h;
    *mem = best->mem;
    return true;
}

/* ---- the decoder ---------------------------------------------------------- */

/* Access units in turn: the decoder may still be reading one after Decode
 * returns (a big picture), so a slot comes round again only after several
 * others. Three, as first tried, broke a 4K high-tier film at its first big
 * picture (744 KB): INVALID_SEQUENCE from then on (console runs 2-4). */
#define AU_SLOTS 8
/* Output pictures: the decoder keeps the one it was handed until that
 * picture's turn to be shown (B-frames come out late), so a buffer is only
 * handed out again once it came back. Three in turn, as first tried, were
 * written over while still waiting: green blocks, then refusals. */
#define MAX_FRAMES 16
#define AU_SLOT_SIZE ((size_t)4 << 20)
/* stream errors (0x811D03xx: new/invalid sequence, bad access unit) */
#define VDEC_ERROR_STREAM_FIRST 0x811D0300u
#define VDEC_ERROR_STREAM_LAST 0x811D0304u
#define MAX_RESYNCS 5
#define MAX_PENDING 64

typedef struct {
    mtime_t pts;
    bool interlaced;
    bool top_first;
    bool hidden;    /* VP9: a frame never shown (a reference only): its picture is dropped */
} Pending;

struct decoder_sys_t {
    void *dec;
    uint32_t codec;
    Mem cpu, gpu, cpu_gpu;
    Mem au[AU_SLOTS];
    Mem frames;                /* frame_count pictures, frame_size apart */
    size_t frame_size;
    unsigned frame_count;
    bool frame_held[MAX_FRAMES];   /* with the decoder, not back yet */
    unsigned au_next;
    uint64_t copy_us;              /* time copying pictures out, for the log */
    uint64_t decode_us, decode_max_us;   /* time in the decoder's calls */
    unsigned decode_calls;

    uint8_t *headers;          /* parameter sets, Annex B */
    size_t headers_size;
    bool send_headers;         /* in front of the next access unit */
    uint8_t nal_length;        /* avcC/hvcC input: length bytes (0: Annex B) */

    /* pictures come out in display order: each takes the earliest time
     * still waiting (with how its access unit was coded) */
    Pending pending[MAX_PENDING];
    unsigned pending_count;

    /* H.264 interlaced TV (1080i): from the sequence header, and a field
     * picture waiting for its other half (the decoder gives one frame for
     * the two, so the pair has one time: issue #11) */
    bool sps_ok;
    bool frame_mbs_only;
    bool separate_colour_plane;
    unsigned log2_max_frame_num;
    bool field_open;           /* a first field went in, its partner not yet */
    bool field_open_bottom;
    unsigned field_open_frame_num;
    unsigned fields_paired;

    /* VP9: the config the decoder was made with (made again once the first
     * frame says 8-bit or 10-bit), and whether that was checked */
    VdecConfig cfg;
    bool vp9_checked;
    bool chip_refused;         /* CreateDecoder said 0x811D01xx: no more configs */

    unsigned decoded, errors, shown;
    unsigned diag_in, diag_out;   /* the first units in and pictures out, in the log */
    unsigned hidden_dropped;
    bool failed;
    bool wait_keyframe;        /* after a stream error: from the next keyframe on */
    bool skip_rasl;            /* HEVC: after a restart point, its RASL pictures */
    mtime_t key_pts;           /* the restart keyframe's time: pictures before it are left out */
    unsigned leading;
    unsigned resyncs, skipped;
    bool format_set;
    uint32_t out_w, out_h, out_pitch_bytes;
    bool out_ten;
    bool out_planar8;
};

static void pending_add(decoder_sys_t *sys, Pending p)
{
    if (sys->pending_count == MAX_PENDING) {
        memmove(sys->pending, sys->pending + 1, (MAX_PENDING - 1) * sizeof(Pending));
        sys->pending_count--;
    }
    sys->pending[sys->pending_count++] = p;
}

/* VP9: the oldest one. VP9 pictures come out in the order they went in,
 * hidden reference frames included: the chip gives a picture for those too,
 * and taken by the smallest time the hidden frame's picture got the next
 * shown frame's time (shown out of place), and two pictures a second were
 * left with no time at all and thrown away by VLC (planet_earth: 43 hidden
 * frames in 480, each in a block of its own; 2026-10-18). */
static Pending pending_take_fifo(decoder_sys_t *sys)
{
    Pending none = { VLC_TS_INVALID, false, true, false };
    if (!sys->pending_count)
        return none;
    Pending p = sys->pending[0];
    memmove(sys->pending, sys->pending + 1, (sys->pending_count - 1) * sizeof(Pending));
    sys->pending_count--;
    return p;
}

static Pending pending_take(decoder_sys_t *sys)
{
    if (sys->codec == VDEC_CODEC_VP9)
        return pending_take_fifo(sys);
    Pending none = { VLC_TS_INVALID, false, true };
    if (!sys->pending_count)
        return none;
    unsigned best = 0;
    for (unsigned i = 1; i < sys->pending_count; i++)
        if (sys->pending[i].pts < sys->pending[best].pts)
            best = i;
    Pending p = sys->pending[best];
    sys->pending[best] = sys->pending[--sys->pending_count];
    return p;
}

/* HEVC NAL units the decoder can't take: Dolby Vision's own (types 62 and
 * 63: its picture data and the enhancement layer of profile 7) and any other
 * layer than the base one. Left out, a Dolby Vision film plays as the HDR10
 * its base layer is (it refused profile 7 at the first enhancement unit). */
static bool hevc_skip(const uint8_t *nal, size_t n)
{
    if (n < 2)
        return true;
    unsigned type = (nal[0] >> 1) & 0x3f;
    unsigned layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
    return type == 62 || type == 63 || layer != 0;
}

/* Length-prefixed NAL units -> Annex B, into dst (room for 4 + size per
 * unit is enough: lengths of 1 to 4 bytes become 4-byte start codes). */
static size_t to_annexb(uint8_t *dst, size_t room, const uint8_t *src, size_t size, uint8_t len_size,
                        bool hevc)
{
    size_t out = 0;
    while (size >= len_size) {
        uint32_t n = 0;
        for (unsigned i = 0; i < len_size; i++)
            n = (n << 8) | src[i];
        src += len_size;
        size -= len_size;
        if (n > size || out + 4 + n > room)
            break;
        if (hevc && hevc_skip(src, n)) {
            src += n;
            size -= n;
            continue;
        }
        memcpy(dst + out, "\x00\x00\x00\x01", 4);
        memcpy(dst + out + 4, src, n);
        out += 4 + n;
        src += n;
        size -= n;
    }
    return out;
}

/* The parameter sets from the extra data: Annex B as it is, or out of an
 * avcC / hvcC record (then the blocks are length-prefixed too). */
static void read_headers(decoder_t *dec, decoder_sys_t *sys)
{
    const uint8_t *p = dec->fmt_in.p_extra;
    size_t n = dec->fmt_in.i_extra;
    if (!p || !n)
        return;
    if (p[0] != 1) {   /* Annex B */
        sys->headers = malloc(n);
        if (sys->headers) {
            memcpy(sys->headers, p, n);
            sys->headers_size = n;
        }
        return;
    }
    size_t room = n * 2 + 64;
    sys->headers = malloc(room);
    if (!sys->headers)
        return;
    size_t out = 0;
    if (sys->codec == VDEC_CODEC_AVC && n >= 7) {
        /* avcC: lengthSizeMinusOne, then SPS and PPS lists */
        sys->nal_length = (p[4] & 3) + 1;
        size_t at = 5;
        for (int list = 0; list < 2 && at < n; list++) {
            unsigned count = list == 0 ? (p[at] & 0x1f) : p[at];
            at++;
            for (unsigned i = 0; i < count && at + 2 <= n; i++) {
                size_t len = (p[at] << 8) | p[at + 1];
                at += 2;
                if (at + len > n || out + 4 + len > room)
                    break;
                memcpy(sys->headers + out, "\x00\x00\x00\x01", 4);
                memcpy(sys->headers + out + 4, p + at, len);
                out += 4 + len;
                at += len;
            }
        }
    } else if (sys->codec == VDEC_CODEC_HEVC && n >= 23) {
        /* hvcC: lengthSizeMinusOne at byte 21, arrays from byte 22 */
        sys->nal_length = (p[21] & 3) + 1;
        unsigned arrays = p[22];
        size_t at = 23;
        for (unsigned a = 0; a < arrays && at + 3 <= n; a++) {
            unsigned count = (p[at + 1] << 8) | p[at + 2];
            at += 3;
            for (unsigned i = 0; i < count && at + 2 <= n; i++) {
                size_t len = (p[at] << 8) | p[at + 1];
                at += 2;
                if (at + len > n || out + 4 + len > room)
                    break;
                memcpy(sys->headers + out, "\x00\x00\x00\x01", 4);
                memcpy(sys->headers + out + 4, p + at, len);
                out += 4 + len;
                at += len;
            }
        }
    }
    sys->headers_size = out;
}

static void give_up(decoder_t *dec, const char *why)
{
    decoder_sys_t *sys = dec->p_sys;
    if (sys->failed)
        return;
    sys->failed = true;
    VLOG("giving the video to FFmpeg: %s (%u decoded, %u errors)\n", why, sys->decoded, sys->errors);
    if (vlc_ps5_vdec_failed)
        vlc_ps5_vdec_failed();
}

/* A picture the decoder gave back: its buffer can be handed out again. */
static void frame_back(decoder_sys_t *sys, const void *buffer)
{
    const uint8_t *base = sys->frames.addr, *b = buffer;
    if (!b || !base || b < base || !sys->frame_size)
        return;
    size_t i = (size_t)(b - base) / sys->frame_size;
    if (i < sys->frame_count)
        sys->frame_held[i] = false;
}

/* shift: 16-bit samples moved up this many bits. The decoder's 10-bit
 * samples sit at the bottom of 16 bits (console run 3: read as P010 they made
 * a green picture); I0AL, what we give VLC now, keeps them there (shift 0). */
static void copy_plane(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch,
                       size_t bytes, unsigned rows, unsigned shift)
{
    if (vlc_ps5_copy_plane_shift) {
        vlc_ps5_copy_plane_shift(dst, dst_pitch, src, src_pitch, bytes, rows, shift);
        return;
    }
    for (unsigned r = 0; r < rows; r++) {
        if (!shift) {
            memcpy(dst + (size_t)r * dst_pitch, src + (size_t)r * src_pitch, bytes);
            continue;
        }
        uint16_t *d = (uint16_t *)(dst + (size_t)r * dst_pitch);
        const uint16_t *s = (const uint16_t *)(src + (size_t)r * src_pitch);
        for (size_t i = 0; i < bytes / 2; i++)
            d[i] = (uint16_t)(s[i] << shift);
    }
}

/* Interleaved 16-bit U,V rows into a U plane and a V plane. */
static void split_plane16(uint8_t *dst_u, size_t u_pitch, uint8_t *dst_v, size_t v_pitch,
                          const uint8_t *src, size_t src_pitch, size_t bytes, unsigned rows)
{
    if (vlc_ps5_split_plane16) {
        vlc_ps5_split_plane16(dst_u, u_pitch, dst_v, v_pitch, src, src_pitch, bytes, rows);
        return;
    }
    for (unsigned r = 0; r < rows; r++) {
        uint16_t *u = (uint16_t *)(dst_u + (size_t)r * u_pitch);
        uint16_t *v = (uint16_t *)(dst_v + (size_t)r * v_pitch);
        const uint16_t *s = (const uint16_t *)(src + (size_t)r * src_pitch);
        for (size_t i = 0; i < bytes / 4; i++) {
            u[i] = s[2 * i];
            v[i] = s[2 * i + 1];
        }
    }
}

/* Exp-Golomb ue(v) from a NAL unit's bytes (emulation prevention ignored:
 * only the first few bits of a slice header are read). */
static unsigned read_ue(const uint8_t *p, size_t n, size_t *bit)
{
    unsigned zeros = 0;
    while (*bit < n * 8 && !((p[*bit / 8] >> (7 - *bit % 8)) & 1) && zeros < 31) {
        zeros++;
        (*bit)++;
    }
    (*bit)++;   /* the 1 */
    unsigned v = 0;
    for (unsigned i = 0; i < zeros && *bit < n * 8; i++, (*bit)++)
        v = (v << 1) | ((p[*bit / 8] >> (7 - *bit % 8)) & 1);
    return (1u << zeros) - 1 + v;
}

/* ---- VP9: frames, superframes and the start of the frame header -------- */

/* A block's frames: one, or a superframe's. A superframe ends with an index:
 * a marker byte 0b110sssnn (n+1 frames, sizes of s+1 bytes, little-endian),
 * the sizes, and the marker again. 0: a broken index. */
static unsigned vp9_frames(const uint8_t *p, size_t n, size_t off[8], size_t len[8])
{
    if (!n)
        return 0;
    uint8_t m = p[n - 1];
    if ((m & 0xe0) == 0xc0) {
        unsigned frames = (m & 7) + 1, bytes = ((m >> 3) & 3) + 1;
        size_t index = 2 + (size_t)bytes * frames;
        if (n >= index && p[n - index] == m) {
            size_t at = 0;
            for (unsigned i = 0; i < frames; i++) {
                size_t size = 0;
                for (unsigned b = 0; b < bytes; b++)
                    size |= (size_t)p[n - index + 1 + i * bytes + b] << (8 * b);
                if (!size || at + size > n - index)
                    return 0;
                off[i] = at;
                len[i] = size;
                at += size;
            }
            return frames;
        }
    }
    off[0] = 0;
    len[0] = n;
    return 1;
}

static unsigned vp9_bit(const uint8_t *p, size_t n, unsigned *bit)
{
    unsigned v = *bit < n * 8 ? (p[*bit / 8] >> (7 - *bit % 8)) & 1 : 0;
    (*bit)++;
    return v;
}

/* A frame: a key frame? Shown (or an earlier frame shown again)? */
static void vp9_header(const uint8_t *p, size_t n, bool *key, bool *shows)
{
    unsigned bit = 0;
    *key = *shows = false;
    if (n < 1 || (p[0] >> 6) != 2)     /* frame_marker */
        return;
    bit = 2;
    unsigned profile = vp9_bit(p, n, &bit);
    profile |= vp9_bit(p, n, &bit) << 1;
    if (profile == 3)
        bit++;                         /* reserved_zero */
    if (vp9_bit(p, n, &bit)) {         /* show_existing_frame */
        *shows = true;
        return;
    }
    *key = vp9_bit(p, n, &bit) == 0;   /* frame_type: 0 is KEY_FRAME */
    *shows = vp9_bit(p, n, &bit) != 0; /* show_frame */
}

/* A VP9 frame's profile (0-3) from its first byte; 4: not a frame. */
static unsigned vp9_profile(const uint8_t *p, size_t n)
{
    if (n < 1 || (p[0] >> 6) != 2)
        return 4;
    return ((p[0] >> 5) & 1) | (((p[0] >> 4) & 1) << 1);
}

static bool vp9_rebuild(decoder_t *dec, unsigned profile);

/* An access unit a decoder can start from (Annex B): H.264 an IDR picture or
 * a picture of I slices, HEVC a random access point (IRAP, types 16-21),
 * VP9 a block whose first frame is a key frame. */
static bool is_keyframe(const uint8_t *p, size_t n, uint32_t codec)
{
    if (codec == VDEC_CODEC_VP9) {
        size_t off[8], len[8];
        bool key = false, shows;
        if (vp9_frames(p, n, off, len))
            vp9_header(p + off[0], len[0], &key, &shows);
        return key;
    }
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        const uint8_t *nal = p + i + 3;
        size_t left = n - i - 3;
        if (codec == VDEC_CODEC_HEVC) {
            unsigned type = (nal[0] >> 1) & 0x3f;
            if (type < 32)
                return type >= 16 && type <= 21;
        } else {
            unsigned type = nal[0] & 0x1f;
            if (type == 5)
                return true;
            if (type == 1 && left > 1) {
                size_t bit = 0;
                read_ue(nal + 1, left - 1, &bit);                       /* first_mb_in_slice */
                unsigned slice_type = read_ue(nal + 1, left - 1, &bit);
                return slice_type % 5 == 2;                              /* I */
            }
        }
        i += 2;
    }
    return false;
}

/* HEVC: the type of an access unit's first picture slice (-1: none). */
static int hevc_first_slice_type(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            unsigned type = (p[i + 3] >> 1) & 0x3f;
            if (type < 32)
                return (int)type;
            i += 2;
        }
    }
    return -1;
}

/* ---- H.264 interlacing: sequence header and slice header bits ------------- */

typedef struct {
    uint8_t b[256];   /* the NAL unit's payload, emulation prevention taken out */
    size_t n, bit;
} Bits;

static void bits_init(Bits *r, const uint8_t *p, size_t n)
{
    r->n = r->bit = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < n && r->n < sizeof(r->b); i++) {
        if (zeros >= 2 && p[i] == 3) {   /* 00 00 03: the 03 isn't data */
            zeros = 0;
            continue;
        }
        if (zeros >= 2 && p[i] <= 2)     /* the next start code */
            break;
        zeros = p[i] == 0 ? zeros + 1 : 0;
        r->b[r->n++] = p[i];
    }
}

static unsigned bits_u(Bits *r, unsigned count)
{
    unsigned v = 0;
    for (unsigned i = 0; i < count; i++, r->bit++)
        v = (v << 1) | (r->bit < r->n * 8 ? (r->b[r->bit / 8] >> (7 - r->bit % 8)) & 1 : 0);
    return v;
}

static unsigned bits_ue(Bits *r)
{
    unsigned zeros = 0;
    while (r->bit < r->n * 8 && !bits_u(r, 1) && zeros < 31)
        zeros++;
    return (1u << zeros) - 1 + bits_u(r, zeros);
}

static int bits_se(Bits *r)
{
    unsigned v = bits_ue(r);
    return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2);
}

/* A sequence parameter set (after the NAL header byte): down to
 * frame_mbs_only_flag, which says whether pictures can be interlaced. */
static void h264_read_sps(decoder_sys_t *sys, const uint8_t *p, size_t n)
{
    Bits r;
    bits_init(&r, p, n);
    unsigned profile = bits_u(&r, 8);
    bits_u(&r, 16);                       /* constraint flags, level */
    bits_ue(&r);                          /* seq_parameter_set_id */
    bool separate = false;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        unsigned chroma = bits_ue(&r);
        if (chroma == 3)
            separate = bits_u(&r, 1);
        bits_ue(&r);                      /* bit depths */
        bits_ue(&r);
        bits_u(&r, 1);                    /* qpprime_y_zero_transform_bypass */
        if (bits_u(&r, 1)) {              /* scaling matrices */
            for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
                if (!bits_u(&r, 1))
                    continue;
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); j++) {
                    if (next)
                        next = (last + bits_se(&r) + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    unsigned log2_frame_num = bits_ue(&r) + 4;
    unsigned poc_type = bits_ue(&r);
    if (poc_type == 0) {
        bits_ue(&r);
    } else if (poc_type == 1) {
        bits_u(&r, 1);
        bits_se(&r);
        bits_se(&r);
        unsigned cycle = bits_ue(&r);
        for (unsigned i = 0; i < cycle && i < 256; i++)
            bits_se(&r);
    }
    bits_ue(&r);                          /* max_num_ref_frames */
    bits_u(&r, 1);                        /* gaps_in_frame_num_allowed */
    bits_ue(&r);                          /* width, height in macroblocks */
    bits_ue(&r);
    if (r.bit >= r.n * 8 || log2_frame_num > 16)
        return;                           /* cut short: keep what we had */
    bool mbs_only = bits_u(&r, 1);
    if (!sys->sps_ok || mbs_only != sys->frame_mbs_only)
        VLOG("H.264 %s\n", mbs_only ? "progressive" : "interlaced (fields or MBAFF): deinterlaced after decoding");
    sys->frame_mbs_only = mbs_only;
    sys->separate_colour_plane = separate;
    sys->log2_max_frame_num = log2_frame_num;
    sys->sps_ok = true;
}

/* Sequence headers in an Annex B buffer (the extra data or an access unit). */
static void h264_scan_sps(decoder_sys_t *sys, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        unsigned type = p[i + 3] & 0x1f;
        if (type == 7)
            h264_read_sps(sys, p + i + 4, n - i - 4);
        else if (type == 1 || type == 5)
            return;   /* the picture: parameter sets come before it */
        i += 2;
    }
}

/* How an access unit is coded: a frame, or one field (top or bottom), with
 * its frame_num to pair it with the other field. false: no slice found. */
static bool h264_slice_field(const decoder_sys_t *sys, const uint8_t *p, size_t n, bool *field,
                             bool *bottom, unsigned *frame_num)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        unsigned type = p[i + 3] & 0x1f;
        if (type == 1 || type == 5) {
            Bits r;
            bits_init(&r, p + i + 4, n - i - 4 < 64 ? n - i - 4 : 64);
            bits_ue(&r);                          /* first_mb_in_slice */
            bits_ue(&r);                          /* slice_type */
            bits_ue(&r);                          /* pic_parameter_set_id */
            if (sys->separate_colour_plane)
                bits_u(&r, 2);
            *frame_num = bits_u(&r, sys->log2_max_frame_num);
            *field = !sys->frame_mbs_only && bits_u(&r, 1);
            *bottom = *field && bits_u(&r, 1);
            return true;
        }
        i += 2;
    }
    return false;
}

/* NV12's interleaved U,V rows into a U plane and a V plane (I420). */
static void split_plane8(uint8_t *dst_u, size_t u_pitch, uint8_t *dst_v, size_t v_pitch,
                         const uint8_t *src, size_t src_pitch, size_t width, unsigned rows)
{
    for (unsigned r = 0; r < rows; r++) {
        uint8_t *u = dst_u + (size_t)r * u_pitch, *v = dst_v + (size_t)r * v_pitch;
        const uint8_t *s = src + (size_t)r * src_pitch;
        for (size_t i = 0; i < width; i++) {
            u[i] = s[2 * i];
            v[i] = s[2 * i + 1];
        }
    }
}

/* One decoded picture into a VLC picture. */
/* Average brightness (0-255) of a decoder picture: 16x16 points of its luma. */
extern int vlc_ps5_log_details(void) __attribute__((weak));

static unsigned frame_luma(const VdecOutput *out)
{
    const uint8_t *p = out->frame_buffer;
    unsigned w = out->frame_width, h = out->frame_height;
    if (!p || !w || !h || !out->frame_pitch_in_bytes)
        return 0;
    bool ten = out->frame_pitch_in_bytes >= 2 * out->frame_pitch;
    uint64_t sum = 0;
    for (unsigned y = 0; y < 16; y++) {
        const uint8_t *row = p + (size_t)(h * y / 16 + h / 32) * out->frame_pitch_in_bytes;
        for (unsigned x = 0; x < 16; x++) {
            unsigned px = w * x / 16 + w / 32;
            sum += ten ? ((const uint16_t *)row)[px] >> 2 : row[px];
        }
    }
    return (unsigned)(sum / 256);
}

static void output(decoder_t *dec, const VdecOutput *out)
{
    decoder_sys_t *sys = dec->p_sys;
    unsigned pending_before = sys->pending_count;
    Pending when = pending_take(sys);
    if (sys->diag_out < 12) {
        /* the picture as the chip gives it: its buffer, its own time (what
         * we sent with its unit) against the one we give it, its brightness */
        sys->diag_out++;
        const uint8_t *base = sys->frames.addr, *b = out->frame_buffer;
        long idx = base && b && sys->frame_size ? (long)((b - base) / (ptrdiff_t)sys->frame_size) : -1;
        long long chip = -1;
        int prc = -1;
        if (sys->codec != VDEC_CODEC_VP9) {
            uint64_t info[32] = { 0 }, info2[32] = { 0 };
            info[0] = info2[0] = sys->codec == VDEC_CODEC_AVC ? 0x78 : 0xB8;
            prc = sceVideodec2GetPictureInfo(out, info, info2);
            const VdecPictureHead *h = (const VdecPictureHead *)info;
            if (prc == 0 && h->is_valid)
                chip = (long long)h->pts_data;
        }
        VLOG("diag out %u: buffer %ld, our time %lld, chip's %lld (info %#x), %u waiting, %s, luma %u\n",
             sys->diag_out, idx, (long long)when.pts, chip, (unsigned)prc, pending_before,
             out->is_error_frame ? "ERROR FRAME" : "ok", frame_luma(out));
    }
    if (out->is_error_frame || !out->frame_buffer) {
        sys->errors++;
        frame_back(sys, out->frame_buffer);
        return;
    }
    if (when.hidden) {
        if (sys->hidden_dropped++ == 0)
            VLOG("VP9: pictures of hidden reference frames left out\n");
        frame_back(sys, out->frame_buffer);
        return;
    }
    /* A picture shown before the keyframe the decoding started from (an
     * open GOP's leading pictures): it refers to pictures the decoder never
     * got, so it's made of whatever is in memory. FFmpeg leaves those out
     * too. */
    if (when.pts != VLC_TS_INVALID && sys->key_pts != VLC_TS_INVALID && when.pts < sys->key_pts) {
        if (sys->leading++ == 0)
            VLOG("pictures from before the first keyframe left out\n");
        frame_back(sys, out->frame_buffer);
        return;
    }
    bool ten = out->frame_pitch_in_bytes >= 2 * out->frame_pitch;
    /* interlaced TV as planar I420: VLC's deinterlacer takes planar pictures
     * only (NV12 went through undeinterlaced: combing, issue #11) */
    bool planar8 = !ten && sys->sps_ok && !sys->frame_mbs_only;
    if (!sys->format_set || out->frame_width != sys->out_w || out->frame_height != sys->out_h ||
        ten != sys->out_ten || planar8 != sys->out_planar8) {
        video_format_t *v = &dec->fmt_out.video;
        /* 10-bit as planar I0AL, not P010: VLC draws subtitles onto I0AL but
         * has no way to blend them onto P010 (console run 7: none showed) */
        dec->fmt_out.i_codec = ten ? VLC_CODEC_I420_10L : planar8 ? VLC_CODEC_I420 : VLC_CODEC_NV12;
        v->i_chroma = dec->fmt_out.i_codec;
        /* Only the picture's real size, not the decoder's padding (1088 rows
         * for 1080): VLC's vmem shows its whole buffer, so a padded size made
         * VLC put a software scaler in between, on the CPU for every
         * picture, and its P010 came out green (console run 2). */
        unsigned vw = dec->fmt_in.video.i_visible_width, vh = dec->fmt_in.video.i_visible_height;
        if (!vw || vw > out->frame_width)
            vw = out->frame_width;
        if (!vh || vh > out->frame_height)
            vh = out->frame_height;
        v->i_width = v->i_visible_width = vw;
        v->i_height = v->i_visible_height = vh;
        v->i_x_offset = v->i_y_offset = 0;
        if (!v->i_sar_num || !v->i_sar_den) {
            v->i_sar_num = 1;
            v->i_sar_den = 1;
        }
        sys->out_w = out->frame_width;
        sys->out_h = out->frame_height;
        sys->out_ten = ten;
        sys->out_planar8 = planar8;
        sys->format_set = true;
        VLOG("pictures %ux%u (shown %ux%u) %s, pitch %u bytes, buffer %" PRIu64 " bytes, format %u\n",
             out->frame_width, out->frame_height, v->i_visible_width, v->i_visible_height,
             ten ? "I0AL" : planar8 ? "I420" : "NV12", out->frame_pitch_in_bytes, out->frame_buffer_size,
             out->frame_format);
    }
    picture_t *pic = decoder_UpdateVideoFormat(dec) ? NULL : decoder_NewPicture(dec);
    if (!pic) {
        frame_back(sys, out->frame_buffer);
        return;
    }
    mtime_t t0 = mdate();
    const uint8_t *src = out->frame_buffer;
    size_t pitch = out->frame_pitch_in_bytes;
    unsigned shown_rows = dec->fmt_out.video.i_visible_height;
    /* The chroma follows the decoder's luma rows (frame_height: 1088 for a
     * padded 1080, 2160 for 4K; reading it 2176 rows down for 4K left a green
     * line at the bottom, console run 4). */
    unsigned luma_rows = shown_rows < (unsigned)pic->p[0].i_lines ? shown_rows : (unsigned)pic->p[0].i_lines;
    size_t luma_bytes = pitch < (size_t)pic->p[0].i_pitch ? pitch : (size_t)pic->p[0].i_pitch;
    copy_plane(pic->p[0].p_pixels, pic->p[0].i_pitch, src, pitch, luma_bytes, luma_rows, 0);
    const uint8_t *chroma = src + pitch * out->frame_height;
    unsigned chroma_rows = (shown_rows + 1) / 2;
    if (chroma_rows > (unsigned)pic->p[1].i_lines)
        chroma_rows = pic->p[1].i_lines;
    if (ten) {
        /* I0AL: the interleaved U,V split into their own planes, 10 bits at
         * the bottom as the decoder gives them */
        size_t bytes = (size_t)pic->p[1].i_pitch * 2;
        if (bytes > pitch)
            bytes = pitch;
        split_plane16(pic->p[1].p_pixels, pic->p[1].i_pitch, pic->p[2].p_pixels, pic->p[2].i_pitch,
                      chroma, pitch, bytes, chroma_rows);
    } else if (planar8) {
        size_t width = (size_t)pic->p[1].i_pitch;
        if (width * 2 > pitch)
            width = pitch / 2;
        split_plane8(pic->p[1].p_pixels, pic->p[1].i_pitch, pic->p[2].p_pixels, pic->p[2].i_pitch,
                     chroma, pitch, width, chroma_rows);
    } else {
        size_t bytes = pitch < (size_t)pic->p[1].i_pitch ? pitch : (size_t)pic->p[1].i_pitch;
        copy_plane(pic->p[1].p_pixels, pic->p[1].i_pitch, chroma, pitch, bytes, chroma_rows, 0);
    }
    sys->copy_us += mdate() - t0;
    frame_back(sys, out->frame_buffer);
    pic->date = when.pts;
    pic->b_progressive = !when.interlaced;
    pic->b_top_field_first = when.top_first;
    pic->i_nb_fields = 2;
    sys->shown++;
    if (sys->shown % 240 == 0)
        VLOG("%u pictures, copying out %.1f ms each\n", sys->shown, sys->copy_us / 240 / 1000.0);
    if (sys->shown % 240 == 0)
        sys->copy_us = 0;
    decoder_QueueVideo(dec, pic);
}

/* A buffer the decoder doesn't hold (-1: all of them out). */
static int next_frame(decoder_sys_t *sys, VdecFrameBuffer *fb)
{
    for (unsigned i = 0; i < sys->frame_count; i++) {
        if (!sys->frame_held[i]) {
            *fb = (VdecFrameBuffer){ .this_size = sizeof(*fb) };
            fb->frame_buffer = (uint8_t *)sys->frames.addr + i * sys->frame_size;
            fb->frame_buffer_size = sys->frame_size;
            return (int)i;
        }
    }
    return -1;
}

/* The pictures still inside: out (decoded) or away (seek). */
static void drain(decoder_t *dec, bool show)
{
    decoder_sys_t *sys = dec->p_sys;
    for (int i = 0; i < 32; i++) {
        VdecFrameBuffer fb;
        int slot = next_frame(sys, &fb);
        if (slot < 0)
            break;
        VdecOutput out = { .this_size = sizeof(out) };
        int rc = sceVideodec2Flush(sys->dec, &fb, &out);
        if (fb.is_accepted)
            sys->frame_held[slot] = true;
        if (rc != 0 || !out.is_valid)
            break;
        if (show) {
            output(dec, &out);
        } else {
            pending_take(sys);
            frame_back(sys, out.frame_buffer);
        }
    }
}

/* One access unit (or one VP9 frame) into the decoder; the picture that
 * comes out, if any, into VLC. when: this unit's time, added before the
 * picture is taken (it may be this one's), or NULL if it shows none. */
static int run_decode(decoder_t *dec, uint8_t *au, size_t size, mtime_t pts, const Pending *when)
{
    decoder_sys_t *sys = dec->p_sys;
    VdecInput in = { .this_size = sizeof(in), .au_data = au, .au_size = size,
                     .pts_data = (uint64_t)pts, .dts_data = UINT64_MAX };
    VdecFrameBuffer fb;
    int frame = next_frame(sys, &fb);
    if (frame < 0) {
        /* every buffer waiting to be shown: shouldn't happen with the count
         * the decoder's picture store needs, but never hand one out twice */
        VLOG("no free picture buffer (%u)\n", sys->frame_count);
        give_up(dec, "out of picture buffers");
        return 0;
    }
    VdecOutput out = { .this_size = sizeof(out) };
    mtime_t t0 = mdate();
    int rc = sceVideodec2Decode(sys->dec, &in, &fb, &out);
    mtime_t took = mdate() - t0;
    sys->decode_us += took;
    if (took > sys->decode_max_us)
        sys->decode_max_us = took;
    if (++sys->decode_calls == 240) {
        VLOG("decode calls: %.1f ms each, slowest %.1f ms (a 24 fps film has 41.7)\n",
             sys->decode_us / 240 / 1000.0, sys->decode_max_us / 1000.0);
        sys->decode_calls = 0;
        sys->decode_us = sys->decode_max_us = 0;
    }
    if (fb.is_accepted)
        sys->frame_held[frame] = true;
    if (sys->diag_in < 12) {
        sys->diag_in++;
        unsigned held = 0;
        for (unsigned i = 0; i < sys->frame_count; i++)
            held += sys->frame_held[i];
        VLOG("diag in %u: %zu bytes, time %lld, keyframe %d, shows %d, buffer %d %s, %u held, result %#x, picture out %d\n",
             sys->diag_in, size, (long long)pts, (int)is_keyframe(au, size, sys->codec), when != NULL, frame,
             fb.is_accepted ? "taken" : "not taken", held, (unsigned)rc, (int)out.is_valid);
    }
    if (rc != 0)
        return rc;
    if (when)
        pending_add(sys, *when);
    if (out.is_valid)
        output(dec, &out);
    return 0;
}

static int Decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    if (block == NULL) {   /* the end: what's left */
        if (!sys->failed)
            drain(dec, true);
        return VLCDEC_SUCCESS;
    }
    /* (a block VLC marks damaged still goes in: leaving it out broke the
     * pictures that refer to it) */
    if (sys->failed) {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    bool marked_key = (block->i_flags & BLOCK_FLAG_TYPE_I) != 0;
    if (sys->codec == VDEC_CODEC_VP9 && !sys->vp9_checked) {
        /* the stream's own profile (0: 8-bit, 2: 10-bit), from the first
         * frame's header (a superframe's first frame starts the block): the
         * file rarely says, and a decoder made for the other one refused
         * every frame (2026-10-13 log). Before the block is copied: a
         * rebuild frees the access unit slots. */
        sys->vp9_checked = true;
        unsigned prof = vp9_profile(block->p_buffer, block->i_buffer);
        if (prof <= 3 && prof != sys->cfg.profile) {
            if (prof == 1 || prof == 3)
                give_up(dec, "VP9 4:2:2 or 4:4:4");
            if (prof == 1 || prof == 3 || !vp9_rebuild(dec, prof)) {
                block_Release(block);
                return VLCDEC_SUCCESS;
            }
        }
    }

    /* the access unit into this turn's slot, made bigger first if it needs
     * to be (a 4K keyframe can pass 4 MiB; cut short, it was a broken
     * picture): at most the parameter sets, the block, and for 1- or 2-byte
     * lengths the start codes they become */
    Mem *slot = &sys->au[sys->au_next];
    size_t need = sys->headers_size + block->i_buffer * (sys->nal_length && sys->nal_length < 4 ? 3 : 1) + 64;
    if (need > slot->len) {
        mem_free(slot);
        if (!mem_direct(slot, need + ((size_t)1 << 20), PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) {
            VLOG("no memory for an access unit of %zu bytes\n", block->i_buffer);
            give_up(dec, "access unit too big");
            block_Release(block);
            return VLCDEC_SUCCESS;
        }
        VLOG("access unit slot grown to %zu KiB\n", slot->len >> 10);
    }
    uint8_t *au = slot->addr;
    size_t size = 0;
    if (sys->send_headers && sys->headers_size && sys->headers_size < slot->len) {
        memcpy(au, sys->headers, sys->headers_size);
        size = sys->headers_size;
    }
    if (sys->nal_length && !(block->i_buffer >= 4 && !memcmp(block->p_buffer, "\x00\x00\x00\x01", 4))) {
        size += to_annexb(au + size, slot->len - size, block->p_buffer, block->i_buffer, sys->nal_length,
                          sys->codec == VDEC_CODEC_HEVC);
    } else if (size + block->i_buffer <= slot->len) {
        memcpy(au + size, block->p_buffer, block->i_buffer);
        size += block->i_buffer;
    } else {
        VLOG("access unit of %zu bytes is too big\n", block->i_buffer);
        give_up(dec, "access unit too big");
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    mtime_t pts = block->i_pts != VLC_TS_INVALID ? block->i_pts : block->i_dts;
    uint32_t field_flags = block->i_flags & (BLOCK_FLAG_TOP_FIELD_FIRST | BLOCK_FLAG_BOTTOM_FIELD_FIRST);
    block_Release(block);
    if (sys->codec == VDEC_CODEC_AVC)
        h264_scan_sps(sys, au, size);

    /* After a seek or a stream error: nothing until a keyframe. VLC's MKV
     * reader marks them, its MP4 reader doesn't (console run 9: every jump
     * in an MP4 waited for good), so the access unit itself is looked at. */
    if (sys->wait_keyframe) {
        if (!marked_key && !is_keyframe(au, size, sys->codec)) {
            if (++sys->skipped >= 600)
                give_up(dec, "no keyframe after a seek or a stream error");
            return VLCDEC_SUCCESS;
        }
        VLOG("going on from a keyframe (%u skipped)\n", sys->skipped);
        sys->key_pts = pts;
        sys->wait_keyframe = false;
        sys->skip_rasl = true;
        sys->skipped = 0;
    }

    /* After a restart point (a seek, a reset), HEVC's RASL pictures refer to
     * pictures from before it, which the decoder never got: given to it,
     * every picture after failed (open-GOP x265 encodes). They're left out
     * until the first picture that isn't one. */
    if (sys->skip_rasl && sys->codec == VDEC_CODEC_HEVC) {
        int type = hevc_first_slice_type(au, size);
        if (type == 8 || type == 9) {   /* RASL_N, RASL_R */
            sys->skipped++;
            return VLCDEC_SUCCESS;
        }
        if (type >= 0 && type != 21 && type != 16 && type != 17 && type != 18 && type != 19 &&
            type != 20)
            sys->skip_rasl = false;     /* past the restart point's own pictures */
    }

    /* One time per picture that will come out. A field coded on its own
     * (1080i TV) comes out as one frame with its other field: the pair's
     * time is the first field's, the second adds none (two each made the
     * times run behind the pictures: stutter, late drops and the sound
     * drifting away, issue #11). A VP9 block with no frame shown (a lone
     * hidden reference frame) adds none either. */
    bool field = false, bottom = false, second_field = false;
    unsigned frame_num = 0;
    if (sys->codec == VDEC_CODEC_AVC && sys->sps_ok && !sys->frame_mbs_only &&
        h264_slice_field(sys, au, size, &field, &bottom, &frame_num) && field) {
        if (sys->field_open && bottom != sys->field_open_bottom && frame_num == sys->field_open_frame_num) {
            second_field = true;
            sys->field_open = false;
            if (++sys->fields_paired == 1)
                VLOG("field pictures: two fields make one frame (first field %s)\n", bottom ? "top" : "bottom");
        } else {
            sys->field_open = true;
            sys->field_open_bottom = bottom;
            sys->field_open_frame_num = frame_num;
        }
    } else {
        sys->field_open = false;
    }
    size_t voff[8], vlen[8];
    bool vshow[8];
    unsigned vframes = 0;
    bool shows = !second_field;
    if (sys->codec == VDEC_CODEC_VP9) {
        vframes = vp9_frames(au, size, voff, vlen);
        if (!vframes) {
            VLOG("VP9 block of %zu bytes with a broken superframe index: left out\n", size);
            return VLCDEC_SUCCESS;
        }
        shows = false;
        for (unsigned i = 0; i < vframes; i++) {
            bool key;
            vp9_header(au + voff[i], vlen[i], &key, &vshow[i]);
            shows = shows || vshow[i];
        }
    }
    bool interlaced = sys->codec == VDEC_CODEC_AVC && sys->sps_ok && !sys->frame_mbs_only;
    bool top_first = field ? !bottom : !(field_flags & BLOCK_FLAG_BOTTOM_FIELD_FIRST);
    Pending when = { pts, interlaced, top_first, false };
    if (sys->codec == VDEC_CODEC_VP9) {
        /* one picture comes out per block, shown or not (see pending_take_fifo) */
        when.hidden = !shows;
        shows = true;
    }

    int rc = run_decode(dec, au, size, pts, shows ? &when : NULL);
    sys->au_next = (sys->au_next + 1) % AU_SLOTS;
    if (rc != 0) {
        sys->errors++;
        if (sys->errors <= 5)
            VLOG("decode %#x (access unit %u, %zu bytes)\n", (unsigned)rc, sys->decoded, size);
        /* VP9: the chip decodes some encodes and refuses others (from the
         * first refused frame on, keyframes after a reset too: ten seconds
         * of stutter before FFmpeg, 2026-10-13 log). FFmpeg at once, at the
         * same place. */
        if (sys->codec == VDEC_CODEC_VP9) {
            give_up(dec, "a VP9 frame the chip refuses");
            return VLCDEC_SUCCESS;
        }
        /* never got going: FFmpeg instead */
        if (sys->shown == 0 && sys->errors >= 3) {
            give_up(dec, "the decoder refuses this video");
            return VLCDEC_SUCCESS;
        }
        /* a stream error leaves the decoder refusing everything after it:
         * start it again from the next keyframe, a few times at most */
        if ((unsigned)rc >= VDEC_ERROR_STREAM_FIRST && (unsigned)rc <= VDEC_ERROR_STREAM_LAST) {
            if (sys->resyncs++ >= MAX_RESYNCS) {
                give_up(dec, "stream errors keep coming");
                return VLCDEC_SUCCESS;
            }
            int r = sceVideodec2Reset(sys->dec);
            VLOG("stream error %#x: reset (%#x), waiting for a keyframe (%u of %u)\n", (unsigned)rc,
                 (unsigned)r, sys->resyncs, MAX_RESYNCS);
            sys->pending_count = 0;
            sys->field_open = false;
            sys->send_headers = true;
            sys->wait_keyframe = true;
            memset(sys->frame_held, 0, sizeof(sys->frame_held));
        } else if (sys->errors >= 30) {
            give_up(dec, "the decoder refuses this video");
        }
        return VLCDEC_SUCCESS;
    }
    sys->send_headers = false;
    sys->decoded++;
    if (sys->decoded == 1 || sys->decoded == 100)
        VLOG("%u access units in, %u pictures out, %u errors\n", sys->decoded, sys->shown, sys->errors);
    return VLCDEC_SUCCESS;
}

static void Flush(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    if (sys->failed)
        return;
    drain(dec, false);
    int rc = sceVideodec2Reset(sys->dec);
    if (rc != 0)
        VLOG("reset %#x\n", (unsigned)rc);
    sys->pending_count = 0;
    sys->field_open = false;
    sys->send_headers = true;
    /* after a reset the decoder holds nothing, and takes nothing but a
     * keyframe: a seek's first block often isn't one (console run 5: every
     * jump ended in INVALID_SEQUENCE and a stalled picture) */
    memset(sys->frame_held, 0, sizeof(sys->frame_held));
    sys->wait_keyframe = true;
    sys->skipped = 0;
}

static int bring_up(decoder_sys_t *sys, VdecConfig *config, VdecMemory *mem);

/* VP9: the decoder made again for the stream's profile (pipeline 4, then 1:
 * 10-bit 4K at 4 asked for 467 + 390 MiB of direct memory and got none).
 * false: the video went to FFmpeg. */
static bool vp9_rebuild(decoder_t *dec, unsigned profile)
{
    decoder_sys_t *sys = dec->p_sys;
    if (sys->dec)
        sceVideodec2DeleteDecoder(sys->dec);
    sys->dec = NULL;
    for (int i = 0; i < AU_SLOTS; i++)
        mem_free(&sys->au[i]);
    mem_free(&sys->frames);
    mem_free(&sys->cpu_gpu);
    mem_free(&sys->gpu);
    mem_free(&sys->cpu);
    memset(sys->frame_held, 0, sizeof(sys->frame_held));
    VdecConfig c = sys->cfg;
    c.profile = profile;
    c.max_level = 62;
    VdecMemory mem;
    int rc = -1;
    static const uint32_t depths[2] = { 4, 1 };
    for (int d = 0; d < 2 && rc != 0 && !sys->chip_refused; d++) {
        c.decode_pipeline_depth = depths[d];
        rc = bring_up(sys, &c, &mem);
    }
    VLOG("VP9 stream is profile %u (%s): decoder made again: %#x (pipeline %u)\n", profile,
         profile == 2 ? "10-bit" : "8-bit", (unsigned)rc, c.decode_pipeline_depth);
    if (rc != 0) {
        give_up(dec, "no VP9 decoder for this profile");
        return false;
    }
    sys->cfg = c;
    sys->au_next = 0;
    return true;
}

static void free_sys(decoder_sys_t *sys)
{
    if (sys->dec)
        sceVideodec2DeleteDecoder(sys->dec);
    for (int i = 0; i < AU_SLOTS; i++)
        mem_free(&sys->au[i]);
    mem_free(&sys->frames);
    mem_free(&sys->cpu_gpu);
    mem_free(&sys->gpu);
    mem_free(&sys->cpu);
    free(sys->headers);
    free(sys);
}

/* The decoder's memory and the decoder itself, for this config; on failure
 * what was taken is given back (so a lighter config can be tried). */
static int bring_up(decoder_sys_t *sys, VdecConfig *config, VdecMemory *mem)
{
    *mem = (VdecMemory){ .this_size = sizeof(*mem) };
    int rc = sceVideodec2QueryDecoderMemoryInfo(config, mem);
    if (rc != 0) {
        int32_t w = config->max_frame_width, h = config->max_frame_height;
        if (cached_answer(config, mem)) {
            VLOG("memory query %#x: using the answer from before full access (%dx%d for %dx%d)\n",
                 (unsigned)rc, config->max_frame_width, config->max_frame_height, w, h);
            rc = 0;
        } else {
            VLOG("memory query %#x\n", (unsigned)rc);
            return rc;
        }
    }
    size_t align = mem->frame_buffer_alignment > PAGE_16K ? mem->frame_buffer_alignment : PAGE_16K;
    sys->frame_size = (mem->max_frame_buffer_size + align - 1) & ~(align - 1);
    bool ok = (!mem->cpu_memory_size || mem_cpu(&sys->cpu, mem->cpu_memory_size)) &&
              (!mem->gpu_memory_size || mem_direct(&sys->gpu, mem->gpu_memory_size, PAGE_16K,
                                                   PROT_CPU_RW | PROT_GPU_RW)) &&
              (!mem->cpu_gpu_memory_size || mem_direct(&sys->cpu_gpu, mem->cpu_gpu_memory_size,
                                                       PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) &&
              mem_direct(&sys->frames, sys->frame_size * sys->frame_count, align,
                         PROT_CPU_RW | PROT_GPU_RW);
    for (int i = 0; ok && i < AU_SLOTS; i++)
        ok = mem_direct(&sys->au[i], AU_SLOT_SIZE, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW);
    if (ok) {
        /* The memory as new: the decoder before's pictures are still in it
         * (the kernel's direct memory and our reused CPU block come back as
         * they were left), and a picture the chip didn't fully decode showed
         * the video before for a moment ("a frame from another video", only
         * after a different video, 2026-10-13). Picture buffers mid-grey. */
        mtime_t t0 = mdate();
        if (sys->cpu.addr)
            memset(sys->cpu.addr, 0, sys->cpu.len);
        if (sys->gpu.addr)
            memset(sys->gpu.addr, 0, sys->gpu.len);
        if (sys->cpu_gpu.addr)
            memset(sys->cpu_gpu.addr, 0, sys->cpu_gpu.len);
        bool ten = config->codec_type != VDEC_CODEC_AVC && config->profile == 2;
        if (ten) {
            uint64_t *q = sys->frames.addr;
            for (size_t i = 0, n = sys->frames.len / 8; i < n; i++)
                q[i] = 0x0200020002000200ull;
        } else {
            memset(sys->frames.addr, 0x80, sys->frames.len);
        }
        VLOG("decoder memory cleared in %.1f ms\n", (mdate() - t0) / 1000.0);
        mem->cpu_memory = sys->cpu.addr;
        mem->gpu_memory = sys->gpu.addr;
        mem->cpu_gpu_memory = sys->cpu_gpu.addr;
        rc = sceVideodec2CreateDecoder(config, mem, &sys->dec);
        if (rc == 0)
            return 0;
        VLOG("create %#x\n", (unsigned)rc);
        /* 0x811D01xx: the chip itself (resources, state), not this config:
         * other configs won't do better (75 tries in a row, 2026-10-13) */
        if (((unsigned)rc & 0xFFFFFF00u) == 0x811D0100u)
            sys->chip_refused = true;
        sys->dec = NULL;
    } else {
        VLOG("no memory (cpu %" PRIu64 ", gpu %" PRIu64 ", cpu+gpu %" PRIu64 " KiB, frames %u x %zu KiB)\n",
             mem->cpu_memory_size >> 10, mem->gpu_memory_size >> 10, mem->cpu_gpu_memory_size >> 10,
             sys->frame_count, sys->frame_size >> 10);
        rc = -1;
    }
    for (int i = 0; i < AU_SLOTS; i++)
        mem_free(&sys->au[i]);
    mem_free(&sys->frames);
    mem_free(&sys->cpu_gpu);
    mem_free(&sys->gpu);
    mem_free(&sys->cpu);
    return rc;
}

static int OpenDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;
    if (!var_InheritBool(dec, "ps5vdec"))
        return VLC_EGENERIC;
    bool ours = dec->fmt_in.i_codec == VLC_CODEC_H264 || dec->fmt_in.i_codec == VLC_CODEC_HEVC ||
                dec->fmt_in.i_codec == VLC_CODEC_VP9;
    if (!vlc_ps5_vdec_available || !vlc_ps5_vdec_available()) {
        if (ours)
            VLOG("hardware decoding isn't available: FFmpeg\n");
        return VLC_EGENERIC;
    }

    const es_format_t *in = &dec->fmt_in;
    uint32_t codec, profile, level;
    if (in->i_codec == VLC_CODEC_H264) {
        /* the decoder does 8-bit 4:2:0: not High 10, 4:2:2, 4:4:4, CAVLC 4:4:4 */
        if (in->i_profile == 110 || in->i_profile == 122 || in->i_profile == 244 || in->i_profile == 44) {
            VLOG("H.264 profile %d (10-bit or 4:2:2 / 4:4:4): FFmpeg\n", in->i_profile);
            return VLC_EGENERIC;
        }
        codec = VDEC_CODEC_AVC;
        profile = 100;
        level = 52;
    } else if (in->i_codec == VLC_CODEC_HEVC) {
        /* Main and Main 10 (Main 10's decoder does both); not the range extensions */
        if (in->i_profile > 2) {
            VLOG("HEVC profile %d (range extensions): FFmpeg\n", in->i_profile);
            return VLC_EGENERIC;
        }
        codec = VDEC_CODEC_HEVC;
        profile = 2;
        level = 156;   /* 5.2: 4K films at 60-80 Mbit/s (the Hybrid sample) */
    } else if (in->i_codec == VLC_CODEC_VP9) {
        /* profiles 0 (8-bit) and 2 (10-bit, YouTube HDR) 4:2:0; 1 and 3
         * (4:2:2, 4:4:4) to FFmpeg. The console's profile and level values
         * for VP9 aren't documented: tried below, and logged. */
        if (in->i_profile == 1 || in->i_profile == 3)
            return VLC_EGENERIC;
        codec = VDEC_CODEC_VP9;
        profile = in->i_profile == 2 ? 2 : 0;   /* unknown: 8-bit, the usual (the first frame says) */
        level = 62;                             /* what the chip took (2026-10-13 log) */
    } else {
        return VLC_EGENERIC;
    }
    unsigned w = in->video.i_width, h = in->video.i_height;
    if (w > 3840 || h > 2176) {
        VLOG("%ux%u is bigger than 4K: FFmpeg\n", w, h);
        return VLC_EGENERIC;
    }
    bool small = w && h && w <= 1920 && h <= 1088;

    void *queue = get_compute_queue();
    if (queue && codec == VDEC_CODEC_VP9 && vp9_queue)
        queue = vp9_queue;
    if (!queue) {
        VLOG("no compute queue: FFmpeg\n");
        return VLC_EGENERIC;
    }

    decoder_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    for (int i = 0; i < AU_SLOTS; i++)
        sys->au[i].start = -1;
    sys->frames.start = sys->cpu.start = sys->gpu.start = sys->cpu_gpu.start = -1;
    sys->codec = codec;

    VdecConfig config = {
        .this_size = sizeof(config),
        .resource_type = 1,
        .codec_type = codec,
        .profile = profile,
        .max_level = level,
        /* the video's own size (rounded to 16), 4K when it isn't known yet */
        .max_frame_width = w ? (int32_t)((w + 15) & ~15u) : 3840,
        .max_frame_height = h ? (int32_t)((h + 15) & ~15u) : 2176,
        /* the decoder sizes its picture store for the stream itself */
        .max_dpb_frame_count = -1,
        /* several pictures in the decoder at once: one at a time left it idle
         * between calls, 20 pictures a second for a 4K high-tier film that
         * needs 24 (console run 6) */
        .decode_pipeline_depth = 4,
        .compute_queue = queue,
        .cpu_affinity_mask = 0x3f,
        .cpu_thread_priority = 700,
        .optimize_progressive_video = true,
    };
    /* the decoder's picture store, plus a few waiting to be shown */
    sys->frame_count = small ? MAX_FRAMES : 10;
    VdecMemory mem;
    int rc = bring_up(sys, &config, &mem);
    if (rc != 0 && !sys->chip_refused && config.decode_pipeline_depth > 1) {
        /* depth 4 costs memory (cpu+gpu 90 -> 189 MiB at 4K): when it can't
         * have it, one at a time still plays most videos */
        VLOG("pipeline depth %u failed (%#x): trying 1\n", config.decode_pipeline_depth, (unsigned)rc);
        config.decode_pipeline_depth = 1;
        rc = bring_up(sys, &config, &mem);
    }
    if (rc != 0 && !sys->chip_refused && (config.max_frame_width < 1920 || config.max_frame_height < 1088)) {
        /* small TV pictures (544x482, 640x386) failed the memory query
         * (0x811D0200) while 1080p and 4K never did: room for 1080p, which
         * still decodes them */
        VLOG("%dx%d refused (%#x): trying room for 1920x1088\n", config.max_frame_width,
             config.max_frame_height, (unsigned)rc);
        config.max_frame_width = config.max_frame_width > 1920 ? config.max_frame_width : 1920;
        config.max_frame_height = config.max_frame_height > 1088 ? config.max_frame_height : 1088;
        config.decode_pipeline_depth = 4;
        rc = bring_up(sys, &config, &mem);
        if (rc != 0 && !sys->chip_refused) {
            config.decode_pipeline_depth = 1;
            rc = bring_up(sys, &config, &mem);
        }
    }
    if (rc != 0 && !sys->chip_refused && codec == VDEC_CODEC_VP9) {
        /* VP9's values, one after another until one is taken: the profile
         * as VP9 numbers it (10-bit first when the file doesn't say: like
         * HEVC Main 10, it may do both), levels as 10 x VP9's or none */
        static const uint32_t levels[] = { 62, 61, 52, 51 };
        const uint32_t profiles[2] = { profile, profile == 2 ? 0 : 2 };
        for (int pi = 0; pi < 2 && rc != 0 && !sys->chip_refused; pi++)
            for (size_t li = 0; li < sizeof(levels) / sizeof(levels[0]) && rc != 0; li++) {
                config.profile = profiles[pi];
                config.max_level = levels[li];
                config.decode_pipeline_depth = 1;
                rc = bring_up(sys, &config, &mem);
                VLOG("VP9 profile %u level %u (pipeline 1): %#x\n", config.profile, config.max_level, (unsigned)rc);
            }
    }
    if (rc != 0 && !sys->chip_refused && config.max_level == 156) {
        /* level 5.2 not taken: 5.1, the one known to work */
        VLOG("level 5.2 failed (%#x): trying 5.1\n", (unsigned)rc);
        config.max_level = 153;
        rc = bring_up(sys, &config, &mem);
    }
    if (rc != 0) {
        free_sys(sys);
        return VLC_EGENERIC;
    }
    sys->cfg = config;
    if (codec != VDEC_CODEC_VP9)   /* VP9's extra data (codec features) isn't for the decoder */
        read_headers(dec, sys);
    if (codec == VDEC_CODEC_AVC && sys->headers)
        h264_scan_sps(sys, sys->headers, sys->headers_size);
    sys->send_headers = true;
    sys->skip_rasl = true;   /* a stream may start at an open-GOP restart point */
    /* and from its first keyframe: a TV channel joined mid-way, or a file
     * that doesn't start on one, gave the chip pictures referring to ones it
     * never got */
    sys->wait_keyframe = true;
    sys->key_pts = VLC_TS_INVALID;
    /* the first units' and pictures' lines: with the detailed log only */
    if (!vlc_ps5_log_details || !vlc_ps5_log_details())
        sys->diag_in = sys->diag_out = 12;
    VLOG("%s %ux%u (profile %d, level %d; decoder profile %u level %u, pipeline %u): decoder ready, memory cpu %" PRIu64
         " / gpu %" PRIu64 " / cpu+gpu %" PRIu64 " KiB, frames %u x %zu KiB, headers %zu bytes%s\n",
         codec == VDEC_CODEC_AVC ? "H.264" : codec == VDEC_CODEC_HEVC ? "HEVC" : "VP9", w, h, in->i_profile,
         in->i_level, config.profile, config.max_level,
         config.decode_pipeline_depth,
         mem.cpu_memory_size >> 10, mem.gpu_memory_size >> 10, mem.cpu_gpu_memory_size >> 10,
         sys->frame_count, sys->frame_size >> 10, sys->headers_size,
         sys->nal_length ? " (length-prefixed input)" : "");

    dec->p_sys = sys;
    es_format_Copy(&dec->fmt_out, in);
    dec->fmt_out.i_cat = VIDEO_ES;
    dec->fmt_out.i_codec = VLC_CODEC_NV12;
    dec->fmt_out.video.i_chroma = VLC_CODEC_NV12;
    dec->pf_decode = Decode;
    dec->pf_flush = Flush;
    if (vlc_ps5_vdec_active)
        vlc_ps5_vdec_active(1);
    return VLC_SUCCESS;
}

static void CloseDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;
    decoder_sys_t *sys = dec->p_sys;
    VLOG("closed: %u access units, %u pictures, %u errors\n", sys->decoded, sys->shown, sys->errors);
    free_sys(sys);
    if (vlc_ps5_vdec_active)
        vlc_ps5_vdec_active(0);
}

vlc_module_begin()
    set_shortname("PS5 video decoder")
    set_description("H.264, HEVC and VP9 on the console's video decoder")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_capability("video decoder", 120)
    add_bool("ps5vdec", true, "Hardware decoding", "Decode H.264, HEVC and VP9 on the console's video decoder", false)
    set_callbacks(OpenDecoder, CloseDecoder)
vlc_module_end()
