/*
 * VLC-PS5's subtitle downloads from OpenSubtitles: its REST API (v1) over our
 * own small HTTPS client (GnuTLS, which VLC's build already carries; the
 * certificates are the app's own cacert.pem), and a small JSON reader.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "osub.h"

#include <algorithm>
#include <ctype.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "lang.h"
#include "platform.h"
#include "prefs.h"

namespace {

const char *API_HOST = "api.opensubtitles.com";
const char *USER_AGENT = "VLC-PS5 v0.1";

/* ---- what the interface sees ---- */

std::mutex lock;
OsubState state = OSUB_IDLE;
std::vector<SubResult> results;
std::string message, saved_path;
/* a signed-in account: its token and the server it was given */
std::string token, token_user, base_host;

void finish(OsubState s, const std::string &msg)
{
    std::lock_guard<std::mutex> g(lock);
    state = s;
    message = msg;
}

/* ---- HTTPS ---- */

struct Reply {
    int status = 0;
    std::string location;
    std::string body;
    std::string error;   /* couldn't talk to the server at all */
};

bool send_all(gnutls_session_t s, const std::string &data)
{
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = gnutls_record_send(s, data.data() + done, data.size() - done);
        if (n == GNUTLS_E_INTERRUPTED || n == GNUTLS_E_AGAIN)
            continue;
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

int connect_to(const std::string &host, int port)
{
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res)
        return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        struct timeval tv = { 15, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* the body of a chunked reply, joined */
std::string unchunk(const std::string &in)
{
    std::string out;
    size_t at = 0;
    while (at < in.size()) {
        size_t eol = in.find("\r\n", at);
        if (eol == std::string::npos)
            break;
        unsigned long n = strtoul(in.c_str() + at, nullptr, 16);
        if (n == 0)
            break;
        at = eol + 2;
        if (at + n > in.size())
            n = in.size() - at;
        out.append(in, at, n);
        at += n + 2;
    }
    return out;
}

Reply https(const std::string &method, const std::string &host, const std::string &path,
            const std::string &headers, const std::string &body)
{
    Reply r;
    static std::once_flag once;
    std::call_once(once, [] { gnutls_global_init(); });

    int fd = connect_to(host, 443);
    if (fd < 0) {
        r.error = trf("Couldn't reach %s", host.c_str());
        return r;
    }
    gnutls_certificate_credentials_t cred;
    gnutls_certificate_allocate_credentials(&cred);
    std::string ca = std::string(plat_data_dir()) + "/certs/cacert.pem";
    if (gnutls_certificate_set_x509_trust_file(cred, ca.c_str(), GNUTLS_X509_FMT_PEM) <= 0)
        fprintf(stderr, "osub: no certificates in %s\n", ca.c_str());

    gnutls_session_t s;
    gnutls_init(&s, GNUTLS_CLIENT);
    gnutls_set_default_priority(s);
    gnutls_credentials_set(s, GNUTLS_CRD_CERTIFICATE, cred);
    gnutls_server_name_set(s, GNUTLS_NAME_DNS, host.data(), host.size());
    gnutls_session_set_verify_cert(s, host.c_str(), 0);
    gnutls_transport_set_int(s, fd);
    gnutls_handshake_set_timeout(s, 15000);

    int ret;
    do
        ret = gnutls_handshake(s);
    while (ret < 0 && !gnutls_error_is_fatal(ret));
    if (ret < 0) {
        fprintf(stderr, "osub: TLS with %s: %s\n", host.c_str(), gnutls_strerror(ret));
        r.error = trf("Couldn't make a secure connection to %s", host.c_str());
    } else {
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nUser-Agent: " + USER_AGENT +
                          "\r\nAccept: */*\r\nConnection: close\r\n" + headers;
        if (!body.empty() || method == "POST")
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        if (!send_all(s, req)) {
            r.error = trf("The connection to %s broke", host.c_str());
        } else {
            std::string all;
            char buf[16384];
            for (;;) {
                ssize_t n = gnutls_record_recv(s, buf, sizeof buf);
                if (n == GNUTLS_E_INTERRUPTED || n == GNUTLS_E_AGAIN)
                    continue;
                if (n <= 0)   /* the end (or a server that just closes) */
                    break;
                all.append(buf, (size_t)n);
                if (all.size() > (32u << 20))
                    break;
            }
            size_t he = all.find("\r\n\r\n");
            if (he == std::string::npos) {
                r.error = trf("No answer from %s", host.c_str());
            } else {
                std::string head = all.substr(0, he + 2);
                r.body = all.substr(he + 4);
                sscanf(head.c_str(), "HTTP/%*s %d", &r.status);
                bool chunked = false;
                size_t at = head.find("\r\n");
                while (at != std::string::npos && at + 2 < head.size()) {
                    size_t eol = head.find("\r\n", at + 2);
                    std::string line = head.substr(at + 2, eol - at - 2);
                    at = eol;
                    size_t colon = line.find(':');
                    if (colon == std::string::npos)
                        continue;
                    std::string name = line.substr(0, colon), value = line.substr(colon + 1);
                    while (!value.empty() && value[0] == ' ')
                        value.erase(0, 1);
                    if (!strcasecmp(name.c_str(), "transfer-encoding") && value.find("chunked") != std::string::npos)
                        chunked = true;
                    else if (!strcasecmp(name.c_str(), "location"))
                        r.location = value;
                }
                if (chunked)
                    r.body = unchunk(r.body);
            }
        }
        gnutls_bye(s, GNUTLS_SHUT_WR);
    }
    gnutls_deinit(s);
    gnutls_certificate_free_credentials(cred);
    close(fd);
    return r;
}

/* splits https://host/path */
bool split_url(const std::string &url, std::string &host, std::string &path)
{
    if (url.compare(0, 8, "https://") != 0)
        return false;
    size_t slash = url.find('/', 8);
    host = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    return !host.empty();
}

/* a GET that follows redirects */
Reply https_get(std::string url)
{
    Reply r;
    for (int hop = 0; hop < 4; hop++) {
        std::string host, path;
        if (!split_url(url, host, path)) {
            r.error = trf("Unexpected address %s", url.c_str());
            return r;
        }
        r = https("GET", host, path, "", "");
        if (r.error.empty() && r.status >= 300 && r.status < 400 && !r.location.empty()) {
            url = r.location[0] == '/' ? "https://" + host + r.location : r.location;
            continue;
        }
        return r;
    }
    return r;
}

/* ---- JSON ---- */

struct Json {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    double num = 0;
    bool yes = false;
    std::string str;
    std::vector<Json> items;
    std::vector<std::string> keys;   /* an object: keys[i] names items[i] */

    const Json &operator[](const char *key) const
    {
        static const Json none;
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key)
                return items[i];
        return none;
    }
};

struct JsonReader {
    const char *p, *end;
    int depth = 0;

    void space()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    static void put_utf8(std::string &o, unsigned c)
    {
        if (c < 0x80) {
            o += (char)c;
        } else if (c < 0x800) {
            o += (char)(0xC0 | c >> 6);
            o += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += (char)(0xE0 | c >> 12);
            o += (char)(0x80 | (c >> 6 & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        } else {
            o += (char)(0xF0 | c >> 18);
            o += (char)(0x80 | (c >> 12 & 0x3F));
            o += (char)(0x80 | (c >> 6 & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        }
    }
    unsigned hex4()
    {
        unsigned v = 0;
        for (int i = 0; i < 4 && p < end; i++, p++) {
            char c = *p;
            v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    bool string(std::string &o)
    {
        if (p >= end || *p != '"')
            return false;
        p++;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                o += *p++;
                continue;
            }
            if (++p >= end)
                return false;
            char e = *p++;
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned c = hex4();
                if (c >= 0xD800 && c < 0xDC00 && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = hex4();
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(o, c);
                break;
            }
            default: o += e;
            }
        }
        if (p >= end)
            return false;
        p++;
        return true;
    }
    bool value(Json &v)
    {
        space();
        if (p >= end || ++depth > 64)
            return false;
        bool ok = true;
        if (*p == '{') {
            v.kind = Json::OBJ;
            p++;
            space();
            if (p < end && *p == '}') {
                p++;
            } else {
                for (;;) {
                    space();
                    std::string k;
                    if (!string(k))
                        return false;
                    space();
                    if (p >= end || *p++ != ':')
                        return false;
                    v.keys.push_back(k);
                    v.items.emplace_back();
                    if (!value(v.items.back()))
                        return false;
                    space();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        p++;
                        break;
                    }
                    return false;
                }
            }
        } else if (*p == '[') {
            v.kind = Json::ARR;
            p++;
            space();
            if (p < end && *p == ']') {
                p++;
            } else {
                for (;;) {
                    v.items.emplace_back();
                    if (!value(v.items.back()))
                        return false;
                    space();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        p++;
                        break;
                    }
                    return false;
                }
            }
        } else if (*p == '"') {
            v.kind = Json::STR;
            ok = string(v.str);
        } else if (end - p >= 4 && !strncmp(p, "true", 4)) {
            v.kind = Json::BOOL;
            v.yes = true;
            p += 4;
        } else if (end - p >= 5 && !strncmp(p, "false", 5)) {
            v.kind = Json::BOOL;
            p += 5;
        } else if (end - p >= 4 && !strncmp(p, "null", 4)) {
            p += 4;
        } else {
            std::string num;
            while (p < end && strchr("+-0123456789.eE", *p))
                num += *p++;
            v.kind = Json::NUM;
            v.num = strtod(num.c_str(), nullptr);
            ok = !num.empty();
        }
        depth--;
        return ok;
    }
};

bool parse_json(const std::string &text, Json &out)
{
    JsonReader r{ text.data(), text.data() + text.size() };
    return r.value(out);
}

std::string json_escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += (char)c;
        }
    }
    return o;
}

