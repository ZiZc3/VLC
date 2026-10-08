/*
 * VLC-PS5's media library.
 *
 * Media lives in the app's own folder (/app0/media on the console: copy files
 * to /data/homebrew/PPSA85300/media over FTP), plus any other root the title
 * can read. A worker thread reads each file's length and, for videos, takes a
 * frame from 10% in as its thumbnail, through a second libvlc player with the
 * vmem output scaled to 480 pixels wide; thumbnails are cached in cache/thumbs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "library.h"

#include <algorithm>
#include <errno.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <dirent.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>

#include <vlc/vlc.h>

#include "backends/imgui_impl_vulkan.h"
#include "gfx.h"
#include "lang.h"
#include "platform.h"
#include "player.h"

#define STB_IMAGE_IMPLEMENTATION
/* PNG for our own pictures; JPEG, BMP, GIF and TGA for photos. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include "stb_image.h"

namespace {

std::vector<MediaItem> items;
std::string media_dir, cache_dir, resume_path;
std::vector<std::string> roots;

struct Resume {
    int64_t ms, length, last;
};
std::vector<std::pair<std::string, Resume>> resumes;
std::vector<std::string> favourites;
std::string favourites_path, bookmarks_path;
std::vector<PlaylistFile> playlists;
std::vector<std::pair<std::string, int64_t>> bookmarks;
const char *const playlist_exts[] = { "m3u", "m3u8", "pls", nullptr };
/* opened like folders (VLC's archive module reads them) */
const char *const archive_exts[] = { "zip", "rar", "7z", nullptr };
/* read in the text viewer */
const char *const text_exts[] = { "txt", "nfo", "diz", "md", "log", nullptr };
/* what stb_image reads (a GIF shows its first picture) */
const char *const image_exts[] = { "jpg", "jpeg", "png", "bmp", "gif", "tga", nullptr };

/* File details, filled by their own thread. */
std::mutex details_lock;
std::string details_path;
MediaDetails details, details_copy;

/* Worker -> main thread. */
struct Finished {
    std::string path;
    std::string title, artist, album; /* a song's tags */
    int64_t length_ms;
    uint32_t w, h;
    std::vector<uint8_t> bgra;
};
std::mutex done_lock;
std::vector<Finished> done;

std::mutex work_lock;
std::condition_variable work_cv;
struct Job {
    std::string path, thumb_file;
    bool audio, image;
};
std::vector<Job> work;           /* files still to look at (copies: no shared items) */
std::atomic<bool> running{ false }, busy{ false };
std::thread worker;

VkSampler thumb_sampler;
struct Texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
};
std::vector<Texture> textures;

const char *const video_exts[] = { "mkv", "mp4", "m4v", "avi", "mov", "wmv", "webm", "ts", "m2ts",
                                   "mts", "mpg", "mpeg", "vob", "flv", "3gp", "ogv", "divx",
                                   "rmvb", "asf", nullptr };
const char *const audio_exts[] = { "mp3", "flac", "m4a", "aac", "ogg", "opus", "wav", "wma",
                                   "ac3", "dts", "mka", "ape", "alac", "aiff", nullptr };

bool has_ext(const char *ext, const char *const *list)
{
    for (; *list; list++)
        if (!strcasecmp(ext, *list))
            return true;
    return false;
}

std::string tidy_name(const std::string &file)
{
    std::string n = file.substr(0, file.rfind('.'));
    for (char &c : n)
        if (c == '_' || c == '.')
            c = ' ';
    while (!n.empty() && n.back() == ' ')
        n.pop_back();
    return n.empty() ? file : n;
}

bool has_file(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

/* What a scan passed over, for the log: "(folder too deep) 1". */
std::map<std::string, int> skipped;
int archives_found, others_found;

/* Folders a drive keeps for the system, never media. */
bool system_folder(const char *name)
{
    return !strcmp(name, "System Volume Information") || !strcmp(name, "$RECYCLE.BIN") ||
           !strcmp(name, "lost+found") || !strcmp(name, "sce_sys") || !strcmp(name, "sce_module");
}

void scan_dir(const std::string &dir, int depth)
{
    /* A disc copied as a folder (VIDEO_TS, BDMV): one item, not its files. */
    bool dvd = has_file(dir + "/VIDEO_TS/VIDEO_TS.IFO") || has_file(dir + "/video_ts/video_ts.ifo");
    bool bd = has_file(dir + "/BDMV/index.bdmv") || has_file(dir + "/bdmv/index.bdmv");
    if ((dvd || bd) && depth > 0) {
        MediaItem m = {};
        m.path = dir;
        m.name = tidy_name(dir.substr(dir.rfind('/') + 1) + ".x");
        m.ext = dvd ? "DVD" : "BLU-RAY";
        m.folder = dir.substr(0, dir.rfind('/'));
        m.disc = true;
        struct stat st;
        if (stat(dir.c_str(), &st) == 0)
            m.mtime = st.st_mtime;
        items.push_back(m);
        return;
    }
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            skipped["(can't read)"]++;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (system_folder(e->d_name))
                continue;
            if (depth < 4)
                scan_dir(path, depth + 1);
            else
                skipped["(folder too deep)"]++;
            continue;
        }
        const char *dot = strrchr(e->d_name, '.');
        if (!dot)
            dot = e->d_name + strlen(e->d_name); /* no extension: "" */
        if (!strcasecmp(dot, ".part"))
            continue; /* a file still arriving from the phone page */
        if (*dot && has_ext(dot + 1, playlist_exts)) {
            playlists.push_back({ path, tidy_name(e->d_name), dir });
            continue;
        }
        const char *x = *dot ? dot + 1 : dot;
        bool video = has_ext(x, video_exts), audio = has_ext(x, audio_exts);
        bool image = has_ext(x, image_exts), text = has_ext(x, text_exts);
        bool archive = has_ext(x, archive_exts), iso = !strcasecmp(x, "iso");
        if (image) {
            /* an album's cover file isn't a photo of its own */
            std::string base = std::string(e->d_name, dot - e->d_name);
            for (char &c : base)
                c = (char)tolower((unsigned char)c);
            if (base == "cover" || base == "folder" || base == "front" || base.compare(0, 8, "albumart") == 0)
                continue;
        }
        /* Everything else shows in Browse too: what VLC can't play says so. */
        bool other = !video && !audio && !image && !text && !archive && !iso;
        archives_found += archive;
        others_found += other;
        MediaItem m = {};
        m.path = path;
        m.name = *dot ? tidy_name(e->d_name) : tidy_name(std::string(e->d_name) + ".x");
        m.ext = x;
        for (char &c : m.ext)
            c = (char)toupper((unsigned char)c);
        m.folder = dir;
        m.audio = audio;
        m.image = image;
        m.text = text;
        m.archive = archive;
        m.other = other;
        m.disc = iso;
        m.size = st.st_size;
        m.mtime = st.st_mtime;
        items.push_back(m);
    }
    closedir(d);
}

