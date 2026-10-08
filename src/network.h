/*
 * VLC-PS5's network places: SMB shares the user adds (remembered with their
 * login), DLNA media servers found on the network, and folder listings of
 * both through libvlc (VLC's smb2 and upnp modules do the talking).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <string>
#include <vector>

struct NetServer {
    std::string name;
    std::string url;   /* smb://host/share or upnp://..., no login in it */
    bool saved;        /* added by the user (an SMB share); else found (DLNA) */
};

struct libvlc_media_t;

struct NetEntry {
    std::string name;
    std::string url;   /* no login in it: net_login_options() gives it */
    bool dir;
    libvlc_media_t *media = nullptr; /* VLC's own item (keeps a list's #EXTVLCOPT) */
};

enum NetListState { NET_IDLE, NET_LOADING, NET_READY, NET_FAILED };

/* A question VLC asks: a login, or yes/no (an untrusted certificate). */
struct NetAsk {
    int serial;               /* 0: nothing is being asked */
    bool login;
    std::string title, text, user;
    std::string actions[2];   /* questions: the buttons ("" if absent) */
    std::string cancel;
};

void net_init();      /* after player_init(): dialogs, DLNA discovery */
void net_shutdown();
void net_update();    /* main thread, every frame */

std::vector<NetServer> net_servers();
/* "192.168.1.20", "\\NAS\Movies", "smb://nas/Movies" -> smb://...; saved. */
std::string net_add_server(const std::string &address);
void net_forget_server(const std::string &url);
bool net_is_saved_server(const std::string &url);

/* Lists a folder (or a server's shares) in the background. */
void net_list(const std::string &url);
void net_list_stop();
NetListState net_list_state();
int net_list_serial();   /* changes whenever the listing does */
const std::vector<NetEntry> &net_list_entries();
std::string net_list_error();

/* The listed entry's media, with a reference for the caller (or null). */
libvlc_media_t *net_take_media(const std::string &url);

/* The media options that log in to an address's saved share (":smb-user=..."). */
std::vector<std::string> net_login_options(const std::string &url);

/* What VLC is asking (login / question), answered from the interface. */
NetAsk net_ask();
void net_answer_login(const std::string &user, const std::string &password, bool remember);
void net_answer_question(int action); /* 1, 2, or 0 to cancel */
/* An error VLC reported ("SMB2 operation failed: ..."), once. */
bool net_pop_error(std::string *title, std::string *text);
