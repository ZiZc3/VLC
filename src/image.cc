/*
 * VLC-PS5: a minimal PNG writer (8-bit RGB, one zlib stream).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "image.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <zlib.h>

namespace {

void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t be[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    crc = crc32(crc, data, len);
    uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(c, 1, 4, f);
}

} // namespace

bool write_png_rgb(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    size_t stride = 1 + (size_t)w * 3;
    std::vector<uint8_t> raw(stride * h);
    for (uint32_t y = 0; y < h; y++) {
        raw[y * stride] = 0; /* no filter */
        memcpy(&raw[y * stride + 1], rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uLongf zlen = compressBound(raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), raw.size(), 6) != Z_OK)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                         8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z.data(), (uint32_t)zlen);
    chunk(f, "IEND", nullptr, 0);
    return fclose(f) == 0;
}
