/*
 * VLC-PS5: writing pictures (screenshots, the host build's test shots).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>

/* rgb: w * h * 3 bytes, rows top to bottom. */
bool write_png_rgb(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h);
