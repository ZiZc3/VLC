/*
 * VLC-PS5's network places (see network.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "network.h"

#include <algorithm>
#include <ctype.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <thread>

#include <vlc/vlc.h>

#include "platform.h"
#include "player.h"

namespace {

/* ---- saved shares: cache/servers.txt, "name<TAB>url<TAB>user<TAB>password" ---- */

struct Saved {
    std::string name, url, user, password;
};
std::vector<Saved> saved;

std::string servers_file()
{
    return std::string(plat_data_dir()) + "/cache/servers.txt";
}

void load_saved()
{
    saved.clear();
    FILE *f = fopen(servers_file().c_str(), "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        Saved s;
        std::string *field[4] = { &s.name, &s.url, &s.user, &s.password };
        size_t at = 0;
        for (int i = 0; i < 4; i++) {
            size_t tab = i < 3 ? l.find('\t', at) : std::string::npos;
            *field[i] = l.substr(at, tab == std::string::npos ? std::string::npos : tab - at);
            if (tab == std::string::npos)
                break;
            at = tab + 1;
        }
        if (!s.url.empty())
            saved.push_back(s);
    }
    fclose(f);
}

void store_saved()
{
    FILE *f = fopen(servers_file().c_str(), "w");
    if (!f)
        return;
    for (const Saved &s : saved)
        fprintf(f, "%s\t%s\t%s\t%s\n", s.name.c_str(), s.url.c_str(), s.user.c_str(), s.password.c_str());
    fclose(f);
}

/* ---- addresses ---- */

/* "smb://user:pw@host/x" -> "smb://host/x" */
std::string without_login(const std::string &url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return url;
    size_t host = scheme + 3, slash = url.find('/', host), at = url.find('@', host);
    if (at == std::string::npos || (slash != std::string::npos && at > slash))
        return url;
    return url.substr(0, host) + url.substr(at + 1);
}

/* Every "scheme://user:pw@" in a message loses its login. */
std::string scrub_logins(std::string text)
{
    size_t at = 0;
    while ((at = text.find("://", at)) != std::string::npos) {
        size_t start = at + 3, end = text.find_first_of(" /'\"", start), sign = text.find('@', start);
        if (sign != std::string::npos && (end == std::string::npos || sign < end))
            text.erase(start, sign + 1 - start);
        at = start;
    }
    return text;
}

std::string host_of(const std::string &url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return "";
    size_t host = scheme + 3;
    return url.substr(host, url.find('/', host) - host);
}

/* The saved share an address is in: the longest saved address it starts with,
 * else one on the same host. */
const Saved *saved_for(const std::string &url)
{
    const Saved *best = nullptr;
    for (const Saved &s : saved)
        if (url.compare(0, s.url.size(), s.url) == 0 && (!best || s.url.size() > best->url.size()))
            best = &s;
    if (!best)
        for (const Saved &s : saved)
            if (host_of(s.url) == host_of(url) && !s.user.empty())
                return &s;
    return best;
}

/* ---- what VLC asks (its dialog callbacks run on VLC's threads) ---- */

std::mutex ask_lock;
libvlc_dialog_id *ask_id;
NetAsk asking;
int ask_serial;
std::vector<libvlc_dialog_id *> to_dismiss;      /* cancelled by VLC: release on our thread */
std::vector<std::pair<std::string, std::string>> errors;
std::string login_for;                            /* the address being listed at the time */
std::string last_error;                           /* VLC's last error text, for the listing */

void cb_error(void *, const char *title, const char *text)
{
    std::lock_guard<std::mutex> g(ask_lock);
    std::string t = scrub_logins(text ? text : "");
    fprintf(stderr, "net: VLC says \"%s\": %s\n", title ? title : "", t.c_str());
    if (errors.size() < 8)
        errors.push_back({ title ? title : "", t });
    last_error = t;
}

void cb_login(void *, libvlc_dialog_id *id, const char *title, const char *text,
              const char *user, bool)
{
    std::lock_guard<std::mutex> g(ask_lock);
    if (ask_id)
        to_dismiss.push_back(ask_id);
    ask_id = id;
    asking = NetAsk{};
    asking.serial = ++ask_serial;
    asking.login = true;
    asking.title = title ? title : "";
    asking.text = text ? text : "";
    asking.user = user ? user : "";
    fprintf(stderr, "net: login asked: %s\n", asking.title.c_str());
}