uint64_t fnv(const std::string &s, uint64_t h = 1469598103934665603ULL)
{
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ULL;
    return h;
}

std::string thumb_file(const MediaItem &m)
{
    char name[64];
    snprintf(name, sizeof(name), m.audio ? "/%016llx.song.thumb" : "/%016llx.thumb",
             (unsigned long long)fnv(m.path + "|" + std::to_string(m.size) + "|" +
                                     std::to_string(m.mtime)));
    return cache_dir + name;
}

/* .thumb: "VTH1", u32 w, u32 h, i64 length_ms, then w*h BGRA (w = h = 0: audio). */
bool read_thumb(const std::string &file, Finished &out)
{
    FILE *f = fopen(file.c_str(), "rb");
    if (!f)
        return false;
    char magic[4];
    bool ok = fread(magic, 1, 4, f) == 4 && (!memcmp(magic, "VTH1", 4) || !memcmp(magic, "VTH2", 4)) &&
              fread(&out.w, 4, 1, f) == 1 && fread(&out.h, 4, 1, f) == 1 &&
              fread(&out.length_ms, 8, 1, f) == 1 && out.w <= 1024 && out.h <= 1024;
    /* VTH2: then the tags, each a u16 length and its bytes */
    if (ok && !memcmp(magic, "VTH2", 4))
        for (std::string *str : { &out.title, &out.artist, &out.album }) {
            uint16_t len = 0;
            ok = ok && fread(&len, 2, 1, f) == 1;
            if (ok && len) {
                str->resize(len);
                ok = fread(&(*str)[0], 1, len, f) == len;
            }
        }
    if (ok) {
        out.bgra.resize((size_t)out.w * out.h * 4);
        ok = out.bgra.empty() || fread(out.bgra.data(), 1, out.bgra.size(), f) == out.bgra.size();
    }
    fclose(f);
    return ok;
}

void write_thumb(const std::string &file, const Finished &t)
{
    FILE *f = fopen(file.c_str(), "wb");
    if (!f)
        return;
    fwrite("VTH2", 1, 4, f);
    fwrite(&t.w, 4, 1, f);
    fwrite(&t.h, 4, 1, f);
    fwrite(&t.length_ms, 8, 1, f);
    for (const std::string *str : { &t.title, &t.artist, &t.album }) {
        uint16_t len = (uint16_t)std::min<size_t>(str->size(), 1000);
        fwrite(&len, 2, 1, f);
        fwrite(str->data(), 1, len, f);
    }
    if (!t.bgra.empty())
        fwrite(t.bgra.data(), 1, t.bgra.size(), f);
    fclose(f);
}

/* ---- the thumbnailer --------------------------------------------------------- */

struct Grab {
    std::mutex lock;
    std::condition_variable cv;
    uint32_t w, h, pitch;
    std::vector<uint8_t> pixels;
    int frames;
    bool got;
};

unsigned grab_setup(void **opaque, char *chroma, unsigned *width, unsigned *height,
                    unsigned *pitches, unsigned *lines)
{
    Grab *g = (Grab *)*opaque;
    uint32_t w = 480, h = *width ? (uint32_t)(480.0 * *height / *width) & ~1u : 270;
    if (h < 2 || h > 1024)
        h = 270;
    memcpy(chroma, "RV32", 4);
    *width = w;
    *height = h;
    pitches[0] = w * 4;
    lines[0] = h;
    g->w = w;
    g->h = h;
    g->pitch = w * 4;
    g->pixels.assign((size_t)w * h * 4, 0);
    return 1;
}

void *grab_lock(void *opaque, void **planes)
{
    Grab *g = (Grab *)opaque;
    planes[0] = g->pixels.data();
    return nullptr;
}

void grab_display(void *opaque, void *picture)
{
    (void)picture;
    Grab *g = (Grab *)opaque;
    std::lock_guard<std::mutex> l(g->lock);
    /* The third picture: past any black first frame after the seek. */
    if (++g->frames == 3) {
        g->got = true;
        g->cv.notify_all();
    }
}

/* ---- covers: the picture a song carries, or its folder's ---- */

uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t syncsafe(const unsigned char *p)
{
    return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 | (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

/* ID3v2 (MP3): the first APIC (v2.3/2.4) or PIC (v2.2) frame. */
bool id3_cover(FILE *f, std::string *out)
{
    unsigned char h[10];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3))
        return false;
    int ver = h[3];
    uint32_t size = syncsafe(h + 6);
    if (size > (32u << 20))
        return false;
    std::string tag(size, '\0');
    if (fread(&tag[0], 1, size, f) != size)
        return false;
    const unsigned char *t = (const unsigned char *)tag.data();
    size_t pos = 0;
    if (h[5] & 0x40 && ver >= 3) /* an extended header */
        pos += ver == 4 ? syncsafe(t) : be32(t) + 4;
    while (pos + (ver == 2 ? 6 : 10) <= size) {
        const unsigned char *fr = t + pos;
        if (!fr[0])
            break;
        size_t hlen = ver == 2 ? 6 : 10;
        size_t flen = ver == 2 ? ((size_t)fr[3] << 16 | (size_t)fr[4] << 8 | fr[5]) : ver == 4 ? syncsafe(fr + 4) : be32(fr + 4);
        if (pos + hlen + flen > size)
            break;
        const unsigned char *d = fr + hlen;
        bool pic = ver == 2 ? !memcmp(fr, "PIC", 3) : !memcmp(fr, "APIC", 4);
        if (pic && flen > 4) {
            int enc = d[0];
            size_t p = 1;
            if (ver == 2)
                p += 3; /* "JPG" / "PNG" */
            else
                while (p < flen && d[p])
                    p++; /* the MIME type */
            if (ver != 2)
                p++;
            p++; /* the picture type */
            /* the description: ends with one zero byte (two in UTF-16) */
            if (enc == 1 || enc == 2) {
                while (p + 1 < flen && (d[p] || d[p + 1]))
                    p += 2;
                p += 2;
            } else {
                while (p < flen && d[p])
                    p++;
                p++;
            }
            if (p < flen) {
                out->assign((const char *)d + p, flen - p);
                return true;
            }
        }
        pos += hlen + flen;
    }
    return false;
}

