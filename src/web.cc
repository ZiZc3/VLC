/*
 * VLC-PS5's web page server: plain HTTP/1.1 on port 8080 for the local
 * network. One thread listens; each connection gets its own (at most 8), so a
 * long upload doesn't hold up the rest of the page. Uploads stream to "<name>.part" and
 * are renamed when complete, so the library never sees half a file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "web.h"

#include <algorithm>
#include <arpa/inet.h>
#include <ctype.h>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "platform.h"
#include "web_page.h"

namespace {

/* 8080 first; something else on the console may hold it (console run 11:
 * EADDRINUSE, etaHEN or a payload), then the next free one up to 8099. */
const int FIRST_PORT = 8080, LAST_PORT = 8099;
std::atomic<int> port{ 0 };
const int MAX_CONNECTIONS = 8;

std::mutex lock;
std::vector<WebPlace> places;
WebOsubInfo osub_info;
WebUpload upload;
bool finished;
/* an OpenSubtitles key and account sent from the page */
bool osub_sent;
std::string osub_key, osub_user, osub_pass;
std::string address;
double address_at = -100;

std::atomic<bool> running{ false };
std::atomic<int> connections{ 0 };
std::thread listener;
int listen_fd = -1;

/* ---- small helpers ---------------------------------------------------------- */

std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string query_value(const std::string &query, const char *key)
{
    std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos)
            end = query.size();
        if (query.compare(pos, k.size(), k) == 0)
            return url_decode(query.substr(pos + k.size(), end - pos - k.size()));
        pos = end + 1;
    }
    return "";
}

std::string json_string(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += (char)c;
        }
    }
    return out + "\"";
}

bool write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void respond(int fd, int code, const char *type, const std::string &body)
{
    const char *text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                     : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : "Internal Server Error";
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, text, type, body.size());
    if (write_all(fd, head, (size_t)n))
        write_all(fd, body.data(), body.size());
}

/* A file name from the page, with no way out of its folder. */
bool safe_name(const std::string &n)
{
    return !n.empty() && n != "." && n != ".." && n.find('/') == std::string::npos &&
           n.find('\\') == std::string::npos && n[0] != '.' && n.size() < 240;
}

bool place_path(const std::string &dir, std::string *path)
{
    std::lock_guard<std::mutex> g(lock);
    int i = atoi(dir.c_str());
    if (dir.empty() || i < 0 || i >= (int)places.size())
        return false;
    *path = places[i].path;
    return true;
}

/* "Film.mkv" taken: "Film (2).mkv". */
std::string free_name(const std::string &dir, const std::string &name)
{
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) != 0)
        return name;
    size_t dot = name.rfind('.');
    std::string base = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    for (int i = 2; i < 1000; i++) {
        std::string n = base + " (" + std::to_string(i) + ")" + ext;
        if (stat((dir + "/" + n).c_str(), &st) != 0)
            return n;
    }
    return name;
}

/* The console's address on the network: a UDP socket "connected" towards
 * the internet picks the interface; nothing is sent. */
std::string find_address()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return "";
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x01010101); /* 1.1.1.1 */
    std::string out;
    if (connect(s, (sockaddr *)&to, sizeof(to)) == 0) {
        sockaddr_in me = {};
        socklen_t len = sizeof(me);
        if (getsockname(s, (sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr != 0) {
            uint32_t a = ntohl(me.sin_addr.s_addr);
            char b[32];
            snprintf(b, sizeof(b), "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
            out = b;
        }
    }
    close(s);
    return out;
}

/* ---- requests ---------------------------------------------------------------- */

void handle_upload(int fd, const std::string &query, int64_t length, std::string &body_start)
{
    std::string dir, name = query_value(query, "name");
    if (!place_path(query_value(query, "dir"), &dir) || !safe_name(name) || length <= 0) {
        respond(fd, 400, "text/plain", "That name or folder can't be used");
        return;
    }
    name = free_name(dir, name);
    std::string part = dir + "/" + name + ".part";
    int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        respond(fd, 500, "text/plain", "Can't write to that folder");
        return;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        upload = { true, name, 0, length };
    }
    fprintf(stderr, "web: receiving %s (%lld bytes) into %s\n", name.c_str(), (long long)length, dir.c_str());
    int64_t done = 0;
    bool ok = true;
    if (!body_start.empty()) {
        size_t n = (size_t)std::min<int64_t>((int64_t)body_start.size(), length);
        ok = write_all(out, body_start.data(), n);
        done += (int64_t)n;
    }
    std::vector<char> buf(1 << 20);
    while (ok && done < length) {
        ssize_t r = read(fd, buf.data(), (size_t)std::min<int64_t>((int64_t)buf.size(), length - done));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            ok = false;
            break;
        }
        ok = write_all(out, buf.data(), (size_t)r);
        done += r;
        std::lock_guard<std::mutex> g(lock);
        upload.done = done;
    }
    ok = close(out) == 0 && ok;
    if (ok)
        ok = rename(part.c_str(), (dir + "/" + name).c_str()) == 0;
    if (!ok)
        unlink(part.c_str());
    {
        std::lock_guard<std::mutex> g(lock);
        upload.active = false;
        if (ok)
            finished = true;
    }
    fprintf(stderr, "web: %s %s\n", name.c_str(), ok ? "received" : "FAILED (connection or disk)");
    respond(fd, ok ? 200 : 500, "text/plain", ok ? "ok" : "The upload stopped: connection or disk full");
}