void cb_question(void *, libvlc_dialog_id *id, const char *title, const char *text,
                 libvlc_dialog_question_type, const char *cancel, const char *a1, const char *a2)
{
    std::lock_guard<std::mutex> g(ask_lock);
    if (ask_id)
        to_dismiss.push_back(ask_id);
    ask_id = id;
    asking = NetAsk{};
    asking.serial = ++ask_serial;
    asking.title = title ? title : "";
    asking.text = text ? text : "";
    asking.actions[0] = a1 ? a1 : "";
    asking.actions[1] = a2 ? a2 : "";
    asking.cancel = cancel ? cancel : "Cancel";
    fprintf(stderr, "net: question asked: %s\n", asking.title.c_str());
}

/* Progress dialogs aren't shown: VLC cancels them when it's done. */
void cb_progress(void *, libvlc_dialog_id *, const char *, const char *, bool, float, const char *) {}
void cb_update_progress(void *, libvlc_dialog_id *, float, const char *) {}

void cb_cancel(void *, libvlc_dialog_id *id)
{
    std::lock_guard<std::mutex> g(ask_lock);
    if (id == ask_id) {
        ask_id = nullptr;
        asking = NetAsk{};
    }
    to_dismiss.push_back(id);
}

const libvlc_dialog_cbs dialog_cbs = {
    cb_error, cb_login, cb_question, cb_progress, cb_cancel, cb_update_progress,
};

/* ---- DLNA discovery ---- */

libvlc_media_discoverer_t *upnp;
std::thread upnp_starter;
std::mutex found_lock;
std::vector<NetServer> found;
double next_poll;

void poll_found()
{
    if (!upnp)
        return;
    std::vector<NetServer> now_found;
    libvlc_media_list_t *list = libvlc_media_discoverer_media_list(upnp);
    if (!list)
        return;
    libvlc_media_list_lock(list);
    for (int i = 0; i < libvlc_media_list_count(list); i++) {
        libvlc_media_t *m = libvlc_media_list_item_at_index(list, i);
        if (!m)
            continue;
        char *mrl = libvlc_media_get_mrl(m), *title = libvlc_media_get_meta(m, libvlc_meta_Title);
        if (mrl)
            now_found.push_back({ title ? title : mrl, mrl, false });
        free(mrl);
        free(title);
        libvlc_media_release(m);
    }
    libvlc_media_list_unlock(list);
    libvlc_media_list_release(list);
    std::lock_guard<std::mutex> g(found_lock);
    found = now_found;
}

/* ---- listing ---- */

libvlc_media_t *listing;
std::string list_url;
NetListState list_state = NET_IDLE;
int list_serial;  /* bumped on every change of the listing */
std::vector<NetEntry> entries;
std::string list_error;
double list_started;

void clear_entries()
{
    for (NetEntry &e : entries)
        if (e.media)
            libvlc_media_release(e.media);
    entries.clear();
}