std::string url_escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)tolower(c);
        } else if (c == ' ') {
            o += '+';
        } else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

/* what the server said went wrong */
std::string server_error(const Reply &r, const char *what)
{
    if (!r.error.empty())
        return r.error;
    if (r.status == 401 || r.status == 403)
        return "OpenSubtitles refused the API key";
    Json j;
    if (parse_json(r.body, j)) {
        if (j["message"].kind == Json::STR)
            return j["message"].str;
        const Json &errs = j["errors"];
        if (errs.kind == Json::ARR && !errs.items.empty() && errs.items[0].kind == Json::STR)
            return errs.items[0].str;
    }
    if (r.status == 429)
        return tr("OpenSubtitles is busy: try again in a moment");
    return trf("%s failed (%d)", tr(what), r.status);
}

/* ---- the API ---- */

/* The OpenSubtitles hash: the file's size plus the 64-bit words of its first
 * and last 64 KiB. Empty when it can't be read. */
std::string movie_hash(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return "";
    fseek(f, 0, SEEK_END);
    long long size = ftello(f);
    uint64_t hash = (uint64_t)size;
    const size_t chunk = 65536;
    bool ok = size >= (long long)chunk;
    std::vector<unsigned char> buf(chunk);
    for (int part = 0; ok && part < 2; part++) {
        fseeko(f, part ? size - (long long)chunk : 0, SEEK_SET);
        if (fread(buf.data(), 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < chunk; i += 8) {
            uint64_t w = 0;
            for (int b = 7; b >= 0; b--)
                w = w << 8 | buf[i + b];
            hash += w;
        }
    }
    fclose(f);
    if (!ok)
        return "";
    char out[20];
    snprintf(out, sizeof out, "%016llx", (unsigned long long)hash);
    return out;
}

struct Account {
    std::string key, user, pass;
};

Account account()
{
    return { pref_str("osub_key", ""), pref_str("osub_user", ""), pref_str("osub_pass", "") };
}

std::string api_headers(const Account &a, const std::string &bearer)
{
    std::string h = "Api-Key: " + a.key + "\r\nContent-Type: application/json\r\n";
    if (!bearer.empty())
        h += "Authorization: Bearer " + bearer + "\r\n";
    return h;
}

/* Signs in (once per account); false with msg when it can't. */
bool sign_in(const Account &a, std::string &msg, std::string &bearer, std::string &host)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (!token.empty() && token_user == a.user) {
            bearer = token;
            host = base_host;
            return true;
        }
    }
    std::string body = "{\"username\":\"" + json_escape(a.user) + "\",\"password\":\"" + json_escape(a.pass) + "\"}";
    Reply r = https("POST", API_HOST, "/api/v1/login", api_headers(a, ""), body);
    Json j;
    if (!r.error.empty() || r.status != 200 || !parse_json(r.body, j) || j["token"].kind != Json::STR) {
        msg = r.status == 401 ? "OpenSubtitles: wrong user name or password" : server_error(r, "Signing in");
        return false;
    }
    bearer = j["token"].str;
    host = j["base_url"].kind == Json::STR && !j["base_url"].str.empty() ? j["base_url"].str : API_HOST;
    if (host.compare(0, 8, "https://") == 0)
        host = host.substr(8);
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    const Json &u = j["user"];
    if (u["allowed_downloads"].kind == Json::NUM)
        msg = trf("Signed in: %d downloads a day", (int)u["allowed_downloads"].num);
    else
        msg = "Signed in";
    std::lock_guard<std::mutex> g(lock);
    token = bearer;
    token_user = a.user;
    base_host = host;
    return true;
}