/* A file from the PS5 to the phone or computer: streamed from the disk in
 * 1 MiB pieces, so any size goes (a 40 GB film too), as an attachment so
 * the browser saves it instead of playing it. */
void handle_download(int fd, const std::string &query)
{
    std::string dir, name = query_value(query, "name");
    if (!place_path(query_value(query, "dir"), &dir) || !safe_name(name)) {
        respond(fd, 400, "text/plain", "That name or folder can't be used");
        return;
    }
    std::string path = dir + "/" + name;
    int in = open(path.c_str(), O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (in >= 0)
            close(in);
        respond(fd, 404, "text/plain", "not found");
        return;
    }
    /* the name for the browser: plain letters as they are, the rest %-coded */
    std::string coded;
    for (unsigned char c : name) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        coded += isalnum(c) || strchr("-._~", c) ? std::string(1, (char)c) : std::string(hex);
    }
    char head[1024];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
                     "Content-Disposition: attachment; filename*=UTF-8''%s\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     (long long)st.st_size, coded.c_str());
    struct timeval tv = { 60, 0 }; /* a phone that stops reading lets go of the thread */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool ok = n > 0 && n < (int)sizeof(head) && write_all(fd, head, (size_t)n);
    std::vector<char> buf(1 << 20);
    int64_t sent = 0;
    while (ok) {
        ssize_t r = read(in, buf.data(), buf.size());
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        ok = write_all(fd, buf.data(), (size_t)r);
        sent += r;
    }
    close(in);
    fprintf(stderr, "web: %s %s\n", name.c_str(), sent == st.st_size ? "sent" : "stopped (connection)");
}

void handle_files(int fd, const std::string &query)
{
    std::string dir;
    if (!place_path(query_value(query, "dir"), &dir)) {
        respond(fd, 400, "application/json", "[]");
        return;
    }
    std::vector<std::pair<std::string, int64_t>> files;
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            struct stat st;
            if (stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
                files.push_back({ e->d_name, (int64_t)st.st_size });
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end(), [](const std::pair<std::string, int64_t> &a,
                                             const std::pair<std::string, int64_t> &b) {
        return strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
    });
    std::string out = "[";
    for (size_t i = 0; i < files.size(); i++)
        out += (i ? "," : "") + std::string("{\"name\":") + json_string(files[i].first) +
               ",\"size\":" + std::to_string(files[i].second) + "}";
    respond(fd, 200, "application/json", out + "]");
}

