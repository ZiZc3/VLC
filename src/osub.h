/*
 * VLC-PS5's subtitle downloads from OpenSubtitles (api.opensubtitles.com),
 * with the user's own free API key (and account, for more downloads a day).
 * Everything runs in the background; the interface polls.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <string>
#include <vector>

struct SubResult {
    long long file_id;
    std::string language;  /* "en", "ar", "pt-BR"... */
    std::string release;   /* the release it was made for */
    int downloads;
    bool hash_match;       /* made for exactly this file */
};

enum OsubState { OSUB_IDLE, OSUB_BUSY, OSUB_DONE, OSUB_FAILED };

bool osub_has_key();
/* Looks for subtitles of a file (path: a local file gets its hash matched;
 * name: what it's called) in languages ("en,ar"). */
void osub_search(const std::string &path, const std::string &name, const std::string &languages);
/* Downloads one into save_base + ".<lang>.srt" (next to the video, or the
 * app's cache when that can't be written). */
void osub_download(const SubResult &r, const std::string &save_base, const std::string &fallback_base);
/* Checks a user's account (signs in), in the background. */
void osub_sign_in();

OsubState osub_state();
std::vector<SubResult> osub_results();
std::string osub_message();     /* what went wrong, or "12 downloads left today" */
std::string osub_saved_path();  /* the file a download wrote, once */