bool start()
{
    std::lock_guard<std::mutex> g(lock);
    if (state == OSUB_BUSY)
        return false;
    state = OSUB_BUSY;
    message.clear();
    return true;
}

}  // namespace

bool osub_has_key()
{
    return !pref_str("osub_key", "").empty();
}

void osub_search(const std::string &path, const std::string &name, const std::string &languages)
{
    if (!start())
        return;
    {
        std::lock_guard<std::mutex> g(lock);
        results.clear();
    }
    Account a = account();
    std::thread([a, path, name, languages] {
        std::string hash = path.find("://") == std::string::npos ? movie_hash(path) : "";
        /* the server wants its parameters in order, lower case */
        std::string q = "/api/v1/subtitles?";
        if (!languages.empty())
            q += "languages=" + url_escape(languages) + "&";
        if (!hash.empty())
            q += "moviehash=" + hash + "&";
        q += "query=" + url_escape(name);
        Reply r = https("GET", API_HOST, q, api_headers(a, ""), "");
        if (r.error.empty() && r.status >= 300 && r.status < 400 && !r.location.empty()) {
            std::string host, p;
            if (r.location[0] == '/')
                r = https("GET", API_HOST, r.location, api_headers(a, ""), "");
            else if (split_url(r.location, host, p))
                r = https("GET", host, p, api_headers(a, ""), "");
        }
        Json j;
        if (!r.error.empty() || r.status != 200 || !parse_json(r.body, j)) {
            finish(OSUB_FAILED, server_error(r, "The search"));
            return;
        }
        std::vector<SubResult> found;
        for (const Json &d : j["data"].items) {
            const Json &at = d["attributes"];
            const Json &files = at["files"];
            if (files.items.empty() || files.items[0]["file_id"].kind != Json::NUM)
                continue;
            SubResult s;
            s.file_id = (long long)files.items[0]["file_id"].num;
            s.language = at["language"].str;
            s.release = at["release"].str;
            if (s.release.empty())
                s.release = files.items[0]["file_name"].str;
            s.downloads = (int)at["download_count"].num;
            s.hash_match = at["moviehash_match"].yes;
            found.push_back(s);
        }
        /* exact matches first, then the most downloaded */
        std::stable_sort(found.begin(), found.end(), [](const SubResult &x, const SubResult &y) {
            if (x.hash_match != y.hash_match)
                return x.hash_match;
            return x.downloads > y.downloads;
        });
        {
            std::lock_guard<std::mutex> g(lock);
            results = found;
        }
        finish(OSUB_DONE, found.empty() ? "No subtitles found" : "");
    }).detach();
}