void finish_listing()
{
    libvlc_media_parsed_status_t st = libvlc_media_get_parsed_status(listing);
    std::string err;
    {
        std::lock_guard<std::mutex> g(ask_lock);
        err = last_error;
    }
    if (st != libvlc_media_parsed_status_done) {
        list_state = NET_FAILED;
        list_serial++;
        list_error = !err.empty() ? err : st == libvlc_media_parsed_status_timeout ? "The server didn't answer." : "Couldn't open this place.";
        fprintf(stderr, "net: listing failed (%d): %s\n", (int)st, list_error.c_str());
        return;
    }
    clear_entries();
    libvlc_media_list_t *subs = libvlc_media_subitems(listing);
    if (subs) {
        libvlc_media_list_lock(subs);
        for (int i = 0; i < libvlc_media_list_count(subs); i++) {
            libvlc_media_t *m = libvlc_media_list_item_at_index(subs, i);
            if (!m)
                continue;
            char *mrl = libvlc_media_get_mrl(m), *title = libvlc_media_get_meta(m, libvlc_meta_Title);
            if (mrl) {
                NetEntry e;
                e.url = without_login(mrl);
                e.name = title ? title : e.url.substr(e.url.rfind('/') + 1);
                e.dir = libvlc_media_get_type(m) == libvlc_media_type_directory;
                /* Hidden files and Windows' admin shares (C$, IPC$) aren't for us. */
                if (!e.name.empty() && e.name[0] != '.' && e.name.back() != '$') {
                    e.media = m;
                    libvlc_media_retain(m);
                    entries.push_back(e);
                }
            }
            free(mrl);
            free(title);
            libvlc_media_release(m);
        }
        libvlc_media_list_unlock(subs);
        libvlc_media_list_release(subs);
    }
    /* Folders A-Z; a playlist (TV channels, radio) keeps its own order. */
    bool playlist = list_url.compare(0, 4, "http") == 0;
    if (!playlist)
        std::stable_sort(entries.begin(), entries.end(), [](const NetEntry &a, const NetEntry &b) {
            if (a.dir != b.dir)
                return a.dir;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    /* Nothing listed and VLC reported an error: it failed, it isn't empty. */
    if (entries.empty() && !err.empty()) {
        list_state = NET_FAILED;
        list_serial++;
        list_error = err;
        fprintf(stderr, "net: listing failed: %s\n", err.c_str());
        return;
    }
    list_state = NET_READY;
    list_serial++;
    fprintf(stderr, "net: %zu entries\n", entries.size());
}

/* ---- free TV and radio: public lists, read by VLC's playlist module ----
 * TV: iptv-org's lists of free channels (github.com/iptv-org/iptv).
 * Radio: radio-browser.info, a community list of stations. Both answer with
 * .m3u lists, so a country or a category is just another folder to list. */

struct Country {
    const char *name, *tv, *radio; /* iptv-org and radio-browser codes */
};
const Country countries[] = {
    { "Algeria", "dz", "DZ" }, { "Argentina", "ar", "AR" }, { "Australia", "au", "AU" },
    { "Austria", "at", "AT" }, { "Bahrain", "bh", "BH" }, { "Belgium", "be", "BE" },
    { "Brazil", "br", "BR" }, { "Canada", "ca", "CA" }, { "Chile", "cl", "CL" },
    { "China", "cn", "CN" }, { "Colombia", "co", "CO" }, { "Czechia", "cz", "CZ" },
    { "Denmark", "dk", "DK" }, { "Egypt", "eg", "EG" }, { "Finland", "fi", "FI" },
    { "France", "fr", "FR" }, { "Germany", "de", "DE" }, { "Greece", "gr", "GR" },
    { "Hungary", "hu", "HU" }, { "India", "in", "IN" }, { "Indonesia", "id", "ID" },
    { "Iran", "ir", "IR" }, { "Iraq", "iq", "IQ" }, { "Ireland", "ie", "IE" },
    { "Italy", "it", "IT" }, { "Japan", "jp", "JP" }, { "Jordan", "jo", "JO" },
    { "Kuwait", "kw", "KW" }, { "Lebanon", "lb", "LB" }, { "Libya", "ly", "LY" },
    { "Malaysia", "my", "MY" }, { "Mexico", "mx", "MX" }, { "Morocco", "ma", "MA" },
    { "Netherlands", "nl", "NL" }, { "New Zealand", "nz", "NZ" }, { "Nigeria", "ng", "NG" },
    { "Norway", "no", "NO" }, { "Oman", "om", "OM" }, { "Pakistan", "pk", "PK" },
    { "Palestine", "ps", "PS" }, { "Peru", "pe", "PE" }, { "Philippines", "ph", "PH" },
    { "Poland", "pl", "PL" }, { "Portugal", "pt", "PT" }, { "Qatar", "qa", "QA" },
    { "Romania", "ro", "RO" }, { "Russia", "ru", "RU" }, { "Saudi Arabia", "sa", "SA" },
    { "South Africa", "za", "ZA" }, { "South Korea", "kr", "KR" }, { "Spain", "es", "ES" },
    { "Sweden", "se", "SE" }, { "Switzerland", "ch", "CH" }, { "Syria", "sy", "SY" },
    { "Thailand", "th", "TH" }, { "Tunisia", "tn", "TN" }, { "Turkey", "tr", "TR" },
    { "Ukraine", "ua", "UA" }, { "United Arab Emirates", "ae", "AE" },
    { "United Kingdom", "uk", "GB" }, { "United States", "us", "US" }, { "Vietnam", "vn", "VN" },
    { "Yemen", "ye", "YE" },
};
const char *const tv_categories[] = {
    "Animation", "Auto", "Business", "Classic", "Comedy", "Cooking", "Culture", "Documentary",
    "Education", "Entertainment", "Family", "General", "Kids", "Legislative", "Lifestyle",
    "Movies", "Music", "News", "Outdoor", "Relax", "Religious", "Science", "Series", "Shop",
    "Sports", "Travel", "Weather",
};
const char *const TV_LISTS = "https://iptv-org.github.io/iptv/";
const char *const RADIO_LISTS = "https://de1.api.radio-browser.info/m3u/stations/";
const char *const RADIO_OPTIONS = "?limit=300&order=clickcount&reverse=true&hidebroken=true";

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

/* The places that are our own menus ("tv:", "radio:"): listed at once. */
bool list_menu(const std::string &url)
{
    if (url.compare(0, 3, "tv:") != 0 && url.compare(0, 6, "radio:") != 0)
        return false;
    clear_entries();
    if (url == "tv:") {
        entries.push_back({ "By country", "tv:countries", true });
        entries.push_back({ "By category", "tv:categories", true });
        entries.push_back({ "Every channel", std::string(TV_LISTS) + "index.m3u", true });
    } else if (url == "tv:countries") {
        for (const Country &c : countries)
            entries.push_back({ c.name, std::string(TV_LISTS) + "countries/" + c.tv + ".m3u", true });
    } else if (url == "tv:categories") {
        for (const char *c : tv_categories)
            entries.push_back({ c, std::string(TV_LISTS) + "categories/" + lower(c) + ".m3u", true });
    } else if (url == "radio:") {
        entries.push_back({ "Most listened", std::string(RADIO_LISTS) + "topclick/300", true });
        entries.push_back({ "By country", "radio:countries", true });
    } else if (url == "radio:countries") {
        for (const Country &c : countries)
            entries.push_back({ c.name, std::string(RADIO_LISTS) + "bycountrycodeexact/" + c.radio + RADIO_OPTIONS, true });
    }
    list_state = NET_READY;
    list_serial++;
    return true;
}

} // namespace

void net_init()
{
    load_saved();
    /* HTTPS (GnuTLS) takes its randomness from /dev/urandom on the console. */
    if (FILE *r = fopen("/dev/urandom", "rb")) {
        unsigned char b[16];
        size_t n = fread(b, 1, sizeof(b), r);
        fclose(r);
        fprintf(stderr, "net: /dev/urandom %s\n", n == sizeof(b) ? "ok" : "gives nothing: HTTPS won't work");
    } else {
        fprintf(stderr, "net: no /dev/urandom: HTTPS won't work\n");
    }
    libvlc_instance_t *vlc = player_vlc();
    if (!vlc)
        return;
    libvlc_dialog_set_callbacks(vlc, &dialog_cbs, nullptr);
    /* UPnP's start can take seconds (it waits for the network): not on the
     * interface's thread. */
    upnp_starter = std::thread([vlc] {
        libvlc_media_discoverer_t *md = libvlc_media_discoverer_new(vlc, "upnp");
        if (md && libvlc_media_discoverer_start(md) == 0) {
            upnp = md;
            fprintf(stderr, "net: looking for DLNA servers\n");
        } else {
            fprintf(stderr, "net: no DLNA discovery\n");
            if (md)
                libvlc_media_discoverer_release(md);
        }
    });
}

void net_shutdown()
{
    if (upnp_starter.joinable())
        upnp_starter.join();
    net_list_stop();
    clear_entries();
    if (upnp) {
        libvlc_media_discoverer_stop(upnp);
        libvlc_media_discoverer_release(upnp);
        upnp = nullptr;
    }
    if (libvlc_instance_t *vlc = player_vlc())
        libvlc_dialog_set_callbacks(vlc, nullptr, nullptr);
}

void net_update()
{
    double t = plat_time();
    if (t >= next_poll) {
        next_poll = t + 1;
        poll_found();
    }
    std::vector<libvlc_dialog_id *> dismiss;
    {
        std::lock_guard<std::mutex> g(ask_lock);
        dismiss.swap(to_dismiss);
    }
    for (libvlc_dialog_id *id : dismiss)
        libvlc_dialog_dismiss(id);
    if (list_state == NET_LOADING && libvlc_media_get_parsed_status(listing) != 0)
        finish_listing();
}

std::vector<NetServer> net_servers()
{
    std::vector<NetServer> out;
    for (const Saved &s : saved)
        out.push_back({ s.name, s.url, true });
    std::lock_guard<std::mutex> g(found_lock);
    out.insert(out.end(), found.begin(), found.end());
    return out;
}

std::string net_add_server(const std::string &address)
{
    std::string a = address;
    while (!a.empty() && (a.back() == ' ' || a.back() == '/' || a.back() == '\\'))
        a.pop_back();
    while (!a.empty() && a[0] == ' ')
        a.erase(0, 1);
    if (a.compare(0, 2, "\\\\") == 0)
        a = a.substr(2);
    std::replace(a.begin(), a.end(), '\\', '/');
    if (a.find("://") == std::string::npos)
        a = "smb://" + a;
    a = without_login(a);
    for (const Saved &s : saved)
        if (s.url == a)
            return a;
    std::string name = a.substr(a.find("://") + 3);
    std::replace(name.begin(), name.end(), '/', ' ');
    saved.push_back({ name, a, "", "" });
    store_saved();
    return a;
}

void net_forget_server(const std::string &url)
{
    saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Saved &s) { return s.url == url; }),
                saved.end());
    store_saved();
}

