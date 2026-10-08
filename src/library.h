/*
 * VLC-PS5's media library: the files in the app's media folder (and any
 * other readable root), their lengths, thumbnails and where you stopped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "imgui.h"

struct MediaItem {
    std::string path;
    std::string name;      /* file name without extension, tidied */
    std::string ext;       /* "MKV" */
    std::string folder;    /* the folder it's in, for Browse */
    bool audio;            /* by extension: music, no thumbnail */
    bool image;            /* by extension: a photo (JPG, PNG, ...) */
    bool text;             /* by extension: .txt, .nfo, ... (the text viewer) */
    std::string artist, album; /* a song's tags, "" until read (or none) */
    bool disc;             /* a DVD / Blu-ray: an .iso, or a folder with VIDEO_TS or BDMV */
    bool archive;          /* .zip / .rar / .7z: opened like a folder in Browse */
    bool other;            /* any other file: listed in Browse only; X hands it to VLC */
    int64_t size;
    int64_t mtime;
    int64_t length_ms;     /* 0 until known */
    int64_t resume_ms;     /* where it was stopped, 0 if watched or new */
    int64_t last_played;   /* unix time, 0 if never */
    ImTextureID thumb;     /* 0 until made */
    float thumb_aspect;
    bool favourite;
};

/* A playlist file (.m3u, .m3u8, .pls) found in the media folders. */
struct PlaylistFile {
    std::string path, name, folder;
};

/* What □ shows about a file: label/value rows, read in the background. */
struct MediaDetails {
    bool ready;
    std::vector<std::pair<std::string, std::string>> rows;
};

void library_init();          /* scan, read caches, start the thumbnailer */
void library_shutdown();
void library_rescan();
/* Bumped by every scan: the lists built from library_items() are stale. */
int library_generation();
/* A PNG as a texture for the interface (BGRA; aspect = width / height). */
ImTextureID library_load_image(const std::string &path, float *aspect);
std::vector<MediaItem> &library_items();
std::vector<std::string> library_roots();   /* readable media folders */
const std::string &library_media_dir();     /* the app's own media folder */
/* Main thread, every frame: thumbnails the worker finished become textures. */
void library_update();
/* A photo at screen size, decoded in the background. Call it every frame
 * until it gives the texture (once; it's then the caller's, to free with
 * library_free_image); *failed when the file can't be read. */
ImTextureID library_photo(const std::string &path, float *aspect, bool *failed);
void library_free_image(ImTextureID tex);
/* Pauses the thumbnailer while a video plays (it shares the CPU). */
void library_set_busy(bool busy);
void library_set_resume(const std::string &path, int64_t ms, int64_t length_ms);
MediaItem *library_find(const std::string &path);
std::vector<PlaylistFile> &library_playlists();
/* The library files a playlist names, in its order (others are skipped). */
std::vector<int> library_playlist_items(const std::string &path);
/* Playlists made in VLC: <media>/Playlists/<name>.m3u8, one absolute path a
 * line (with #EXTINF names), so VLC on a computer reads them too. Only these
 * are changed; ones found elsewhere just play. */
bool library_playlist_ours(const std::string &path);
/* A new empty one; its path, or "" (the name is taken, or no disk). */
std::string library_playlist_create(const std::string &name);
/* Adds a file at the end; false when it was already in or can't be written. */
bool library_playlist_add(const std::string &playlist, const std::string &file);
bool library_playlist_remove(const std::string &playlist, const std::string &file);
/* Moves a file one place up (-1) or down (+1). */
bool library_playlist_move(const std::string &playlist, const std::string &file, int step);
/* A new name; the playlist's new path, or "". */
std::string library_playlist_rename(const std::string &playlist, const std::string &name);
/* Bookmarks inside a file (ms, sorted), remembered in cache/bookmarks.txt. */
std::vector<int64_t> library_bookmarks(const std::string &path);
void library_add_bookmark(const std::string &path, int64_t ms);
void library_clear_bookmarks(const std::string &path);
/* Deletes a file, or a folder with everything in it, and what the library
 * remembered about them; rescans. Never a media folder or drive itself. */
bool library_delete(const std::string &path);
int library_count_files(const std::string &path); /* files in a folder (1 for a file) */
/* Favourites (△), remembered in cache/favourites.txt. Returns the new state. */
bool library_toggle_favourite(const std::string &path);
/* The first call starts reading the file; later calls see it fill in. */
const MediaDetails &library_details(const std::string &path);
std::string format_time(int64_t ms);