void osub_download(const SubResult &res, const std::string &save_base, const std::string &fallback_base)
{
    if (!start())
        return;
    Account a = account();
    std::thread([a, res, save_base, fallback_base] {
        std::string msg, bearer, host = API_HOST;
        if (!a.user.empty() && !sign_in(a, msg, bearer, host)) {
            finish(OSUB_FAILED, msg);
            return;
        }
        std::string body = "{\"file_id\":" + std::to_string(res.file_id) + "}";
        Reply r = https("POST", host, "/api/v1/download", api_headers(a, bearer), body);
        Json j;
        if (!r.error.empty() || r.status != 200 || !parse_json(r.body, j) || j["link"].kind != Json::STR) {
            finish(OSUB_FAILED, r.status == 406 || r.status == 429 ? server_error(r, "The download") +
                                                         (a.user.empty() ? std::string(" ") + tr("(an account gives more a day)") : "")
                                                       : server_error(r, "The download"));
            return;
        }
        std::string left = j["remaining"].kind == Json::NUM
                               ? trf("%d downloads left today", (int)j["remaining"].num) : "";
        Reply f = https_get(j["link"].str);
        if (!f.error.empty() || f.status != 200 || f.body.empty()) {
            finish(OSUB_FAILED, f.error.empty() ? "Couldn't fetch the subtitle file" : f.error);
            return;
        }
        std::string lang = res.language.empty() ? "sub" : res.language;
        std::string out;
        for (const std::string &base : { save_base, fallback_base }) {
            if (base.empty())
                continue;
            std::string p = base + "." + lang + ".srt";
            FILE *o = fopen(p.c_str(), "wb");
            if (!o)
                continue;
            bool ok = fwrite(f.body.data(), 1, f.body.size(), o) == f.body.size();
            ok = fclose(o) == 0 && ok;
            if (ok) {
                out = p;
                break;
            }
            remove(p.c_str());
        }
        if (out.empty()) {
            finish(OSUB_FAILED, "Couldn't save the subtitle file");
            return;
        }
        {
            std::lock_guard<std::mutex> g(lock);
            saved_path = out;
        }
        finish(OSUB_DONE, left);
    }).detach();
}

