/*
 * VLC-PS5: subtitle files that aren't UTF-8 (Windows-1252, GBK, Big5, Shift
 * JIS, Windows-1256...). The console's VLC has no iconv, so it read them as
 * UTF-8 and showed mangled letters; such a file is converted here (GNU
 * libiconv) to a UTF-8 copy in cache/subtitles/converted, which VLC gets
 * instead.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <string>

/* The bytes of a subtitle file made UTF-8: as they are when they already are
 * (or the encoding can't be told), else converted from UTF-16 (by its BOM)
 * or from the legacy code page of lang ("zh-CN", "ar", "ru"...; "" = Western).
 * *from says which encoding it was read as ("" = left alone). */
std::string subconv_to_utf8(const std::string &bytes, const std::string &lang, std::string *from = nullptr);

/* A local subtitle file ready for VLC: the path itself when it's UTF-8 (or
 * can't be read), else the path of a converted UTF-8 copy. */
std::string subconv_local(const std::string &path, const std::string &lang);

/* The same for bytes already read (a file on a share or a link): the copy's
 * path, or "" when they're UTF-8 already and VLC can read the original. name:
 * the file's name, for the copy's. */
std::string subconv_bytes(const std::string &bytes, const std::string &name, const std::string &lang);