/* FLAC: the PICTURE metadata block. */
bool flac_cover(FILE *f, std::string *out)
{
    unsigned char h[4];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4))
        return false;
    for (int i = 0; i < 64; i++) {
        if (fread(h, 1, 4, f) != 4)
            return false;
        bool last = h[0] & 0x80;
        uint32_t len = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
        if ((h[0] & 0x7f) == 6 && len < (32u << 20)) {
            std::string b(len, '\0');
            if (fread(&b[0], 1, len, f) != len)
                return false;
            const unsigned char *p = (const unsigned char *)b.data();
            size_t at = 4;
            if (at + 4 > len)
                return false;
            at += 4 + be32(p + at); /* MIME */
            if (at + 4 > len)
                return false;
            at += 4 + be32(p + at); /* description */
            at += 16;              /* width, height, depth, colours */
            if (at + 4 > len)
                return false;
            uint32_t dlen = be32(p + at);
            at += 4;
            if (at + dlen > len)
                return false;
            out->assign((const char *)p + at, dlen);
            return true;
        }
        if (last || fseek(f, len, SEEK_CUR))
            return false;
    }
    return false;
}

/* MP4 / M4A: moov > udta > meta > ilst > covr > data. */
bool mp4_find(FILE *f, long start, long end, const char *const *path, std::string *out)
{
    long at = start;
    while (at + 8 <= end) {
        unsigned char h[16];
        if (fseek(f, at, SEEK_SET) || fread(h, 1, 8, f) != 8)
            return false;
        uint64_t size = be32(h);
        long hdr = 8;
        if (size == 1) {
            if (fread(h + 8, 1, 8, f) != 8)
                return false;
            size = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(end - at);
        }
        if (size < (uint64_t)hdr || at + (long)size > end)
            return false;
        if (!memcmp(h + 4, *path, 4)) {
            long body = at + hdr, body_end = at + (long)size;
            if (!memcmp(*path, "meta", 4))
                body += 4; /* version and flags */
            if (!path[1]) {
                /* "data": 8 bytes of type and locale, then the picture */
                long n = body_end - body - 8;
                if (n <= 0 || n > (32l << 20) || fseek(f, body + 8, SEEK_SET))
                    return false;
                out->resize((size_t)n);
                return fread(&(*out)[0], 1, (size_t)n, f) == (size_t)n;
            }
            return mp4_find(f, body, body_end, path + 1, out);
        }
        at += (long)size;
    }
    return false;
}

bool embedded_cover(const std::string &path, std::string *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    bool ok = id3_cover(f, out) || flac_cover(f, out);
    if (!ok) {
        static const char *const covr[] = { "moov", "udta", "meta", "ilst", "covr", "data", nullptr };
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        ok = mp4_find(f, 0, end, covr, out);
    }
    fclose(f);
    return ok && out->size() > 64;
}

/* cover.jpg, folder.png, front.jpg, AlbumArt...jpg next to the songs */
std::string folder_cover(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return "";
    std::string best;
    int best_rank = 99;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        std::string low = n;
        for (char &c : low)
            c = (char)tolower((unsigned char)c);
        size_t dot = low.rfind('.');
        if (dot == std::string::npos)
            continue;
        std::string ext = low.substr(dot + 1), base = low.substr(0, dot);
        if (ext != "jpg" && ext != "jpeg" && ext != "png")
            continue;
        int rank = base == "cover" ? 0 : base == "folder" ? 1 : base == "front" ? 2
                 : base.compare(0, 8, "albumart") == 0 ? 3 : 99;
        if (rank < best_rank) {
            best_rank = rank;
            best = dir + "/" + n;
        }
    }
    closedir(d);
    return best;
}

/* A picture file made to fit max_w x max_h (averaging whole blocks of pixels:
 * clean and quick), as BGRA. */
bool decode_fit(const std::string &path, int max_w, int max_h, Finished *out, const std::string *bytes = nullptr)
{
    int w, h, n;
    unsigned char *rgba = bytes ? stbi_load_from_memory((const unsigned char *)bytes->data(), (int)bytes->size(), &w, &h, &n, 4)
                                : stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }
    int k = 1;
    while (w / k > max_w || h / k > max_h)
        k++;
    int ow = std::max(1, w / k), oh = std::max(1, h / k);
    out->w = (uint32_t)ow;
    out->h = (uint32_t)oh;
    out->bgra.resize((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            unsigned sum[4] = { 0, 0, 0, 0 };
            for (int dy = 0; dy < k; dy++) {
                const unsigned char *px = rgba + ((size_t)(y * k + dy) * w + (size_t)x * k) * 4;
                for (int dx = 0; dx < k; dx++, px += 4)
                    for (int c = 0; c < 4; c++)
                        sum[c] += px[c];
            }
            unsigned char *o = &out->bgra[((size_t)y * ow + x) * 4];
            unsigned kk = (unsigned)(k * k);
            o[0] = (unsigned char)(sum[2] / kk); /* RGBA -> BGRA */
            o[1] = (unsigned char)(sum[1] / kk);
            o[2] = (unsigned char)(sum[0] / kk);
            o[3] = 255;
        }
    stbi_image_free(rgba);
    return true;
}

/* The photo being decoded for the viewer: one at a time, the newest asked wins. */
std::mutex photo_lock;
std::string photo_want;    /* the path asked for */
std::string photo_ready_for;
bool photo_failed;
Finished photo;            /* the picture, when photo_ready_for == photo_want */
bool photo_running;

void photo_worker()
{
    for (;;) {
        std::string path;
        {
            std::lock_guard<std::mutex> g(photo_lock);
            if (photo_want.empty() || photo_want == photo_ready_for) {
                photo_running = false;
                return;
            }
            path = photo_want;
        }
        Finished f;
        bool ok = decode_fit(path, 3840, 2160, &f);
        std::lock_guard<std::mutex> g(photo_lock);
        photo_ready_for = path;
        photo_failed = !ok;
        photo = std::move(f);
    }
}