bool net_is_saved_server(const std::string &url)
{
    for (const Saved &s : saved)
        if (s.url == url)
            return true;
    return false;
}

libvlc_media_t *net_take_media(const std::string &url)
{
    for (NetEntry &e : entries)
        if (e.url == url && e.media) {
            libvlc_media_retain(e.media);
            return e.media;
        }
    return nullptr;
}

std::vector<std::string> net_login_options(const std::string &url)
{
    const Saved *s = saved_for(url);
    if (!s || s->user.empty())
        return {};
    return { ":smb-user=" + s->user, ":smb-pwd=" + s->password };
}

void net_list(const std::string &url)
{
    net_list_stop();
    libvlc_instance_t *vlc = player_vlc();
    clear_entries();
    list_url = url;
    list_error.clear();
    {
        std::lock_guard<std::mutex> g(ask_lock);
        login_for = url;
        last_error.clear();
    }
    if (list_menu(url))
        return;
    listing = vlc ? libvlc_media_new_location(vlc, url.c_str()) : nullptr;
    if (!listing) {
        list_state = NET_FAILED;
        list_error = "Couldn't open this place.";
        return;
    }
    for (const std::string &o : net_login_options(url))
        libvlc_media_add_option(listing, o.c_str());
    /* No time limit: a login can take a while to type. ○ stops it. */
    if (libvlc_media_parse_with_options(listing,
            (libvlc_media_parse_flag_t)(libvlc_media_parse_network | libvlc_media_do_interact), 0) != 0) {
        list_state = NET_FAILED;
        list_error = "Couldn't open this place.";
        return;
    }
    list_state = NET_LOADING;
    list_serial++;
    list_started = plat_time();
    fprintf(stderr, "net: listing %s\n", url.c_str());
}