void handle(int fd)
{
    struct timeval tv = { 60, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string req;
    char buf[8192];
    size_t head_end;
    while ((head_end = req.find("\r\n\r\n")) == std::string::npos) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0 || req.size() > 32768) {
            close(fd);
            return;
        }
        req.append(buf, (size_t)r);
    }
    std::string head = req.substr(0, head_end), body = req.substr(head_end + 4);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        close(fd);
        return;
    }
    std::string method = head.substr(0, sp1), target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string path = target.substr(0, target.find('?'));
    std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
    int64_t length = 0;
    for (size_t p = head.find("\r\n"); p != std::string::npos; p = head.find("\r\n", p + 2))
        if (!strncasecmp(head.c_str() + p + 2, "Content-Length:", 15))
            length = atoll(head.c_str() + p + 17);

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", WEB_PAGE);
    } else if (method == "GET" && path == "/api/places") {
        std::string out = "[";
        std::lock_guard<std::mutex> g(lock);
        for (size_t i = 0; i < places.size(); i++)
            out += (i ? "," : "") + json_string(places[i].name);
        respond(fd, 200, "application/json", out + "]");
    } else if (method == "GET" && path == "/api/files") {
        handle_files(fd, query);
    } else if (method == "GET" && path == "/api/download") {
        handle_download(fd, query);
    } else if (method == "PUT" && path == "/api/upload") {
        handle_upload(fd, query, length, body);
    } else if (method == "DELETE" && path == "/api/file") {
        std::string dir, name = query_value(query, "name");
        bool ok = place_path(query_value(query, "dir"), &dir) && safe_name(name) &&
                  unlink((dir + "/" + name).c_str()) == 0;
        if (ok) {
            std::lock_guard<std::mutex> g(lock);
            finished = true; /* the library looks again */
        }
        respond(fd, ok ? 200 : 404, "text/plain", ok ? "ok" : "not found");
    } else if (method == "GET" && path == "/api/opensubtitles") {
        WebOsubInfo s;
        bool waiting;
        {
            std::lock_guard<std::mutex> g(lock);
            s = osub_info;
            waiting = osub_sent; /* sent, not yet taken by the main thread */
        }
        std::string out = std::string("{\"key\":") + (s.has_key ? "true" : "false") +
                          ",\"checking\":" + (s.checking || waiting ? "true" : "false") +
                          ",\"failed\":" + (s.failed ? "true" : "false") +
                          ",\"user\":" + json_string(s.user) +
                          ",\"message\":" + json_string(s.message) + "}";
        respond(fd, 200, "application/json", out);
    } else if (method == "POST" && path == "/api/opensubtitles") {
        /* a small form: the rest of its body, then key, user, pass */
        while ((int64_t)body.size() < length && length < 4096) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0)
                break;
            body.append(buf, (size_t)r);
        }
        std::string key = query_value(body, "key");
        bool ok = !key.empty() && key.size() < 200;
        if (ok) {
            std::lock_guard<std::mutex> g(lock);
            osub_key = key;
            osub_user = query_value(body, "user");
            osub_pass = query_value(body, "pass");
            osub_sent = true;
        }
        respond(fd, ok ? 200 : 400, "text/plain", ok ? "ok" : "no key");
    } else if (method == "GET" && path == "/favicon.ico") {
        respond(fd, 404, "text/plain", "");
    } else {
        respond(fd, 404, "text/plain", "not found");
    }
    close(fd);
}

void listen_main()
{
    while (running) {
        pollfd p = { listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0)
            continue;
        int fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0)
            continue;
        if (connections >= MAX_CONNECTIONS) {
            close(fd);
            continue;
        }
        connections++;
        std::thread([fd] {
            handle(fd);
            connections--;
        }).detach();
    }
}

} // namespace

void web_start()
{
    if (running)
        return;
    signal(SIGPIPE, SIG_IGN); /* a phone closing the page mid-answer */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "web: no socket (errno %d)\n", errno);
        return;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int p = FIRST_PORT;
    for (; p <= LAST_PORT; p++) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(p);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_fd, (sockaddr *)&a, sizeof(a)) == 0)
            break;
        fprintf(stderr, "web: port %d taken (errno %d)\n", p, errno);
    }
    if (p > LAST_PORT || listen(listen_fd, 8) != 0) {
        fprintf(stderr, "web: no free port in %d-%d (errno %d)\n", FIRST_PORT, LAST_PORT, errno);
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    port = p;
    {
        std::lock_guard<std::mutex> g(lock);
        address_at = -100; /* the address shows the new port at once */
    }
    running = true;
    listener = std::thread(listen_main);
    fprintf(stderr, "web: listening on port %d\n", p);
}

void web_stop()
{
    if (!running)
        return;
    running = false;
    if (listener.joinable())
        listener.join();
    close(listen_fd);
    listen_fd = -1;
    fprintf(stderr, "web: stopped\n");
}

bool web_running()
{
    return running;
}

std::string web_address()
{
    /* Looked up again every 10 s: the console may join a network later. */
    double now = plat_time();
    std::lock_guard<std::mutex> g(lock);
    if (now - address_at > 10) {
        address_at = now;
        std::string ip = find_address();
        address = ip.empty() ? "" : "http://" + ip + ":" + std::to_string(port.load());
    }
    return address;
}

void web_set_places(const std::vector<WebPlace> &p)
{
    std::lock_guard<std::mutex> g(lock);
    places = p;
}

void web_set_osub_info(const WebOsubInfo &info)
{
    std::lock_guard<std::mutex> g(lock);
    osub_info = info;
}

WebUpload web_upload_state()
{
    std::lock_guard<std::mutex> g(lock);
    return upload;
}

bool web_take_opensubtitles(std::string *key, std::string *user, std::string *pass)
{
    std::lock_guard<std::mutex> g(lock);
    if (!osub_sent)
        return false;
    osub_sent = false;
    *key = osub_key;
    *user = osub_user;
    *pass = osub_pass;
    osub_pass.clear();
    return true;
}

bool web_take_finished()
{
    std::lock_guard<std::mutex> g(lock);
    bool f = finished;
    finished = false;
    return f;
}