void look_at(const Job &job)
{
    const std::string &path = job.path;
    libvlc_instance_t *vlc = player_vlc();
    Finished out;
    out.path = path;
    out.length_ms = 0;
    out.w = out.h = 0;
    if (job.image) {
        /* a photo: its own picture, made small */
        if (!decode_fit(path, 480, 270, &out))
            out.w = out.h = 0;
        else
            write_thumb(job.thumb_file, out);
        std::lock_guard<std::mutex> l(done_lock);
        done.push_back(std::move(out));
        return;
    }
    libvlc_media_t *m = libvlc_media_new_path(vlc, path.c_str());
    if (!m)
        return;
    libvlc_media_parse_with_options(m, libvlc_media_parse_local, 4000);
    for (int i = 0; i < 100 && libvlc_media_get_parsed_status(m) == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    out.length_ms = libvlc_media_get_duration(m);
    if (job.audio) {
        /* a song: its tags, and its cover (in the file, or the folder's) */
        char *t = libvlc_media_get_meta(m, libvlc_meta_Title), *ar = libvlc_media_get_meta(m, libvlc_meta_Artist),
             *al = libvlc_media_get_meta(m, libvlc_meta_Album);
        out.title = t ? t : "";
        out.artist = ar ? ar : "";
        out.album = al ? al : "";
        free(t);
        free(ar);
        free(al);
        /* VLC names a song without a title tag after its file */
        std::string file = path.substr(path.rfind('/') + 1);
        if (out.title == file)
            out.title.clear();
        std::string cover;
        if (embedded_cover(path, &cover))
            decode_fit(path, 480, 480, &out, &cover);
        else if (!(cover = folder_cover(path.substr(0, path.rfind('/')))).empty())
            decode_fit(cover, 480, 480, &out);
    }
    if (out.length_ms < 0)
        out.length_ms = 0;
    bool video = false;
    libvlc_media_track_t **tracks;
    unsigned n = libvlc_media_tracks_get(m, &tracks);
    for (unsigned i = 0; i < n; i++)
        if (tracks[i]->i_type == libvlc_track_video)
            video = true;
    libvlc_media_tracks_release(tracks, n);
    bool audio_ext = job.audio;

    if (video && !audio_ext) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":start-time=%.3f",
                 out.length_ms > 20000 ? out.length_ms * 0.1 / 1000.0 : 0.0);
        libvlc_media_add_option(m, opt);
        libvlc_media_add_option(m, ":no-audio");
        libvlc_media_add_option(m, ":no-spu");
        libvlc_media_add_option(m, ":avcodec-threads=2");
        Grab g;
        g.frames = 0;
        g.got = false;
        libvlc_media_player_t *p = libvlc_media_player_new_from_media(m);
        libvlc_video_set_callbacks(p, grab_lock, nullptr, grab_display, &g);
        libvlc_video_set_format_callbacks(p, grab_setup, nullptr);
        libvlc_media_player_play(p);
        {
            std::unique_lock<std::mutex> l(g.lock);
            g.cv.wait_for(l, std::chrono::seconds(6), [&] { return g.got; });
        }
        libvlc_media_player_stop(p);
        libvlc_media_player_release(p);
        if (g.got) {
            out.w = g.w;
            out.h = g.h;
            out.bgra = std::move(g.pixels);
            for (size_t i = 3; i < out.bgra.size(); i += 4)
                out.bgra[i] = 255; /* RV32's fourth byte is padding */
        }
    }
    libvlc_media_release(m);
    if (out.length_ms > 0 || out.w || job.audio)
        write_thumb(job.thumb_file, out);
    std::lock_guard<std::mutex> l(done_lock);
    done.push_back(std::move(out));
}

void worker_main()
{
    while (running) {
        Job job;
        {
            std::unique_lock<std::mutex> l(work_lock);
            work_cv.wait_for(l, std::chrono::milliseconds(250),
                             [] { return !running || (!work.empty() && !busy); });
            if (!running)
                break;
            if (work.empty() || busy)
                continue;
            job = work.front();
            work.erase(work.begin());
        }
        look_at(job);
    }
}

/* ---- resume points --------------------------------------------------------------- */

void load_resumes()
{
    resumes.clear();
    FILE *f = fopen(resume_path.c_str(), "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        long long ms, len, last;
        int off = 0;
        if (sscanf(line, "%lld\t%lld\t%lld\t%n", &ms, &len, &last, &off) == 3 && off > 0) {
            std::string path = line + off;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            resumes.push_back({ path, { ms, len, last } });
        }
    }
    fclose(f);
}

void save_resumes()
{
    std::string tmp = resume_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    for (auto &r : resumes)
        fprintf(f, "%lld\t%lld\t%lld\t%s\n", (long long)r.second.ms, (long long)r.second.length,
                (long long)r.second.last, r.first.c_str());
    fclose(f);
    rename(tmp.c_str(), resume_path.c_str());
}

void load_favourites()
{
    favourites.clear();
    FILE *f = fopen(favourites_path.c_str(), "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string path = line;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        if (!path.empty())
            favourites.push_back(path);
    }
    fclose(f);
}

void save_favourites()
{
    std::string tmp = favourites_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    for (const std::string &p : favourites)
        fprintf(f, "%s\n", p.c_str());
    fclose(f);
    rename(tmp.c_str(), favourites_path.c_str());
}

std::string human_size(int64_t bytes)
{
    char b[32];
    if (bytes >= (1LL << 30))
        snprintf(b, sizeof(b), "%.2f GB", bytes / 1073741824.0);
    else
        snprintf(b, sizeof(b), "%.1f MB", bytes / 1048576.0);
    return b;
}

std::string fourcc_name(int type, uint32_t codec)
{
    const char *d = libvlc_media_get_codec_description((libvlc_track_type_t)type, codec);
    if (d && *d)
        return d;
    char f[5] = { (char)codec, (char)(codec >> 8), (char)(codec >> 16), (char)(codec >> 24), 0 };
    return f;
}

