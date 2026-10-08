/*
 * VLC-PS5's interface languages (see lang.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lang.h"

#include <atomic>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fribidi/fribidi.h>

#include "platform.h"
#include "prefs.h"

namespace {

struct Lang {
    const char *code, *name;
    int console;    /* the console's language number(s) */
    int console2;
};
const Lang langs[] = {
    { "en", "English", 1, 18 },
    { "ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", 21, -1 },
    { "fr", "Fran\xC3\xA7" "ais", 2, 22 },
    { "es", "Espa\xC3\xB1ol", 3, 20 },
    { "pt-BR", "Portugu\xC3\xAAs (Brasil)", 17, 7 },
    { "de", "Deutsch", 4, -1 },
    { "it", "Italiano", 5, -1 },
    { "tr", "T\xC3\xBCrk\xC3\xA7" "e", 19, -1 },
    { "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", 8, 30 },
    { "pl", "Polski", 16, -1 },
    { "nl", "Nederlands", 6, -1 },
    { "ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 0, -1 },
    { "ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", 9, -1 },
    { "zh-CN", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", 11, -1 },
    { "zh-TW", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", 10, -1 },
    { "id", "Bahasa Indonesia", 29, -1 },
};
const int LANGS = (int)(sizeof(langs) / sizeof(langs[0]));

typedef std::unordered_map<std::string, std::string> Table;
/* Loaded once each and never freed: a thread reading one while the choice
 * changes stays safe. */
Table *tables[LANGS];
std::atomic<int> current{ 0 };

/* the shaped strings of this frame and the last ones */
std::unordered_map<std::string, std::string> shown;

Table *load(int i)
{
    if (tables[i])
        return tables[i];
    Table *t = new Table;
    std::string path = std::string(plat_data_dir()) + "/assets/lang/" + langs[i].code + ".txt";
    if (FILE *f = fopen(path.c_str(), "rb")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = 0;
            /* "English = translation" */
            char *eq = strstr(line, " = ");
            if (line[0] == '#' || !eq || !eq[3])
                continue;
            *eq = 0;
            (*t)[line] = eq + 3;
        }
        fclose(f);
    } else if (i) {
        fprintf(stderr, "lang: no %s\n", path.c_str());
    }
    fprintf(stderr, "lang: %s, %zu strings\n", langs[i].code, t->size());
    tables[i] = t;
    return t;
}

/* Arabic and Hebrew letters (UTF-8 lead bytes of U+0580-077F): right to left. */
bool has_rtl(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0xD6 && *p <= 0xDD)
            return true;
    return false;
}

/* Joined letters (presentation forms) in the order they're drawn. */
std::string visual(const char *s)
{
    size_t n = strlen(s);
    std::vector<FriBidiChar> in(n + 1), out(n + 1);
    FriBidiStrIndex len = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, s, (FriBidiStrIndex)n, in.data());
    FriBidiParType base = FRIBIDI_PAR_ON;
    if (!fribidi_log2vis(in.data(), len, &base, out.data(), nullptr, nullptr, nullptr))
        return s;
    /* the joining leaves fillers where two letters became one (lam-alef) */
    FriBidiStrIndex k = 0;
    for (FriBidiStrIndex i = 0; i < len; i++)
        if (out[i] != 0xFEFF && out[i] != 0x200F && out[i] != 0x200E)
            out[k++] = out[i];
    std::vector<char> utf8((size_t)k * 4 + 1);
    fribidi_unicode_to_charset(FRIBIDI_CHAR_SET_UTF8, out.data(), k, utf8.data());
    return utf8.data();
}

}  // namespace

void lang_init()
{
    std::string saved = pref_str("ui_lang", "");
    int pick = -1;
    for (int i = 0; i < LANGS; i++)
        if (saved == langs[i].code)
            pick = i;
    if (pick < 0) {
        int sys = plat_language();
        for (int i = 0; i < LANGS && pick < 0; i++)
            if (sys >= 0 && (sys == langs[i].console || sys == langs[i].console2))
                pick = i;
        fprintf(stderr, "lang: console language %d\n", sys);
    }
    if (pick < 0)
        pick = 0;
    load(pick);
    current = pick;
}

int lang_count()
{
    return LANGS;
}

int lang_current()
{
    return current;
}

void lang_set(int i)
{
    if (i < 0 || i >= LANGS)
        return;
    load(i);
    current = i;
    shown.clear();
    pref_set("ui_lang", std::string(langs[i].code));
}

const char *lang_code(int i)
{
    return i >= 0 && i < LANGS ? langs[i].code : "en";
}

const char *lang_native_name(int i)
{
    return i >= 0 && i < LANGS ? langs[i].name : "";
}

const char *tr(const char *english)
{
    int c = current;
    if (c == 0 || !english || !*english)
        return english;
    Table *t = tables[c];
    if (!t)
        return english;
    auto it = t->find(english);
    if (it == t->end()) {
        /* Test aid (the host build with VLCPS5_LANG_MISSES set): each string
         * drawn that this language lacks, once, so a tour finds them all. */
        static const bool log_misses = getenv("VLCPS5_LANG_MISSES") != nullptr;
        if (log_misses) {
            static std::mutex m;
            static std::unordered_set<std::string> said;
            std::lock_guard<std::mutex> g(m);
            if (said.insert(english).second)
                fprintf(stderr, "lang-miss: %s\n", english);
        }
        return english;
    }
    return it->second.c_str();
}

std::string trf(const char *english_format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, english_format);
    vsnprintf(buf, sizeof buf, tr(english_format), ap);
    va_end(ap);
    return buf;
}

const char *lang_shown_as_is(const char *t)
{
    if (!has_rtl(t))
        return t;
    auto it = shown.find(t);
    if (it == shown.end())
        it = shown.emplace(t, visual(t)).first;
    return it->second.c_str();
}

const char *lang_shown(const char *s)
{
    const char *t = tr(s);
    if (!has_rtl(t))
        return t;
    auto it = shown.find(t);
    if (it == shown.end())
        it = shown.emplace(t, visual(t)).first;
    return it->second.c_str();
}

void lang_frame()
{
    /* text that changes (times, names) would pile up */
    if (shown.size() > 3000)
        shown.clear();
}