void osub_sign_in()
{
    if (!start())
        return;
    Account a = account();
    {
        std::lock_guard<std::mutex> g(lock);
        token.clear();
    }
    std::thread([a] {
        if (a.key.empty()) {
            finish(OSUB_FAILED, "Enter your API key first");
            return;
        }
        std::string msg, bearer, host;
        if (a.user.empty()) {
            /* no account: try the key alone with a small search (the
             * informational calls answer any key) */
            Reply r = https("GET", API_HOST, "/api/v1/subtitles?languages=en&query=vlc", api_headers(a, ""), "");
            finish(r.error.empty() && r.status == 200 ? OSUB_DONE : OSUB_FAILED,
                   r.error.empty() && r.status == 200 ? "The API key works" : server_error(r, "Checking the key"));
            return;
        }
        bool ok = sign_in(a, msg, bearer, host);
        finish(ok ? OSUB_DONE : OSUB_FAILED, msg);
    }).detach();
}

OsubState osub_state()
{
    std::lock_guard<std::mutex> g(lock);
    OsubState s = state;
    /* a finished job is reported once */
    if (s == OSUB_DONE || s == OSUB_FAILED)
        state = OSUB_IDLE;
    return s;
}

std::vector<SubResult> osub_results()
{
    std::lock_guard<std::mutex> g(lock);
    return results;
}

std::string osub_message()
{
    std::lock_guard<std::mutex> g(lock);
    return message;
}

std::string osub_saved_path()
{
    std::lock_guard<std::mutex> g(lock);
    std::string p = saved_path;
    saved_path.clear();
    return p;
}