void details_main(std::string path, int64_t size, std::string ext)
{
    MediaDetails d;
    d.rows.push_back({ "File", path.substr(path.rfind('/') + 1) });
    d.rows.push_back({ "Folder", path.substr(0, path.rfind('/')) });
    d.rows.push_back({ "Size", human_size(size) });
    libvlc_media_t *m = libvlc_media_new_path(player_vlc(), path.c_str());
    if (m) {
        libvlc_media_parse_with_options(m, libvlc_media_parse_local, 5000);
        for (int i = 0; i < 120 && libvlc_media_get_parsed_status(m) == 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int64_t len = libvlc_media_get_duration(m);
        if (len > 0) {
            d.rows.push_back({ "Length", format_time(len) });
            char br[32];
            snprintf(br, sizeof(br), "%.1f Mbit/s", size * 8.0 / (len / 1000.0) / 1e6);
            d.rows.push_back({ "Bitrate", br });
        }
        d.rows.push_back({ "Container", ext });
        libvlc_media_track_t **t;
        unsigned n = libvlc_media_tracks_get(m, &t);
        int audio_no = 0, sub_no = 0;
        std::string subs;
        for (unsigned i = 0; i < n; i++) {
            char line[256];
            std::string codec = fourcc_name(t[i]->i_type, t[i]->i_codec);
            std::string lang = t[i]->psz_language ? t[i]->psz_language : "";
            if (t[i]->i_type == libvlc_track_video) {
                const libvlc_video_track_t *v = t[i]->video;
                float fps = v->i_frame_rate_den ? (float)v->i_frame_rate_num / v->i_frame_rate_den : 0;
                snprintf(line, sizeof(line), "%s, %ux%u", codec.c_str(), v->i_width, v->i_height);
                std::string val = line;
                if (fps > 0) {
                    char f[32];
                    snprintf(f, sizeof(f), ", %.3g fps", fps);
                    val += f;
                }
                if (t[i]->i_bitrate > 0) {
                    char b[32];
                    snprintf(b, sizeof(b), ", %.1f Mbit/s", t[i]->i_bitrate / 1e6);
                    val += b;
                }
                d.rows.push_back({ "Video", val });
            } else if (t[i]->i_type == libvlc_track_audio) {
                const libvlc_audio_track_t *a = t[i]->audio;
                const char *ch = a->i_channels == 1 ? "mono" : a->i_channels == 2 ? "stereo"
                               : a->i_channels == 6 ? "5.1" : a->i_channels == 8 ? "7.1" : "";
                snprintf(line, sizeof(line), "%s, %s%s%u Hz", codec.c_str(), ch, *ch ? ", " : "", a->i_rate);
                std::string val = line;
                if (!lang.empty())
                    val += " (" + lang + ")";
                char label[32];
                snprintf(label, sizeof(label), tr("Audio %d"), ++audio_no);
                d.rows.push_back({ label, val });
            } else if (t[i]->i_type == libvlc_track_text) {
                sub_no++;
                if (!subs.empty())
                    subs += ", ";
                subs += lang.empty() ? codec : lang;
            }
        }
        if (sub_no)
            d.rows.push_back({ "Subtitles", subs });
        libvlc_media_tracks_release(t, n);
        libvlc_media_release(m);
    }
    d.ready = true;
    std::lock_guard<std::mutex> l(details_lock);
    if (details_path == path)
        details = d;
}

void apply_resumes()
{
    for (MediaItem &m : items)
        for (auto &r : resumes)
            if (r.first == m.path) {
                m.resume_ms = r.second.ms;
                m.last_played = r.second.last;
                if (!m.length_ms)
                    m.length_ms = r.second.length;
            }
}

/* ---- textures ----------------------------------------------------------------------- */

/* The next smaller level of a BGRA picture: each pixel the mean of 2x2. */
std::vector<uint8_t> half_size(const std::vector<uint8_t> &src, uint32_t w, uint32_t h, uint32_t *nw, uint32_t *nh)
{
    *nw = std::max(1u, w / 2);
    *nh = std::max(1u, h / 2);
    std::vector<uint8_t> out((size_t)*nw * *nh * 4);
    for (uint32_t y = 0; y < *nh; y++) {
        const uint8_t *r0 = &src[(size_t)std::min(2 * y, h - 1) * w * 4];
        const uint8_t *r1 = &src[(size_t)std::min(2 * y + 1, h - 1) * w * 4];
        uint8_t *o = &out[(size_t)y * *nw * 4];
        for (uint32_t x = 0; x < *nw; x++) {
            uint32_t x0 = std::min(2 * x, w - 1) * 4, x1 = std::min(2 * x + 1, w - 1) * 4;
            for (int c = 0; c < 4; c++)
                o[x * 4 + c] = (uint8_t)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
        }
    }
    return out;
}

/* A picture as a texture. Thumbnails (and covers) get every smaller level
 * too: drawn small (a playlist row, a Music cover) a 480-wide picture
 * sampled once a pixel shimmered; the levels make it smooth at any size.
 * Big photos are drawn near their size and keep one level. */
ImTextureID make_texture(const Finished &t)
{
    if (!thumb_sampler) {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.minLod = 0;
        si.maxLod = VK_LOD_CLAMP_NONE;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(gfx.device, &si, nullptr, &thumb_sampler);
    }
    /* the levels, one after another in the staging buffer */
    std::vector<std::vector<uint8_t>> made;
    made.reserve(12);
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize total = t.bgra.size();
    regions.push_back({});
    regions[0].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    regions[0].imageExtent = { t.w, t.h, 1 };
    if (t.w <= 1024 && t.h <= 1024) {
        uint32_t w = t.w, h = t.h;
        const std::vector<uint8_t> *src = &t.bgra;
        while ((w > 1 || h > 1) && regions.size() < 12) {
            uint32_t nw, nh;
            made.push_back(half_size(*src, w, h, &nw, &nh));
            src = &made.back();
            w = nw;
            h = nh;
            VkBufferImageCopy r = {};
            r.bufferOffset = total;
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)regions.size(), 0, 1 };
            r.imageExtent = { w, h, 1 };
            regions.push_back(r);
            total += made.back().size();
        }
    }
    Texture tex;
    if (!gfx_image(t.w, t.h, VK_FORMAT_B8G8R8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex.image,
                   &tex.memory, &tex.view, (uint32_t)regions.size()))
        return 0;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *mapped;
    if (!gfx_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &staging, &staging_mem, &mapped))
        return 0;
    memcpy(mapped, t.bgra.data(), t.bgra.size());
    for (size_t i = 0; i < made.size(); i++)
        memcpy((uint8_t *)mapped + regions[i + 1].bufferOffset, made[i].data(), made[i].size());
    VkCommandBuffer cmd = gfx_one_shot_begin();
    gfx_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    gfx_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    gfx_one_shot_end(cmd);
    vkDestroyBuffer(gfx.device, staging, nullptr);
    vkFreeMemory(gfx.device, staging_mem, nullptr);
    tex.set = ImGui_ImplVulkan_AddTexture(thumb_sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    textures.push_back(tex);
    return (ImTextureID)tex.set;
}

/* What is already known about a file (by its thumbnail file name, which covers
 * path, size and date): rescans reuse textures instead of making new ones. */
struct Known {
    ImTextureID thumb;
    float aspect;
    int64_t length_ms;
    std::string title, artist, album;
};
std::map<std::string, Known> known;
int generation;

