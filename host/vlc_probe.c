/* vlc_probe: World 1's proof that the VLC-PS5 design works.
 *
 *   vlc_probe <media> <outdir> [snapshot-frame]
 *
 * Plays <media> through libvlc with the same plumbing the PS5 app will use:
 *   - video callbacks (vmem): VLC decodes into planes we own (I420, or I0AL for 10-bit),
 *     and one frame is converted to RGB with the shader's math and saved as a PNG;
 *   - audio callbacks (amem): S16 stereo 48 kHz, which is what SceAudioOut takes,
 *     written to a WAV file.
 * Ends with a report: tracks, frames, timing, A/V sample counts.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <zlib.h>
#include <vlc/vlc.h>

#define MAX_PLANES 3
#define SAMPLE_RATE 48000
#define CHANNELS 2

static const char *outdir;
static unsigned snapshot_frame = 100;

/* ---- video ---- */

struct video {
    char chroma[5];
    unsigned width, height;          /* buffer size, padded by the decoder */
    volatile unsigned vis_w, vis_h;  /* visible size, from libvlc_video_get_size() */
    unsigned pitches[MAX_PLANES];    /* bytes per row */
    unsigned lines[MAX_PLANES];      /* rows */
    uint8_t *planes[MAX_PLANES];
    unsigned frames;
    int saved;
    int64_t first_ns, last_ns;
};
static struct video vid;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static unsigned align32(unsigned x) { return (x + 31) & ~31u; }

static unsigned video_format(void **opaque, char *chroma, unsigned *width, unsigned *height,
                             unsigned *pitches, unsigned *lines)
{
    (void)opaque;
    /* 10-bit sources come as I0AL (16-bit little-endian samples, 10 bits used), so the
     * PS5 can show them on an R16 texture; everything else as I420. */
    int ten_bit = !strncmp(chroma, "I0AL", 4) || !strncmp(chroma, "I0AB", 4);
    memcpy(vid.chroma, ten_bit ? "I0AL" : "I420", 4);
    memcpy(chroma, vid.chroma, 4);
    vid.width = *width;
    vid.height = *height;

    unsigned bpp = ten_bit ? 2 : 1;
    unsigned cw = (*width + 1) / 2, ch = (*height + 1) / 2;
    vid.pitches[0] = align32(*width * bpp);
    vid.lines[0] = align32(*height);
    vid.pitches[1] = vid.pitches[2] = align32(cw * bpp);
    vid.lines[1] = vid.lines[2] = align32(ch);
    for (int i = 0; i < MAX_PLANES; i++) {
        pitches[i] = vid.pitches[i];
        lines[i] = vid.lines[i];
        free(vid.planes[i]);
        vid.planes[i] = aligned_alloc(64, (size_t)pitches[i] * lines[i]);
        if (!vid.planes[i])
            return 0;
    }
    printf("[video] %ux%u, VLC offered %.4s -> we take %.4s\n", *width, *height, chroma, vid.chroma);
    return 1; /* one picture buffer */
}

static void video_cleanup(void *opaque)
{
    (void)opaque;
    for (int i = 0; i < MAX_PLANES; i++) {
        free(vid.planes[i]);
        vid.planes[i] = NULL;
    }
}

static void *video_lock(void *opaque, void **planes)
{
    (void)opaque;
    for (int i = 0; i < MAX_PLANES; i++)
        planes[i] = vid.planes[i];
    return NULL;
}

static void video_unlock(void *opaque, void *picture, void *const *planes)
{
    (void)opaque; (void)picture; (void)planes;
}

/* BT.709 limited range -> RGB. Same constants as shaders/yuv.frag will use. */
static uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f); }

