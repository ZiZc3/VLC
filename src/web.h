/*
 * VLC-PS5's web page for phones and computers on the same network: send
 * videos and music to the console (no FTP), and a remote for what's playing.
 * A small HTTP server on its own threads; the main thread hands it what it
 * may show (web_set_*) and takes the remote's commands (web_next_command).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

void web_start();  /* listens on port 8080 (again after web_stop) */
void web_stop();
bool web_running();
/* "http://192.168.1.20:8080", or "" while the console's address is unknown. */
std::string web_address();

/* Where uploads may go: name shown on the page, and the folder. */
struct WebPlace {
    std::string name, path;
};
void web_set_places(const std::vector<WebPlace> &places);

/* What the remote shows. */
struct WebStatus {
    bool playing;      /* a file is open */
    bool paused;
    std::string title;
    int64_t time_ms, length_ms;
    int volume;        /* 0..200 */
    bool loop;
};
void web_set_status(const WebStatus &s);

/* The remote's buttons, for the main thread. */
enum WebCommandKind { WEB_TOGGLE, WEB_JUMP, WEB_SEEK, WEB_NEXT, WEB_PREV, WEB_VOLUME, WEB_STOP };
struct WebCommand {
    WebCommandKind kind;
    int64_t value; /* JUMP: ms (+/-), SEEK: ms, VOLUME: +/- percent */
};
bool web_next_command(WebCommand *c);

/* An upload in progress (for the TV), and whether one finished since asked. */
struct WebUpload {
    bool active;
    std::string name;
    int64_t done, total;
};
WebUpload web_upload_state();
bool web_take_finished();

/* An OpenSubtitles key and account sent from the page, once. */
bool web_take_opensubtitles(std::string *key, std::string *user, std::string *pass);
