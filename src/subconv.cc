/*
 * VLC-PS5: subtitle files that aren't UTF-8, made UTF-8 for VLC (see
 * subconv.h). Our own code; the conversion tables are GNU libiconv's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "subconv.h"

#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"

namespace {

/* Valid UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF). */
bool is_utf8(const std::string &s)
{
    const unsigned char *p = (const unsigned char *)s.data(), *end = p + s.size();
    while (p < end) {
        unsigned c = *p;
        if (c < 0x80) {
            p++;
            continue;
        }
        int n = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : -1;
        if (n < 0 || c > 0xF4 || end - p <= n)
            return false;
        uint32_t cp = c & (0x3F >> n);
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80)
                return false;
            cp = cp << 6 | (p[i] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += n + 1;
    }
    return true;
}

/* The code page subtitles in that language are usually saved in when they
 * aren't Unicode (what Windows' "ANSI" was for it). */
const char *legacy_encoding(const std::string &lang)
{
    std::string l = lang.substr(0, lang.find('-'));
    if (lang == "zh-TW" || lang == "zh-HK" || lang == "zht")
        return "BIG5-HKSCS";
    if (l == "zh" || l == "chi" || l == "zho")
        return "GB18030";
    if (l == "ja" || l == "jpn")
        return "CP932";
    if (l == "ko" || l == "kor")
        return "CP949";
    if (l == "ru" || l == "rus" || l == "uk" || l == "ukr" || l == "bg" || l == "bul" || l == "sr" || l == "mk" || l == "be")
        return "CP1251";
    if (l == "ar" || l == "ara" || l == "fa" || l == "per" || l == "fas" || l == "ur")
        return "CP1256";
    if (l == "he" || l == "heb")
        return "CP1255";
    if (l == "tr" || l == "tur")
        return "CP1254";
    if (l == "el" || l == "gre" || l == "ell")
        return "CP1253";
    if (l == "pl" || l == "pol" || l == "cs" || l == "cze" || l == "ces" || l == "sk" || l == "hu" || l == "hun" ||
        l == "ro" || l == "rum" || l == "hr" || l == "sl")
        return "CP1250";
    if (l == "th" || l == "tha")
        return "CP874";
    if (l == "vi" || l == "vie")
        return "CP1258";
    return "CP1252";
}

bool convert(const std::string &in, const char *from, std::string *out)
{
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1)
        return false;
    out->clear();
    out->resize(in.size() * 4 + 16);
    char *src = (char *)in.data(), *dst = &(*out)[0];
    size_t left = in.size(), room = out->size();
    bool ok = true;
    while (left > 0) {
        if (iconv(cd, &src, &left, &dst, &room) == (size_t)-1) {
            /* a byte the code page hasn't got: U+FFFD, and on */
            if (room < 3) {
                ok = false;
                break;
            }
            memcpy(dst, "\xEF\xBF\xBD", 3);
            dst += 3;
            room -= 3;
            src++;
            left--;
        }
    }
    iconv_close(cd);
    out->resize(out->size() - room);
    return ok;
}

std::string copy_dir()
{
    std::string d = std::string(plat_data_dir()) + "/cache";
    mkdir(d.c_str(), 0777);
    d += "/subtitles";
    mkdir(d.c_str(), 0777);
    d += "/converted";
    mkdir(d.c_str(), 0777);
    return d;
}

} // namespace

std::string subconv_to_utf8(const std::string &bytes, const std::string &lang, std::string *from)
{
    if (from)
        from->clear();
    const unsigned char *b = (const unsigned char *)bytes.data();
    std::string out;
    if (bytes.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        const char *enc = b[0] == 0xFF ? "UTF-16LE" : "UTF-16BE";
        if (convert(bytes.substr(2), enc, &out)) {
            if (from)
                *from = enc;
            return out;
        }
        return bytes;
    }
    if (is_utf8(bytes))
        return bytes;
    const char *enc = legacy_encoding(lang);
    if (!convert(bytes, enc, &out))
        return bytes;
    if (from)
        *from = enc;
    return out;
}

std::string subconv_bytes(const std::string &bytes, const std::string &name, const std::string &lang)
{
    std::string from;
    std::string utf8 = subconv_to_utf8(bytes, lang, &from);
    if (from.empty())
        return "";
    /* The name kept (VLC shows it, and finds the format by its extension). */
    std::string base = name.substr(name.rfind('/') + 1);
    std::string path = copy_dir() + "/" + base;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return "";
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    fprintf(stderr, "subconv: %s read as %s, UTF-8 copy %s\n", base.c_str(), from.c_str(), path.c_str());
    return path;
}

std::string subconv_local(const std::string &path, const std::string &lang)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return path;
    std::string bytes;
    char buf[65536];
    size_t n;
    /* subtitle files are small: a big one is something else, left alone */
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (16u << 20))
        bytes.append(buf, n);
    fclose(f);
    if (bytes.size() >= (16u << 20))
        return path;
    std::string copy = subconv_bytes(bytes, path, lang);
    return copy.empty() ? path : copy;
}