void net_list_stop()
{
    if (listing) {
        if (list_state == NET_LOADING)
            libvlc_media_parse_stop(listing);
        libvlc_media_release(listing);
        listing = nullptr;
    }
    list_state = NET_IDLE;
    list_serial++;
}

int net_list_serial()
{
    return list_serial;
}

NetListState net_list_state()
{
    return list_state;
}

const std::vector<NetEntry> &net_list_entries()
{
    return entries;
}

std::string net_list_error()
{
    return list_error;
}

NetAsk net_ask()
{
    std::lock_guard<std::mutex> g(ask_lock);
    return ask_id ? asking : NetAsk{};
}

void net_answer_login(const std::string &user, const std::string &password, bool remember)
{
    libvlc_dialog_id *id;
    std::string place;
    {
        std::lock_guard<std::mutex> g(ask_lock);
        id = ask_id;
        ask_id = nullptr;
        asking = NetAsk{};
        place = login_for;
    }
    if (!id)
        return;
    if (user.empty() && password.empty())
        libvlc_dialog_dismiss(id);
    else
        libvlc_dialog_post_login(id, user.c_str(), password.c_str(), true);
    /* Kept with the saved share being opened, for the next time. */
    if (remember && !user.empty())
        if (Saved *s = const_cast<Saved *>(saved_for(place))) {
            s->user = user;
            s->password = password;
            store_saved();
        }
}

void net_answer_question(int action)
{
    libvlc_dialog_id *id;
    {
        std::lock_guard<std::mutex> g(ask_lock);
        id = ask_id;
        ask_id = nullptr;
        asking = NetAsk{};
    }
    if (!id)
        return;
    if (action == 1 || action == 2)
        libvlc_dialog_post_action(id, action);
    else
        libvlc_dialog_dismiss(id);
}

bool net_pop_error(std::string *title, std::string *text)
{
    std::lock_guard<std::mutex> g(ask_lock);
    if (errors.empty())
        return false;
    *title = errors[0].first;
    *text = errors[0].second;
    errors.erase(errors.begin());
    return true;
}
