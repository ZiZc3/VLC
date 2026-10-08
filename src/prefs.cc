/*
 * VLC-PS5's remembered choices: prefs.txt, read once, written whole (through a
 * temporary file) on every change.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "prefs.h"

#include <map>
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

namespace {

std::map<std::string, std::string> values;
bool loaded, existed;

std::string path()
{
    return std::string(plat_data_dir()) + "/prefs.txt";
}

void load()
{
    if (loaded)
        return;
    loaded = true;
    FILE *f = fopen(path().c_str(), "r");
    if (!f)
        return;
    existed = true;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        size_t eq = l.find('=');
        if (eq != std::string::npos && eq > 0)
            values[l.substr(0, eq)] = l.substr(eq + 1);
    }
    fclose(f);
}

void save()
{
    std::string tmp = path() + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    for (auto &kv : values)
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
    rename(tmp.c_str(), path().c_str());
}

} // namespace

bool prefs_existed()
{
    load();
    return existed;
}

int pref_int(const char *key, int fallback)
{
    load();
    auto it = values.find(key);
    return it == values.end() || it->second.empty() ? fallback : atoi(it->second.c_str());
}

std::string pref_str(const char *key, const char *fallback)
{
    load();
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

void pref_set(const char *key, int value)
{
    pref_set(key, std::to_string(value));
}

void pref_set(const char *key, const std::string &value)
{
    load();
    if (values[key] == value)
        return;
    values[key] = value;
    save();
}
