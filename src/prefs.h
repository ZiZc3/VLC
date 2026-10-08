/*
 * VLC-PS5's remembered choices (subtitle look, languages, equalizer, picture,
 * favourites live elsewhere): "key=value" lines in prefs.txt in the app's
 * folder. settings.txt stays the hand-edited one (refresh=, vlc_debug=).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <string>

int pref_int(const char *key, int fallback);
std::string pref_str(const char *key, const char *fallback);
void pref_set(const char *key, int value);
void pref_set(const char *key, const std::string &value);
/* prefs.txt was there when VLC started: not the first run. */
bool prefs_existed();