void apply(const Finished &t)
{
    for (MediaItem &m : items) {
        if (m.path != t.path)
            continue;
        Known &k = known[thumb_file(m)];
        if (t.length_ms > 0)
            k.length_ms = m.length_ms = t.length_ms;
        if (!t.title.empty() || !t.artist.empty() || !t.album.empty()) {
            k.title = t.title;
            k.artist = t.artist;
            k.album = t.album;
        }
        if (!k.title.empty())
            m.name = k.title;
        m.artist = k.artist;
        m.album = k.album;
        if (t.w && t.h && !t.bgra.empty() && !k.thumb) {
            k.thumb = make_texture(t);
            k.aspect = (float)t.w / t.h;
        }
        m.thumb = k.thumb;
        m.thumb_aspect = k.aspect;
    }
}

/* The readable roots and the dates of their top two levels: when this changes
 * (a USB drive plugged in or out, files copied in), the library rescans. */
std::string watch_signature()
{
    std::vector<std::string> cands = { media_dir };
    for (const char *const *r = plat_media_roots(); *r; r++)
        cands.push_back(*r);
    std::string sig, drives;
    for (const std::string &c : cands) {
        DIR *d = opendir(c.c_str());
        if (!d) {
            if (c.compare(0, 5, "/mnt/") == 0)
                drives += " " + c.substr(5) + "=-";
            continue;
        }
        struct stat st;
        if (stat(c.c_str(), &st) == 0)
            sig += c + ":" + std::to_string((long long)st.st_mtime) + ";";
        struct dirent *e;
        int entries = 0;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            entries++;
            std::string p = c + "/" + e->d_name;
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                sig += e->d_name + std::string(":") + std::to_string((long long)st.st_mtime) + ";";
        }
        closedir(d);
        /* A drive with only files at its top shows up too. */
        sig += c + "#" + std::to_string(entries) + ";";
        if (c.compare(0, 5, "/mnt/") == 0)
            drives += " " + c.substr(5) + "=" + std::to_string(entries);
    }
    /* What the drive slots look like, whenever that changes ("-": can't open). */
    static std::string last_drives;
    if (drives != last_drives) {
        fprintf(stderr, "library: drives%s\n", drives.c_str());
        last_drives = drives;
    }
    return sig;
}
std::string last_signature;
double next_watch;

void scan()
{
    items.clear();
    playlists.clear();
    roots.clear();
    skipped.clear();
    archives_found = others_found = 0;
    roots.push_back(media_dir);
    scan_dir(media_dir, 0);
    for (const char *const *r = plat_media_roots(); *r; r++) {
        /* A mount point with nothing in it is no drive (the console keeps
         * empty /mnt/usbN folders). */
        DIR *d = opendir(*r);
        if (!d)
            continue;
        bool has_entries = false;
        struct dirent *e;
        while (!has_entries && (e = readdir(d)))
            has_entries = e->d_name[0] != '.';
        closedir(d);
        /* /data/vlc shows even empty (only reachable with full access): it says
         * where files can go. */
        if (!has_entries && strcmp(*r, "/data/vlc") != 0)
            continue;
        roots.push_back(*r);
        scan_dir(*r, 0);
    }
    std::sort(items.begin(), items.end(), [](const MediaItem &a, const MediaItem &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    apply_resumes();
    for (MediaItem &m : items)
        m.favourite = std::find(favourites.begin(), favourites.end(), m.path) != favourites.end();
    std::vector<Job> todo;
    for (MediaItem &m : items) {
        if (m.text || m.archive || m.disc || m.other)
            continue; /* no picture to make (a disc's menu is its picture) */
        auto k = known.find(thumb_file(m));
        if (k != known.end()) {
            m.thumb = k->second.thumb;
            m.thumb_aspect = k->second.aspect;
            if (k->second.length_ms > 0)
                m.length_ms = k->second.length_ms;
            if (!k->second.title.empty())
                m.name = k->second.title;
            m.artist = k->second.artist;
            m.album = k->second.album;
            if (m.thumb || m.audio)
                continue;
        }
        Finished t;
        if (read_thumb(thumb_file(m), t)) {
            t.path = m.path;
            apply(t);
        } else {
            todo.push_back({ m.path, thumb_file(m), m.audio, m.image });
        }
    }
    {
        std::lock_guard<std::mutex> l(work_lock);
        work = todo;
    }
    work_cv.notify_all();
    generation++;
    last_signature = watch_signature();
    fprintf(stderr, "library: %zu files in %zu folders, %zu to look at\n", items.size(),
            roots.size(), todo.size());
    std::string passed;
    for (auto &k : skipped)
        passed += (passed.empty() ? "" : ", ") + k.first + " " + std::to_string(k.second);
    fprintf(stderr, "library: %d archives, %d other files; passed over: %s\n", archives_found,
            others_found, passed.empty() ? "nothing" : passed.c_str());
}

} // namespace

void library_init()
{
    std::string base = plat_data_dir();
    media_dir = base + "/media";
    cache_dir = base + "/cache";
    mkdir(media_dir.c_str(), 0777);
    mkdir(cache_dir.c_str(), 0777);
    cache_dir += "/thumbs";
    mkdir(cache_dir.c_str(), 0777);
    resume_path = base + "/cache/resume.txt";
    favourites_path = base + "/cache/favourites.txt";
    bookmarks_path = base + "/cache/bookmarks.txt";
    if (FILE *f = fopen(bookmarks_path.c_str(), "r")) {
        char line[2048];
        long long ms;
        int off;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%lld\t%n", &ms, &off) == 1 && off > 0) {
                std::string p = line + off;
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                    p.pop_back();
                bookmarks.push_back({ p, ms });
            }
        fclose(f);
    }
    load_resumes();
    load_favourites();
    running = true;
    worker = std::thread(worker_main);
    scan();
}

void library_shutdown()
{
    running = false;
    work_cv.notify_all();
    if (worker.joinable())
        worker.join();
}

void library_rescan()
{
    scan();
}

std::vector<MediaItem> &library_items()
{
    return items;
}

std::vector<std::string> library_roots()
{
    return roots;
}

const std::string &library_media_dir()
{
    return media_dir;
}

void library_update()
{
    std::vector<Finished> ready;
    {
        std::lock_guard<std::mutex> l(done_lock);
        ready.swap(done);
    }
    for (const Finished &t : ready)
        apply(t);
    /* Every 2 s while nothing plays (a rescan rebuilds the lists a playlist
     * points into): new drives and new files show up by themselves. */
    double now = plat_time();
    if (!busy && now >= next_watch) {
        next_watch = now + 2.0;
        if (watch_signature() != last_signature) {
            fprintf(stderr, "library: drives or folders changed, rescanning\n");
            scan();
        }
    }
}

