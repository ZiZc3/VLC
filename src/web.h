/*
 * VLC-PS5's web page for phones and computers on the same network: send
 * videos and music to the console (no FTP), and an OpenSubtitles key.
 * A small HTTP server on its own threads; the main thread hands it what it
 * may show (web_set_*) and takes what arrived (web_take_*).
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

/* What the page shows about subtitle downloads: never the key or the
 * password, only whether a key is saved, the account and the last check. */
struct WebOsubInfo {
    bool has_key, checking, failed; /* failed: the last check went wrong */
    std::string user, message;
};
void web_set_osub_info(const WebOsubInfo &info);

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

/* A link another app or device asked us to play, once:
 *   GET /api/play?url=<link>[&title=<name>][&sub=<subtitle link>]  (or /play)
 * from: the asker's address, for the toast. The newest wins. */
struct WebPlay {
    std::string url, title, sub, from;
};
bool web_take_play(WebPlay *out);
/* A network share typed on the page (POST /api/share: address, user, pass),
 * once: saved on the console with its login. */
bool web_take_share(std::string *address, std::string *user, std::string *pass);
/* Whether a link is one we play when asked from outside: network links only
 * (http, https, smb, ftp, udp, rtp, mms). */
bool web_play_link_ok(const std::string &url);
