/*
 * ps5vdec: H.264 and HEVC (8 and 10-bit) up to 4K on the console's video
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
    int64_t start;    /* direct memory; -1: flexible */
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

static void mem_free(Mem *m)
{
    if (!m->addr)
        return;
    if (m->start >= 0) {
        sceKernelMunmap(m->addr, m->len);
        sceKernelReleaseDirectMemory(m->start, m->len);
    } else {
        sceKernelReleaseFlexibleMemory(m->addr, m->len);
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
void vlc_ps5_vdec_prepare(void);
void vlc_ps5_vdec_prepare(void)
{
    get_compute_queue();
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
     * still waiting */
    mtime_t pending[MAX_PENDING];
    unsigned pending_count;

    unsigned decoded, errors, shown;
    bool failed;
    bool wait_keyframe;        /* after a stream error: from the next keyframe on */
    bool skip_rasl;            /* HEVC: after a restart point, its RASL pictures */
    unsigned resyncs, skipped;
    bool format_set;
    uint32_t out_w, out_h, out_pitch_bytes;
    bool out_ten;
};

static void pending_add(decoder_sys_t *sys, mtime_t t)
{
    if (sys->pending_count == MAX_PENDING) {
        memmove(sys->pending, sys->pending + 1, (MAX_PENDING - 1) * sizeof(mtime_t));
        sys->pending_count--;
    }
    sys->pending[sys->pending_count++] = t;
}

static mtime_t pending_take(decoder_sys_t *sys)
{
    if (!sys->pending_count)
        return VLC_TS_INVALID;
    unsigned best = 0;
    for (unsigned i = 1; i < sys->pending_count; i++)
        if (sys->pending[i] < sys->pending[best])
            best = i;
    mtime_t t = sys->pending[best];
    sys->pending[best] = sys->pending[--sys->pending_count];
    return t;
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

/* An access unit a decoder can start from (Annex B): H.264 an IDR picture or
 * a picture of I slices, HEVC a random access point (IRAP, types 16-21). */
static bool is_keyframe(const uint8_t *p, size_t n, uint32_t codec)
{
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

/* One decoded picture into a VLC picture. */
static void output(decoder_t *dec, const VdecOutput *out)
{
    decoder_sys_t *sys = dec->p_sys;
    mtime_t pts = pending_take(sys);
    if (out->is_error_frame || !out->frame_buffer) {
        sys->errors++;
        frame_back(sys, out->frame_buffer);
        return;
    }
    bool ten = out->frame_pitch_in_bytes >= 2 * out->frame_pitch;
    if (!sys->format_set || out->frame_width != sys->out_w || out->frame_height != sys->out_h ||
        ten != sys->out_ten) {
        video_format_t *v = &dec->fmt_out.video;
        /* 10-bit as planar I0AL, not P010: VLC draws subtitles onto I0AL but
         * has no way to blend them onto P010 (console run 7: none showed) */
        dec->fmt_out.i_codec = ten ? VLC_CODEC_I420_10L : VLC_CODEC_NV12;
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
        sys->format_set = true;
        VLOG("pictures %ux%u (shown %ux%u) %s, pitch %u bytes, buffer %" PRIu64 " bytes, format %u\n",
             out->frame_width, out->frame_height, v->i_visible_width, v->i_visible_height,
             ten ? "I0AL" : "NV12", out->frame_pitch_in_bytes, out->frame_buffer_size,
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
    } else {
        size_t bytes = pitch < (size_t)pic->p[1].i_pitch ? pitch : (size_t)pic->p[1].i_pitch;
        copy_plane(pic->p[1].p_pixels, pic->p[1].i_pitch, chroma, pitch, bytes, chroma_rows, 0);
    }
    sys->copy_us += mdate() - t0;
    frame_back(sys, out->frame_buffer);
    pic->date = pts;
    pic->b_progressive = true;
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
    block_Release(block);

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

    VdecInput in = { .this_size = sizeof(in), .au_data = au, .au_size = size,
                     .pts_data = (uint64_t)pts, .dts_data = UINT64_MAX };
    VdecFrameBuffer fb;
    int frame = next_frame(sys, &fb);
    if (frame < 0) {
        /* every buffer waiting to be shown: shouldn't happen with the count
         * the decoder's picture store needs, but never hand one out twice */
        VLOG("no free picture buffer (%u)\n", sys->frame_count);
        give_up(dec, "out of picture buffers");
        return VLCDEC_SUCCESS;
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
    sys->au_next = (sys->au_next + 1) % AU_SLOTS;
    if (fb.is_accepted)
        sys->frame_held[frame] = true;
    if (rc != 0) {
        sys->errors++;
        if (sys->errors <= 5)
            VLOG("decode %#x (access unit %u, %zu bytes)\n", (unsigned)rc, sys->decoded, size);
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
    pending_add(sys, pts);
    if (out.is_valid)
        output(dec, &out);
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
    sys->send_headers = true;
    /* after a reset the decoder holds nothing, and takes nothing but a
     * keyframe: a seek's first block often isn't one (console run 5: every
     * jump ended in INVALID_SEQUENCE and a stalled picture) */
    memset(sys->frame_held, 0, sizeof(sys->frame_held));
    sys->wait_keyframe = true;
    sys->skipped = 0;
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
static int bring_up(decoder_sys_t *sys, const VdecConfig *config, VdecMemory *mem)
{
    *mem = (VdecMemory){ .this_size = sizeof(*mem) };
    int rc = sceVideodec2QueryDecoderMemoryInfo(config, mem);
    if (rc != 0) {
        VLOG("memory query %#x\n", (unsigned)rc);
        return rc;
    }
    size_t align = mem->frame_buffer_alignment > PAGE_16K ? mem->frame_buffer_alignment : PAGE_16K;
    sys->frame_size = (mem->max_frame_buffer_size + align - 1) & ~(align - 1);
    bool ok = (!mem->cpu_memory_size || mem_flexible(&sys->cpu, mem->cpu_memory_size)) &&
              (!mem->gpu_memory_size || mem_direct(&sys->gpu, mem->gpu_memory_size, PAGE_16K,
                                                   PROT_CPU_RW | PROT_GPU_RW)) &&
              (!mem->cpu_gpu_memory_size || mem_direct(&sys->cpu_gpu, mem->cpu_gpu_memory_size,
                                                       PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) &&
              mem_direct(&sys->frames, sys->frame_size * sys->frame_count, align,
                         PROT_CPU_RW | PROT_GPU_RW);
    for (int i = 0; ok && i < AU_SLOTS; i++)
        ok = mem_direct(&sys->au[i], AU_SLOT_SIZE, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW);
    if (ok) {
        mem->cpu_memory = sys->cpu.addr;
        mem->gpu_memory = sys->gpu.addr;
        mem->cpu_gpu_memory = sys->cpu_gpu.addr;
        rc = sceVideodec2CreateDecoder(config, mem, &sys->dec);
        if (rc == 0)
            return 0;
        VLOG("create %#x\n", (unsigned)rc);
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
    if (!vlc_ps5_vdec_available || !vlc_ps5_vdec_available())
        return VLC_EGENERIC;

    const es_format_t *in = &dec->fmt_in;
    uint32_t codec, profile, level;
    if (in->i_codec == VLC_CODEC_H264) {
        /* the decoder does 8-bit 4:2:0: not High 10, 4:2:2, 4:4:4, CAVLC 4:4:4 */
        if (in->i_profile == 110 || in->i_profile == 122 || in->i_profile == 244 || in->i_profile == 44)
            return VLC_EGENERIC;
        codec = VDEC_CODEC_AVC;
        profile = 100;
        level = 52;
    } else if (in->i_codec == VLC_CODEC_HEVC) {
        /* Main and Main 10 (Main 10's decoder does both); not the range extensions */
        if (in->i_profile > 2)
            return VLC_EGENERIC;
        codec = VDEC_CODEC_HEVC;
        profile = 2;
        level = 156;   /* 5.2: 4K films at 60-80 Mbit/s (the Hybrid sample) */
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
    if (!queue)
        return VLC_EGENERIC;

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
    if (rc != 0 && config.decode_pipeline_depth > 1) {
        /* depth 4 costs memory (cpu+gpu 90 -> 189 MiB at 4K): when it can't
         * have it, one at a time still plays most videos */
        VLOG("pipeline depth %u failed (%#x): trying 1\n", config.decode_pipeline_depth, (unsigned)rc);
        config.decode_pipeline_depth = 1;
        rc = bring_up(sys, &config, &mem);
    }
    if (rc != 0 && config.max_level == 156) {
        /* level 5.2 not taken: 5.1, the one known to work */
        VLOG("level 5.2 failed (%#x): trying 5.1\n", (unsigned)rc);
        config.max_level = 153;
        rc = bring_up(sys, &config, &mem);
    }
    if (rc != 0) {
        free_sys(sys);
        return VLC_EGENERIC;
    }
    read_headers(dec, sys);
    sys->send_headers = true;
    sys->skip_rasl = true;   /* a stream may start at an open-GOP restart point */
    VLOG("%s %ux%u (profile %d, level %d; decoder level %u, pipeline %u): decoder ready, memory cpu %" PRIu64
         " / gpu %" PRIu64 " / cpu+gpu %" PRIu64 " KiB, frames %u x %zu KiB, headers %zu bytes%s\n",
         codec == VDEC_CODEC_AVC ? "H.264" : "HEVC", w, h, in->i_profile, in->i_level, config.max_level,
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
    set_description("H.264 and HEVC on the console's video decoder")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_capability("video decoder", 120)
    add_bool("ps5vdec", true, "Hardware decoding", "Decode H.264 and HEVC on the console's video decoder", false)
    set_callbacks(OpenDecoder, CloseDecoder)
vlc_module_end()