int library_generation()
{
    return generation;
}

ImTextureID library_load_image(const std::string &path, float *aspect)
{
    int w, h, n;
    unsigned char *rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba) {
        fprintf(stderr, "library: can't load %s\n", path.c_str());
        return 0;
    }
    Finished t;
    t.w = (uint32_t)w;
    t.h = (uint32_t)h;
    t.length_ms = 0;
    t.bgra.assign(rgba, rgba + (size_t)w * h * 4);
    stbi_image_free(rgba);
    for (size_t i = 0; i < t.bgra.size(); i += 4)
        std::swap(t.bgra[i], t.bgra[i + 2]); /* RGBA -> BGRA */
    if (aspect)
        *aspect = (float)w / h;
    return make_texture(t);
}

ImTextureID library_photo(const std::string &path, float *aspect, bool *failed)
{
    *failed = false;
    std::lock_guard<std::mutex> g(photo_lock);
    if (photo_want != path) {
        photo_want = path;
        if (!photo_running) {
            photo_running = true;
            std::thread(photo_worker).detach();
        }
        return 0;
    }
    if (photo_ready_for != path)
        return 0;
    if (photo_failed) {
        *failed = true;
        return 0;
    }
    ImTextureID t = make_texture(photo);
    if (aspect)
        *aspect = (float)photo.w / photo.h;
    /* handed over: asking again (the same photo later) decodes it again */
    photo = Finished{};
    photo_want.clear();
    photo_ready_for.clear();
    return t;
}

void library_free_image(ImTextureID tex)
{
    if (!tex)
        return;
    for (size_t i = 0; i < textures.size(); i++) {
        if ((ImTextureID)textures[i].set != tex)
            continue;
        vkDeviceWaitIdle(gfx.device); /* the last frames may still read it */
        ImGui_ImplVulkan_RemoveTexture(textures[i].set);
        vkDestroyImageView(gfx.device, textures[i].view, nullptr);
        vkDestroyImage(gfx.device, textures[i].image, nullptr);
        vkFreeMemory(gfx.device, textures[i].memory, nullptr);
        textures.erase(textures.begin() + (long)i);
        return;
    }
}

void library_set_busy(bool b)
{
    busy = b;
    if (!b)
        work_cv.notify_all();
}

void library_set_resume(const std::string &path, int64_t ms, int64_t length_ms)
{
    /* Near either end counts as "start over". */
    if (ms < 15000 || (length_ms > 0 && ms > length_ms - 30000))
        ms = 0;
    int64_t now = (int64_t)time(nullptr);
    bool found = false;
    for (auto &r : resumes)
        if (r.first == path) {
            r.second = { ms, length_ms, now };
            found = true;
        }
    if (!found)
        resumes.push_back({ path, { ms, length_ms, now } });
    save_resumes();
    for (MediaItem &m : items)
        if (m.path == path) {
            m.resume_ms = ms;
            m.last_played = now;
            if (length_ms > 0)
                m.length_ms = length_ms;
        }
}

MediaItem *library_find(const std::string &path)
{
    for (MediaItem &m : items)
        if (m.path == path)
            return &m;
    return nullptr;
}

std::string format_time(int64_t ms)
{
    if (ms < 0)
        ms = 0;
    int64_t s = ms / 1000;
    char buf[32];
    if (s >= 3600)
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)(s / 3600),
                 (long long)(s / 60 % 60), (long long)(s % 60));
    else
        snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

bool library_toggle_favourite(const std::string &path)
{
    auto it = std::find(favourites.begin(), favourites.end(), path);
    bool now_on = it == favourites.end();
    if (now_on)
        favourites.push_back(path);
    else
        favourites.erase(it);
    save_favourites();
    for (MediaItem &m : items)
        if (m.path == path)
            m.favourite = now_on;
    return now_on;
}

const MediaDetails &library_details(const std::string &path)
{
    std::lock_guard<std::mutex> l(details_lock);
    if (details_path != path) {
        details_path = path;
        details = MediaDetails{};
        MediaItem *m = library_find(path);
        std::thread(details_main, path, m ? m->size : 0, m ? m->ext : std::string()).detach();
    }
    details_copy = details;
    return details_copy;
}

std::vector<PlaylistFile> &library_playlists()
{
    return playlists;
}

namespace {

/* "%20"-style escapes of file:// lines. */
std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void save_bookmarks()
{
    std::string tmp = bookmarks_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    for (auto &b : bookmarks)
        fprintf(f, "%lld\t%s\n", (long long)b.second, b.first.c_str());
    fclose(f);
    rename(tmp.c_str(), bookmarks_path.c_str());
}

} // namespace

std::vector<int> library_playlist_items(const std::string &path)
{
    std::vector<int> out;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return out;
    std::string dir = path.substr(0, path.rfind('/'));
    bool pls = strcasecmp(path.c_str() + path.size() - 4, ".pls") == 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
            l.pop_back();
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF) /* UTF-8 mark */
            l = l.substr(3);
        if (pls) {
            if (strncasecmp(l.c_str(), "File", 4) != 0 || l.find('=') == std::string::npos)
                continue;
            l = l.substr(l.find('=') + 1);
        } else if (l.empty() || l[0] == '#') {
            continue;
        }
        if (!strncasecmp(l.c_str(), "file://", 7))
            l = url_decode(l.substr(7));
        for (char &c : l)
            if (c == '\\')
                c = '/'; /* playlists made on Windows */
        std::string full = l[0] == '/' ? l : dir + "/" + l;
        for (int i = 0; i < (int)items.size(); i++)
            if (items[i].path == full) {
                out.push_back(i);
                break;
            }
    }
    fclose(f);
    return out;
}

/* ---- playlists made in VLC -------------------------------------------------------- */

namespace {

std::string playlist_dir()
{
    return media_dir + "/Playlists";
}

/* A name that can be a file name everywhere (FAT/exFAT drives, Windows). */
std::string playlist_file_name(const std::string &name)
{
    std::string n;
    for (char c : name)
        n += strchr("/\\:*?\"<>|", c) || (unsigned char)c < 32 ? '-' : c;
    while (!n.empty() && (n.back() == ' ' || n.back() == '.'))
        n.pop_back();
    while (!n.empty() && n[0] == ' ')
        n.erase(0, 1);
    return n.substr(0, 120);
}

/* The files of one of ours, in order. */
std::vector<std::string> playlist_read(const std::string &path)
{
    std::vector<std::string> out;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return out;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF)
            l = l.substr(3);
        if (!l.empty() && l[0] != '#')
            out.push_back(l);
    }
    fclose(f);
    return out;
}

