/*
 * VLC-PS5's interface languages. The English text is the key: tr() gives its
 * translation from assets/lang/<code>.txt ("English = translation" lines),
 * or the English itself. The drawing functions translate every string they
 * draw, so most labels need nothing more; sentences built from parts use
 * trf() with a format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <string>

void lang_init();               /* the saved choice, else the console's language */
int lang_count();
int lang_current();
void lang_set(int i);           /* also remembered */
const char *lang_code(int i);   /* "en", "ar", "pt-BR"... */
const char *lang_native_name(int i); /* in its own language: "Deutsch" */

const char *tr(const char *english);
std::string trf(const char *english_format, ...);

/* What text() draws: translated, and right-to-left scripts (Arabic, Hebrew)
 * joined and put in visual order. Valid until lang_frame(). */
const char *lang_shown(const char *s);
const char *lang_shown_as_is(const char *s);  /* the same, not translated */
void lang_frame();              /* once a frame */
