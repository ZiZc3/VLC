/*
 * VLC-PS5's player: one libvlc media player whose pictures land in Vulkan
 * buffers (vmem callbacks) and whose sound goes to the platform (amem).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "volk.h"

struct libvlc_instance_t;
struct libvlc_media_t;

struct Track {
    int id;
    std::string name;
};

struct VideoInfo {
    uint32_t width, height; /* visible size */
    float aspect;           /* display aspect, sample aspect applied */
    bool ten_bit;
    int hdr;                /* 0 SDR, 1 HDR10 (PQ), 2 HLG */
    bool hardware;          /* on the console's video decoder */
    float fps;              /* 0 if the file doesn't say */
    std::string codec, audio_codec;
    unsigned audio_channels;
    bool spherical;         /* a 360° video (equirectangular) */
};

enum DecodeMode { DECODE_AUTO, DECODE_FAST, DECODE_FULL };

bool player_init();
void player_shutdown();
libvlc_instance_t *player_vlc();

/* options: extra ":name=value" media options (a share's login, never in the address). */
bool player_open(const std::string &path, int64_t start_ms, const std::vector<std::string> &options = {});
/* The same with a media VLC made (an entry of a listed playlist, with the options
 * its list gave it); takes the reference. path: what it's called in the log. */
bool player_open_media(libvlc_media_t *m, const std::string &path, int64_t start_ms,
                       const std::vector<std::string> &options = {});
void player_stop();
bool player_active();    /* a file is open (playing, paused, buffering) */
bool player_ended();     /* the file played to its end (or failed) */
bool player_failed();
bool player_buffering(); /* opening or buffering: no picture moving yet */
/* Disc menus: the main menu (DVD root, Blu-ray Top Menu) or a Blu-ray's
 * pop-up menu; whether the Blu-ray playing has one. */
/* The Blu-ray region VLC plays as (A, B, C), from the next file. */
std::string player_bluray_region();
void player_set_bluray_region(const std::string &region);
void player_disc_menu(bool popup);
bool player_disc_has_popup();
bool player_paused();
void player_toggle_pause();
int64_t player_time();   /* ms */
int64_t player_length(); /* ms, 0 if unknown */
void player_seek(int64_t ms);
int player_volume();     /* 0..200 */
void player_set_volume(int volume);
float player_rate();
void player_set_rate(float rate);
std::vector<Track> player_audio_tracks();
int player_audio_track();
void player_set_audio_track(int id);
std::vector<Track> player_subtitle_tracks();
int player_subtitle_track();
void player_set_subtitle_track(int id);
/* Adds a subtitle file (.srt, .ass, ...) to what plays and selects it. */
bool player_add_subtitle(const std::string &path); /* a local path, or a share's / a link's address */
bool player_is_subtitle_name(const std::string &name); /* .srt, .ass, .vtt... */
const VideoInfo &player_video_info();

/* A 360° video: where the view looks (degrees) and how wide it sees. */
void player_view(float yaw, float pitch, float fov);

/* A DVD / Blu-ray menu: 0 activate, 1 up, 2 down, 3 left, 4 right. */
void player_navigate(int action);
/* A disc's menu is on screen (its current title is a menu). */
bool player_in_menu();

/* The sound being heard, as n bands from bass to treble (0..1 each). */
void player_spectrum(float *bands, int n);

/* Render thread, every frame: */
void video_upload(VkCommandBuffer cmd);           /* before the render pass */
bool video_has_picture();
/* An HDR video is showing and HDR10 output is wanted for it (settings.txt
 * hdr_output, the console's output). */
bool video_wants_hdr_output();
void video_draw(VkCommandBuffer cmd, float x, float y, float w, float h); /* in the pass */
void video_forget_picture();                      /* back to the library */
/* One line about the player in the log (state, time, picture slots, sound queue,
 * pictures decoded/shown/lost since the last line). */
void player_debug_line();

/* Lip sync and subtitle timing, ms (positive: later). Per file, back to 0 on open. */
int64_t player_audio_delay();
void player_set_audio_delay(int64_t ms);
int64_t player_subtitle_delay();
void player_set_subtitle_delay(int64_t ms);

struct Chapter {
    int64_t start_ms;
    std::string name;
};
std::vector<Chapter> player_chapters(); /* empty if the file has none */
int player_chapter();
void player_set_chapter(int index);

void player_next_frame(); /* one picture on (pauses first) */
/* The picture on screen as a PNG (written in the background). False if there's none. */
bool player_screenshot(const std::string &path);

/* Equalizer: -1 off, else one of VLC's presets (Rock, Pop, ...). Remembered. */
int player_eq_presets();
const char *player_eq_name(int preset);
int player_eq();
void player_set_eq(int preset);

/* Sound output: 0 stereo (VLC downmixes surround files), 1 surround (5.1/7.1
 * files go out on 8 channels). From the next file. Remembered. */
int player_audio_output();
void player_set_audio_output(int mode);
/* Night mode: loud parts quieter, voices clearer. Live, remembered. */
bool player_night_mode();
void player_set_night_mode(bool on);
/* Speaker test: a tone on one of the console's 8 channels (L R C LFE Ls Rs
 * Lb Rb = 0..7), -1 stops. False if the console has no 8-channel output. */
bool player_speaker_test(int channel);

/* Deinterlace: 0 Auto (when the video says it's interlaced), 1 On, 2 Off. Remembered. */
int player_deinterlace();
void player_set_deinterlace(int mode);

/* Subtitle look, live: size 50..200 %, colour index, background box. Remembered. */
enum { SUB_WHITE, SUB_YELLOW, SUB_CYAN, SUB_GREEN, SUB_COLOURS };
extern const char *const sub_colour_names[SUB_COLOURS];
struct SubStyle {
    int size;   /* % */
    int colour; /* SUB_* */
    bool box;   /* dark box behind the text */
};
SubStyle player_sub_style();
void player_set_sub_style(const SubStyle &s);

/* Preferred languages (ISO 639 codes, "" = the file's default; subtitles "off"
 * = none unless forced). From the next file opened. Remembered. */
std::string player_audio_language();
std::string player_sub_language();
void player_set_languages(const std::string &audio, const std::string &sub);

/* Picture controls, done in the video shader. Remembered. */
struct PictureAdjust {
    int brightness; /* -100..100, 0 = as is */
    int contrast;   /* -100..100 */
    int saturation; /* -100..100 */
    int gamma;      /* -100..100 */
    int hue;        /* -180..180 degrees */
    int sharpen;    /* 0..100 */
    int rotate;     /* quarter turns clockwise, 0..3 (this file only) */
    bool mirror, upside_down; /* flips (this file only) */
};
int player_rotation();
PictureAdjust player_picture();
void player_set_picture(const PictureAdjust &p);

/* Detailed log (VLC's warnings, stats every 5 s): debug=1 in settings.txt, or Settings. */
bool player_debug_log();
void player_set_debug_log(bool on);

/* Decoding: Auto (fast once a file can't keep up), always fast, always full. */
DecodeMode player_decode_mode();
void player_set_decode_mode(DecodeMode mode); /* saved; applies from the next file */
bool player_fast_decoding();                  /* the current file decodes fast */
/* Main thread, every frame while playing: watches for lost pictures. */
enum PlayerEvent { PLAYER_EVENT_NONE, PLAYER_EVENT_WENT_FAST, PLAYER_EVENT_TOO_SLOW };
PlayerEvent player_tick();