/* Written whole to a temporary file, then put in place. */
bool playlist_write(const std::string &path, const std::vector<std::string> &files)
{
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    bool ok = fputs("#EXTM3U\n", f) >= 0;
    for (const std::string &p : files) {
        std::string name = p.substr(p.rfind('/') + 1);
        ok = ok && fprintf(f, "#EXTINF:-1,%s\n%s\n", tidy_name(name).c_str(), p.c_str()) > 0;
    }
    ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok)
        unlink(tmp.c_str());
    return ok;
}

void playlists_sort()
{
    std::sort(playlists.begin(), playlists.end(), [](const PlaylistFile &a, const PlaylistFile &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

} // namespace

bool library_playlist_ours(const std::string &path)
{
    std::string d = playlist_dir() + "/";
    return path.compare(0, d.size(), d) == 0 && path.size() > 5 &&
           !strcasecmp(path.c_str() + path.size() - 5, ".m3u8");
}

std::string library_playlist_create(const std::string &name)
{
    std::string n = playlist_file_name(name);
    if (n.empty())
        return "";
    mkdir(playlist_dir().c_str(), 0777);
    std::string path = playlist_dir() + "/" + n + ".m3u8";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 || !playlist_write(path, {}))
        return "";
    playlists.push_back({ path, tidy_name(n + ".m3u8"), playlist_dir() });
    playlists_sort();
    return path;
}

bool library_playlist_add(const std::string &playlist, const std::string &file)
{
    std::vector<std::string> files = playlist_read(playlist);
    if (std::find(files.begin(), files.end(), file) != files.end())
        return false;
    files.push_back(file);
    return playlist_write(playlist, files);
}

bool library_playlist_remove(const std::string &playlist, const std::string &file)
{
    std::vector<std::string> files = playlist_read(playlist);
    auto it = std::find(files.begin(), files.end(), file);
    if (it == files.end())
        return false;
    files.erase(it);
    return playlist_write(playlist, files);
}

bool library_playlist_move(const std::string &playlist, const std::string &file, int step)
{
    std::vector<std::string> files = playlist_read(playlist);
    auto it = std::find(files.begin(), files.end(), file);
    if (it == files.end())
        return false;
    /* to the next one the library has (a file on a drive that's out is
     * skipped over, so a move always shows) */
    long i = it - files.begin(), j = i;
    do
        j += step;
    while (j >= 0 && j < (long)files.size() && !library_find(files[j]));
    if (j < 0 || j >= (long)files.size())
        return false;
    std::string moved = files[i];
    files.erase(files.begin() + i);
    files.insert(files.begin() + j, moved);
    return playlist_write(playlist, files);
}

std::string library_playlist_rename(const std::string &playlist, const std::string &name)
{
    std::string n = playlist_file_name(name);
    if (n.empty())
        return "";
    std::string path = playlist_dir() + "/" + n + ".m3u8";
    struct stat st;
    if (path == playlist)
        return path;
    if (stat(path.c_str(), &st) == 0 || rename(playlist.c_str(), path.c_str()) != 0)
        return "";
    for (PlaylistFile &p : playlists)
        if (p.path == playlist) {
            p.path = path;
            p.name = tidy_name(n + ".m3u8");
        }
    playlists_sort();
    return path;
}

std::vector<int64_t> library_bookmarks(const std::string &path)
{
    std::vector<int64_t> out;
    for (auto &b : bookmarks)
        if (b.first == path)
            out.push_back(b.second);
    std::sort(out.begin(), out.end());
    return out;
}

void library_add_bookmark(const std::string &path, int64_t ms)
{
    for (auto &b : bookmarks)
        if (b.first == path && llabs(b.second - ms) < 2000)
            return; /* one is already there */
    bookmarks.push_back({ path, ms });
    save_bookmarks();
}

void library_clear_bookmarks(const std::string &path)
{
    bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                   [&](const std::pair<std::string, int64_t> &b) { return b.first == path; }),
                    bookmarks.end());
    save_bookmarks();
}

/* Deleting from the app: the file, or a folder with all it holds, and what
 * the library remembered about them (resume points, favourites, bookmarks,
 * thumbnails). */
namespace {

int delete_tree(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return 0;
    int n = 0;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            std::vector<std::string> names;
            while (struct dirent *e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                    names.push_back(e->d_name);
            closedir(d);
            for (const std::string &name : names)
                n += delete_tree(path + "/" + name);
        }
        if (rmdir(path.c_str()) != 0)
            fprintf(stderr, "library: can't remove folder %s (errno %d)\n", path.c_str(), errno);
        return n;
    }
    if (unlink(path.c_str()) == 0)
        return 1;
    fprintf(stderr, "library: can't delete %s (errno %d)\n", path.c_str(), errno);
    return 0;
}

void forget_under(const std::string &path)
{
    auto inside = [&](const std::string &p) {
        return p == path || p.compare(0, path.size() + 1, path + "/") == 0;
    };
    for (MediaItem &m : items)
        if (inside(m.path))
            unlink(thumb_file(m).c_str());
    resumes.erase(std::remove_if(resumes.begin(), resumes.end(),
                                 [&](const std::pair<std::string, Resume> &r) { return inside(r.first); }),
                  resumes.end());
    save_resumes();
    favourites.erase(std::remove_if(favourites.begin(), favourites.end(), inside), favourites.end());
    save_favourites();
    bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                   [&](const std::pair<std::string, int64_t> &b) { return inside(b.first); }),
                    bookmarks.end());
    save_bookmarks();
}

} // namespace

int library_count_files(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return 1;
    int n = 0;
    if (DIR *d = opendir(path.c_str())) {
        while (struct dirent *e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                n += library_count_files(path + "/" + e->d_name);
        closedir(d);
    }
    return n;
}

bool library_delete(const std::string &path)
{
    for (const std::string &r : roots)
        if (r == path)
            return false; /* the media folder or a drive itself: never */
    forget_under(path);
    int n = delete_tree(path);
    struct stat st;
    bool gone = stat(path.c_str(), &st) != 0;
    fprintf(stderr, "library: deleted %s (%d files)%s\n", path.c_str(), n, gone ? "" : ", not all of it");
    scan();
    return gone;
}