static float sample(int plane, unsigned x, unsigned y, int ten_bit)
{
    const uint8_t *row = vid.planes[plane] + (size_t)y * vid.pitches[plane];
    if (ten_bit)
        return ((const uint16_t *)row)[x] / 4.0f; /* 10 bits -> 8-bit scale */
    return row[x];
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t be[4] = { len >> 24, len >> 16, len >> 8, len };
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    crc = crc32(crc, data, len);
    uint8_t c[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
    fwrite(c, 1, 4, f);
}

static void save_png(void)
{
    /* The buffer is padded (854x480 arrives as 864x482): save only the visible part. */
    unsigned w = vid.vis_w && vid.vis_w <= vid.width ? vid.vis_w : vid.width;
    unsigned h = vid.vis_h && vid.vis_h <= vid.height ? vid.vis_h : vid.height;
    int ten_bit = !memcmp(vid.chroma, "I0AL", 4);
    size_t stride = 1 + (size_t)w * 3;
    uint8_t *raw = malloc(stride * h);
    for (unsigned y = 0; y < h; y++) {
        uint8_t *o = raw + y * stride;
        *o++ = 0; /* filter: none */
        for (unsigned x = 0; x < w; x++) {
            float Y = sample(0, x, y, ten_bit) - 16.0f;
            float U = sample(1, x / 2, y / 2, ten_bit) - 128.0f;
            float V = sample(2, x / 2, y / 2, ten_bit) - 128.0f;
            *o++ = clamp8(1.1644f * Y + 1.7927f * V);
            *o++ = clamp8(1.1644f * Y - 0.2132f * U - 0.5329f * V);
            *o++ = clamp8(1.1644f * Y + 2.1124f * U);
        }
    }
    uLongf zlen = compressBound(stride * h);
    uint8_t *z = malloc(zlen);
    compress2(z, &zlen, raw, stride * h, 6);

    char path[4096];
    snprintf(path, sizeof path, "%s/frame.png", outdir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(raw);
    printf("[video] frame %u saved: %s\n", vid.frames, path);
}

static void video_display(void *opaque, void *picture)
{
    (void)opaque; (void)picture;
    int64_t t = now_ns();
    if (!vid.frames)
        vid.first_ns = t;
    vid.last_ns = t;
    vid.frames++;
    if (!vid.saved && vid.frames == snapshot_frame) {
        save_png();
        vid.saved = 1;
    }
}

/* ---- audio ---- */

struct audio {
    FILE *wav;
    uint64_t samples; /* per channel */
    int64_t first_pts;
};
static struct audio aud;
static pthread_mutex_t aud_lock = PTHREAD_MUTEX_INITIALIZER;

static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint32_t byte_rate = SAMPLE_RATE * CHANNELS * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);           /* PCM */
    s = CHANNELS; memcpy(h + 22, &s, 2);
    v = SAMPLE_RATE; memcpy(h + 24, &v, 4);
    memcpy(h + 28, &byte_rate, 4);
    s = CHANNELS * 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static void audio_play(void *opaque, const void *samples, unsigned count, int64_t pts)
{
    (void)opaque;
    pthread_mutex_lock(&aud_lock);
    if (!aud.samples)
        aud.first_pts = pts;
    fwrite(samples, CHANNELS * 2, count, aud.wav);
    aud.samples += count;
    pthread_mutex_unlock(&aud_lock);
}

/* ---- main ---- */

static const char *track_kind(libvlc_track_type_t t)
{
    switch (t) {
    case libvlc_track_audio: return "audio";
    case libvlc_track_video: return "video";
    case libvlc_track_text: return "subtitle";
    default: return "other";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <media> <outdir> [snapshot-frame]\n", argv[0]);
        return 2;
    }
    /* The PS5 probe calls this once per file in one process. */
    memset(&vid, 0, sizeof(vid));
    memset(&aud, 0, sizeof(aud));
    outdir = argv[2];
    if (argc > 3)
        snapshot_frame = (unsigned)atoi(argv[3]);

    const char *vlc_args[] = {
        /* No --no-plugins-cache: a static build has no plugin cache, nor the option. */
        "--ignore-config", "--no-video-title-show",
        /* The player opens an audio output before our callbacks exist; amem and adummy
         * both have priority 0, so without this VLC logs "no suitable audio output". */
        "--aout=adummy",
        "--no-stats", "--verbose=0",
    };
    int64_t t0 = now_ns();
    libvlc_instance_t *vlc = libvlc_new(sizeof vlc_args / sizeof *vlc_args, vlc_args);
    if (!vlc) {
        fprintf(stderr, "libvlc_new failed: %s\n", libvlc_errmsg());
        return 1;
    }
    printf("[vlc] libvlc %s up in %.1f ms\n", libvlc_get_version(), (now_ns() - t0) / 1e6);

    libvlc_media_t *m = libvlc_media_new_path(vlc, argv[1]);
    libvlc_media_player_t *mp = libvlc_media_player_new_from_media(m);

    libvlc_video_set_callbacks(mp, video_lock, video_unlock, video_display, NULL);
    libvlc_video_set_format_callbacks(mp, video_format, video_cleanup);

    char path[4096];
    snprintf(path, sizeof path, "%s/audio.wav", outdir);
    aud.wav = fopen(path, "wb");
    if (!aud.wav) {
        perror(path);
        return 1;
    }
    wav_header(aud.wav, 0);
    libvlc_audio_set_callbacks(mp, audio_play, NULL, NULL, NULL, NULL, NULL);
    libvlc_audio_set_format(mp, "S16N", SAMPLE_RATE, CHANNELS);

    int64_t start = now_ns();
    libvlc_media_player_play(mp);
    for (;;) {
        libvlc_state_t st = libvlc_media_player_get_state(mp);
        if (st == libvlc_Ended || st == libvlc_Error)
            break;
        unsigned w, h;
        if (!vid.vis_w && libvlc_video_get_size(mp, 0, &w, &h) == 0 && w && h) {
            vid.vis_h = h;
            vid.vis_w = w;
            printf("[video] visible size %ux%u\n", w, h);
        }
        usleep(50 * 1000);
    }
    double wall = (now_ns() - start) / 1e9;
    libvlc_state_t final = libvlc_media_player_get_state(mp);

    libvlc_media_track_t **tracks;
    unsigned n = libvlc_media_tracks_get(m, &tracks);
    for (unsigned i = 0; i < n; i++) {
        libvlc_media_track_t *t = tracks[i];
        const char *codec = libvlc_media_get_codec_description(t->i_type, t->i_codec);
        if (t->i_type == libvlc_track_video)
            printf("[track] %s %.4s (%s) %ux%u\n", track_kind(t->i_type), (char *)&t->i_codec,
                   codec ? codec : "?", t->video->i_width, t->video->i_height);
        else if (t->i_type == libvlc_track_audio)
            printf("[track] %s %.4s (%s) %u ch %u Hz\n", track_kind(t->i_type), (char *)&t->i_codec,
                   codec ? codec : "?", t->audio->i_channels, t->audio->i_rate);
        else
            printf("[track] %s %.4s (%s) %s\n", track_kind(t->i_type), (char *)&t->i_codec,
                   codec ? codec : "?", t->psz_language ? t->psz_language : "");
    }
    libvlc_media_tracks_release(tracks, n);

    libvlc_media_player_stop(mp);
    libvlc_media_player_release(mp);
    libvlc_media_release(m);
    libvlc_release(vlc);

    long data_bytes = ftell(aud.wav) - 44;
    wav_header(aud.wav, (uint32_t)data_bytes);
    fclose(aud.wav);

    double vspan = vid.frames > 1 ? (vid.last_ns - vid.first_ns) / 1e9 : 0;
    printf("[result] state=%s wall=%.2f s\n", final == libvlc_Ended ? "ended" : "ERROR", wall);
    printf("[result] video: %u frames over %.2f s (%.2f fps shown), snapshot %s\n", vid.frames, vspan,
           vspan > 0 ? (vid.frames - 1) / vspan : 0.0, vid.saved ? "yes" : "NO");
    printf("[result] audio: %.2f s of S16 stereo 48 kHz -> %s/audio.wav\n",
           (double)aud.samples / SAMPLE_RATE, outdir);
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        printf("[result] cpu (process total so far): user %.2f s, sys %.2f s\n",
               ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
               ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
    return final == libvlc_Ended && vid.frames && vid.saved ? 0 : 1;
}
