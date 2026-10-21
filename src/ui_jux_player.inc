/*
 * VLC-PS5: "Redesign by Juxtopposed" (PLAN-REDESIGN.txt): the player's bar
 * and its side panel. Included into ui.cc just before modal_tracks (it uses
 * the panel's entries and settings, declared above it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* ---- the player (the design's Player frame) ------------------------------------------ */

/* the design's sound icon on the bar: the audio button (a waveform) */
void jux_icon_wave(float cx, float cy, float s, ImU32 col)
{
    static const float hgt[7] = { 0.25f, 0.55f, 0.85f, 1.0f, 0.85f, 0.55f, 0.25f };
    for (int i = 0; i < 7; i++) {
        float x = cx + (i - 3) * s * 0.15f, h = s * 0.42f * hgt[i];
        line(x, cy - h, x, cy + h, col, 2.2f);
    }
}

/* the volume as the design's wedge: dark, filled from green towards orange */
void jux_volume(float x, float cy, float w, float a)
{
    float h = 26, lvl = std::min(1.0f, player_volume() / 200.0f);
    auto wedge = [&](float x1, ImU32 col) {
        float t = (x1 - x) / w;
        dl->AddQuadFilled(P(x, cy + h / 2), P(x1, cy + h / 2 - h * t), P(x1, cy + h / 2), P(x1, cy + h / 2), col);
        dl->AddTriangleFilled(P(x, cy + h / 2), P(x1, cy + h / 2 - h * t), P(x1, cy + h / 2), col);
    };
    wedge(x + w, alpha(jux_light ? J_EDGE : IM_COL32(0, 0, 0, 255), 0.8f * a));
    if (lvl > 0.005f)
        wedge(x + w * lvl, alpha(mix(IM_COL32(126, 157, 18, 255), C_ORANGE, lvl), a));
}

double jux_osd_at = -10;   /* the controls came up */

void player_overlay_jux(float a, int64_t t, int64_t len, bool paused, const std::string &meta)
{
    if (a < 0.05f && now - jux_osd_at > 0.5)
        jux_osd_at = now;
    /* one part after another as the controls come up */
    auto k = [&](int order) { return a * ease(((float)(now - jux_osd_at) - 0.035f * order) / 0.35f); };

    /* top: the title in the middle (the design's title bar), the time on the right */
    if (jux_light) {
        /* the light frame: a light title bar over the picture */
        rect(0, 0, 1920, 116, alpha(J_PANEL, 0.96f * a));
        rect(0, 116, 1920, 1, alpha(J_LINE, a));
    } else {
        dl->AddRectFilledMultiColor(P(0, 0), P(1920, 180), alpha(IM_COL32(0, 0, 0, 255), 0.75f * a),
                                    alpha(IM_COL32(0, 0, 0, 255), 0.75f * a), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    }
    float ty = 34 - (1 - k(0)) * 16;
    text(f_reg, 28, 960, ty, alpha(J_TEXT, k(0)), playing_name.c_str(), 0.5f, 1300);
    text(f_reg, 19, 960, ty + 42, alpha(J_DIM, k(1)), meta.c_str(), 0.5f, 1300);
    time_t tt = time(nullptr);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d", tmv.tm_hour, tmv.tm_min);
    text(f_time, 28, 1880, ty, alpha(J_TEXT, k(0)), clock, 1);
    if (len > 0 && !paused) {
        time_t ends = tt + (time_t)((len - t) / 1000 / std::max(0.25f, player_rate()));
        localtime_r(&ends, &tmv);
        char e[32];
        snprintf(e, sizeof(e), tr("Ends at %d:%02d"), tmv.tm_hour, tmv.tm_min);
        text(f_reg, 19, 1880, ty + 42, alpha(J_DIM, k(1)), e, 1);
    }

    /* the bar: one row along the bottom, rising into place */
    const float bh = 100;
    float by = 1080 - bh + (1 - a) * 40, cy = by + bh / 2;
    rect(0, by, 1920, bh, alpha(J_PANEL, 0.96f * a));
    rect(0, by, 1920, 1, alpha(J_LINE, a));

    /* the buttons (osd_col 0..9): previous, back 10, play, forward 10, next |
     * loop, audio, subtitles, playback, picture */
    static const float bx[10] = { 52, 112, 196, 280, 340, 1652, 1708, 1764, 1820, 1876 };
    static const int order[10] = { 0, 1, 2, 3, 4, 9, 10, 11, 12, 13 };
    int repeat_mode = loop_on() ? 2 : 0;
    for (int i = 0; i < 10; i++) {
        bool foc = osd_row == 1 && osd_col == i && modal == NONE;
        float f = ease(focus_anim(800 + i, foc, 16));
        float ka = k(order[i]), x = bx[i], y = cy + (1 - ka) * 12;
        ImU32 ic = alpha(mix(J_DIM, i == 2 ? J_INK : J_TEXT, i == 2 ? 1.0f : f), ka);
        if (i == 2) {
            /* the play button: the accent pill, breathing while it has the focus */
            float pw = 96 + 8 * f, ph = 52 + 4 * f;
            float breathe = 0.8f + 0.2f * sinf((float)now * 2.6f);
            if (f > 0.01f)
                glow(x - pw / 2, y - ph / 2, pw, ph, 12, alpha(C_ORANGE, 0.55f * f * breathe * ka), 16, 6);
            rect(x - pw / 2, y - ph / 2, pw, ph, alpha(C_ORANGE, ka), 12);
            if (paused)
                icon_play(x + 3, y, 26, ic);
            else
                icon_pause(x, y, 22, ic);
            continue;
        }
        if (f > 0.01f) {
            glow(x - 26, y - 26, 52, 52, 10, alpha(C_ORANGE, 0.3f * f * ka), 10, 5);
            rect(x - 26, y - 26, 52, 52, alpha(J_RAISED, f * ka), 10);
            ic = alpha(mix(J_DIM, C_ORANGE, f), ka);
        }
        switch (i) {
        case 0: icon_skip(x, y, 22, ic, false); break;
        case 1: icon_jump(x, y, 30, ic, false, "10"); break;
        case 3: icon_jump(x, y, 30, ic, true, "10"); break;
        case 4: icon_skip(x, y, 22, ic, true); break;
        case 5: icon_repeat(x, y, 28, repeat_mode && f < 0.5f ? alpha(C_ORANGE, ka) : ic, repeat_mode); break;
        case 6: jux_icon_wave(x, y, 30, ic); break;
        case 7: icon_cc(x, y, 30, ic); break;
        case 8: icon_gear(x, y, 28, ic); break;
        case 9: icon_picture(x, y, 28, ic); break;
        }
    }
    /* the focused button's name, above the bar */
    static const char *const tips[10] = { "Previous", "Back 10 s", "Play / Pause", "Forward 10 s",
                                          "Next", "Loop", "Audio & volume", "Subtitles", "Playback", "Picture" };
    if (osd_row == 1 && modal == NONE && osd_col != 2 && osd_col < 10) {
        float tx = std::min(1860.0f, std::max(60.0f, bx[osd_col]));
        float tw = text_size(f_reg, 19, tips[osd_col]).x + 24;
        tx = std::min(1900 - tw / 2, std::max(20 + tw / 2, tx));
        rect(tx - tw / 2, by - 50, tw, 34, alpha(J_RAISED, k(14)), 8);
        text(f_reg, 19, tx, by - 44, alpha(J_TEXT, k(14)), tips[osd_col], 0.5f);
    }

    /* times in DM Mono, the seek bar between them, the volume after */
    float ka = k(5);
    text(f_time, 24, 400, cy - 14 + (1 - ka) * 12, alpha(J_DIM, ka), format_time(scrubbing ? scrub_ms : t).c_str());
    const float sx = 476, sw = 1392 - 476, sh = 24;
    float fs = ease(focus_anim(700, osd_row == 0 && modal == NONE));
    float sy = cy - sh / 2 + (1 - k(6)) * 12, sa = k(6);
    rect(sx, sy, sw, sh, alpha(J_BAR, sa), 7);
    stroke(sx, sy, sw, sh, alpha(mix(J_LINE, C_ORANGE, 0.5f * fs), sa), 7, 1.5f);
    float frac = len > 0 ? std::min(1.0f, (float)t / len) : 0;
    float sfrac = len > 0 ? std::min(1.0f, (float)scrub_ms / len) : 0;
    float shown = scrubbing ? sfrac : frac;
    if (frac > 0)
        rect(sx + 3, sy + 3, (sw - 6) * frac, sh - 6, alpha(C_ORANGE2, sa), 5);
    if (scrubbing)
        rect(sx + 3 + (sw - 6) * std::min(frac, sfrac), sy + 3, (sw - 6) * fabsf(sfrac - frac), sh - 6,
             alpha(J_EDGE, sa), 5);
    if (len > 0) {
        for (const Chapter &c : player_chapters())
            if (c.start_ms > 0)
                rect(sx + sw * c.start_ms / len - 1, sy + 5, 2, sh - 10, alpha(J_DIM, sa));
        for (int64_t bm : library_bookmarks(playing_path)) {
            float bmx = sx + sw * bm / len;
            tri(bmx - 6, sy - 12, bmx + 6, sy - 12, bmx, sy - 3, alpha(J_TEXT, sa));
        }
        if (loop_a >= 0)
            rect(sx + sw * loop_a / len - 2, sy - 6, 4, sh + 12, alpha(C_ORANGE, sa), 2);
        if (loop_b >= 0)
            rect(sx + sw * loop_b / len - 2, sy - 6, 4, sh + 12, alpha(C_ORANGE, sa), 2);
    }
    /* the knob: the design's accent pill; bigger and glowing when the bar has the focus */
    float kx = sx + 3 + (sw - 6) * shown, kw = 9 + 5 * fs, kh = sh + 10 + 8 * fs;
    if (fs > 0.01f) {
        float breathe = 0.75f + 0.25f * sinf((float)now * 3.0f);
        glow(kx - kw / 2, cy - kh / 2, kw, kh, kw / 2, alpha(C_ORANGE, 0.6f * fs * breathe * sa), 12, 6);
    }
    rect(kx - kw / 2, cy - kh / 2, kw, kh, alpha(C_ORANGE, sa), kw / 2);
    if (scrubbing) {
        std::string s = format_time(scrub_ms);
        float tw = text_size(f_time, 26, s.c_str()).x + 30;
        rect(kx - tw / 2, sy - 64, tw, 44, alpha(J_RAISED, sa), 10);
        tri(kx - 8, sy - 20, kx + 8, sy - 20, kx, sy - 10, alpha(J_RAISED, sa));
        text(f_time, 26, kx, sy - 56, alpha(J_TEXT, sa), s.c_str(), 0.5f);
    }
    ka = k(7);
    if (len > 0)
        text(f_time, 24, 1460, cy - 14 + (1 - ka) * 12, alpha(J_DIM, ka), format_time(len).c_str(), 1);
    ka = k(8);
    {
        /* the volume: a button too (up / down move it while it's chosen) */
        float fv = ease(focus_anim(810, osd_row == 1 && osd_col == OSD_VOLUME && modal == NONE, 16));
        float vy = cy + (1 - ka) * 12;
        if (fv > 0.01f) {
            glow(1476, vy - 26, 152, 52, 10, alpha(C_ORANGE, 0.3f * fv * ka), 10, 5);
            rect(1476, vy - 26, 152, 52, alpha(J_RAISED, fv * ka), 10);
            /* its value, and which way is which */
            char vv[48];
            snprintf(vv, sizeof(vv), "%s  %d%%", tr("Volume"), player_volume());
            float tw = text_size(f_reg, 19, vv).x + 24;
            rect(1552 - tw / 2, by - 50, tw, 34, alpha(J_RAISED, fv * ka), 8);
            text(f_reg, 19, 1552, by - 44, alpha(J_TEXT, fv * ka), vv, 0.5f);
        }
        icon_speaker(1494, vy, 24, alpha(mix(J_DIM, C_ORANGE, fv), ka));
        jux_volume(1512, vy - 3, 104, ka);
    }

    /* (no button hints over the picture: the bar's own buttons say it, and
     * the frame step (square while paused) still works without one: the
     * user, it spoiled the view) */
}

/* ---- the player's side panel (the design's Subtitles / settings sidebar) ---------------- */

/* The design's sections in each tab (her Subtitles panel: Defaults,
 * Synchronization, Appearance): every line goes under one, in this order. */
int jux_group_of(const PanelEntry &p, const char **name)
{
    switch (p.kind) {
    case 1: case 2: case 6: *name = "Tracks"; return 0;
    case 3: *name = "Speed"; return 3;
    case 4: *name = "Shape"; return 0;
    case 5: case 10: *name = "Sound"; return 2;
    case 8: *name = "Chapters"; return 9;
    case 9: *name = "Bookmarks"; return 8;
    default: break;
    }
    switch (p.value) {
    case SET_SUB_PICK: case SET_OSUB: *name = "Tracks"; return 0;
    case SET_AUDIO_DELAY: case SET_SUB_DELAY: *name = "Synchronization"; return 1;
    case SET_EQ: case SET_NIGHT: case SET_SOUND_OUT: *name = "Sound"; return 2;
    case SET_SUB_SIZE: case SET_SUB_COLOUR: case SET_SUB_BOX: *name = "Appearance"; return 2;
    case SET_REPEAT: case SET_SHUFFLE: *name = "Order"; return 0;
    case SET_SLEEP: case SET_LOOP: *name = "Timers"; return 5;
    case SET_DISC_MENU: case SET_POPUP: *name = "Disc"; return 6;
    case SET_BOOKMARK: case SET_BM_CLEAR: *name = "Bookmarks"; return 8;
    case SET_UPSCALE: case SET_DEINT: *name = "Quality"; return 1;
    case SET_BRIGHT: case SET_CONTRAST: case SET_SAT: case SET_GAMMA: case SET_HUE: *name = "Adjustments"; return 2;
    case SET_ROTATE: case SET_FLIP: case SET_PIC_RESET: *name = "Orientation"; return 3;
    default: *name = ""; return 7;
    }
}

/* the tab's lines under their sections (the old section titles go) */
std::vector<PanelEntry> jux_regroup(const std::vector<PanelEntry> &e)
{
    std::vector<PanelEntry> out;
    for (int g = 0; g < 10; g++) {
        const char *title = nullptr;
        for (const PanelEntry &p : e) {
            const char *name;
            if (!p.kind || jux_group_of(p, &name) != g)
                continue;
            if (!title) {
                title = name;
                PanelEntry h = {};
                h.kind = 0;
                h.label = name;
                out.push_back(h);
            }
            out.push_back(p);
        }
    }
    return out;
}

/* lines that are a number in a range: a slider under them, as the design's
 * subtitle delay. lo / hi / now, and the end labels */
bool jux_range(int id, float *lo, float *hi, float *v, const char **l0, const char **l1)
{
    PictureAdjust pa = player_picture();
    switch (id) {
    case SET_AUDIO_DELAY: *lo = -10000, *hi = 10000, *v = (float)player_audio_delay(), *l0 = "-10 s", *l1 = "+10 s"; return true;
    case SET_SUB_DELAY: *lo = -10000, *hi = 10000, *v = (float)player_subtitle_delay(), *l0 = "-10 s", *l1 = "+10 s"; return true;
    case SET_BRIGHT: *lo = -100, *hi = 100, *v = (float)pa.brightness, *l0 = "-100", *l1 = "+100"; return true;
    case SET_CONTRAST: *lo = -100, *hi = 100, *v = (float)pa.contrast, *l0 = "-100", *l1 = "+100"; return true;
    case SET_SAT: *lo = -100, *hi = 100, *v = (float)pa.saturation, *l0 = "-100", *l1 = "+100"; return true;
    case SET_GAMMA: *lo = -100, *hi = 100, *v = (float)pa.gamma, *l0 = "-100", *l1 = "+100"; return true;
    case SET_HUE: *lo = -180, *hi = 180, *v = (float)pa.hue, *l0 = "-180\xC2\xB0", *l1 = "+180\xC2\xB0"; return true;
    default: return false;
    }
}

float jux_line_h(const PanelEntry &p)
{
    float lo, hi, v;
    const char *a, *b;
    if (!p.kind)
        return 92;
    if (p.kind == 10)
        return 112;
    return p.kind == 7 && jux_range(p.value, &lo, &hi, &v, &a, &b) ? 112 : 66;
}

/* the design's slider from the middle: a dark track, the part from 0 to the
 * value in the accent's dark tone, the knob a pill; the ends named under it */
void jux_center_slider(float x, float y, float w, float lo, float hi, float v, const char *l0, const char *l1, float f, float a)
{
    float t = std::min(1.0f, std::max(0.0f, (v - lo) / (hi - lo))), mid = (0 - lo) / (hi - lo);
    rect(x, y, w, 16, alpha(J_BAR, a), 6);
    float x0 = x + 2 + (w - 4) * std::min(t, mid), x1 = x + 2 + (w - 4) * std::max(t, mid);
    if (x1 - x0 > 1)
        rect(x0, y + 2, x1 - x0, 12, alpha(C_ORANGE2, a), 4);
    rect(x + w * mid - 1, y - 3, 2, 22, alpha(J_EDGE, a));
    float kx = x + 2 + (w - 4) * t;
    rect(kx - 4 - 2 * f, y - 6 - 2 * f, 8 + 4 * f, 28 + 4 * f, alpha(C_ORANGE, a), 4);
    text(f_time, 18, x, y + 24, alpha(J_DIM, a), l0);
    text(f_time, 18, x + w * mid, y + 24, alpha(J_DIM, a), "0", 0.5f);
    text(f_time, 18, x + w, y + 24, alpha(J_DIM, a), l1, 1);
}

/* the subtitle colours as colours: a square of it in the value box */
ImU32 jux_sub_colour(int c)
{
    static const ImU32 cols[SUB_COLOURS] = { IM_COL32(238, 238, 238, 255), IM_COL32(255, 225, 77, 255),
                                             IM_COL32(77, 225, 255, 255), IM_COL32(91, 227, 106, 255) };
    return c >= 0 && c < SUB_COLOURS ? cols[c] : cols[0];
}

double jux_panel_at = -10;
int jux_panel_seen = -1;
float jux_tab_x = -1, jux_tab_w = 0;

/* tabs in the design's order; panel_section's numbers (Audio, Subtitles, Playback, Picture) */
const int jux_panel_tabs[4] = { 2, 3, 0, 1 };
const char *const jux_panel_names[4] = { "Playback", "Video", "Audio", "Subtitles" };

void jux_tracks_draw(const std::vector<PanelEntry> &e, const std::vector<int> &focusable, float a)
{
    if (jux_panel_seen != panel_section) {
        jux_panel_seen = panel_section;
        jux_panel_at = now;
    }
    const float w = 640, m = 24;
    float x = 1920 - (w + m) * a, y0 = m, h = 1080 - 2 * m;
    dl->AddRectFilledMultiColor(P(x - 240, 0), P(1920, 1080), IM_COL32(0, 0, 0, 0), alpha(IM_COL32(0, 0, 0, 255), 0.55f * a),
                                alpha(IM_COL32(0, 0, 0, 255), 0.55f * a), IM_COL32(0, 0, 0, 0));
    glow(x, y0 + 10, w, h, 16, alpha(IM_COL32(0, 0, 0, 255), J_SHADE * 0.8f * a), 24, 6);
    rect(x, y0, w, h, alpha(J_PANEL, a), 16);

    /* the tabs: icon and name; the accent line under the chosen one glides */
    const float th = 84;
    rect(x, y0, w, th, alpha(J_TABS, a), 16);
    rect(x, y0 + th - 16, w, 16, alpha(J_TABS, a));
    rect(x, y0 + th, w, 1, alpha(J_LINE, a));
    float tw = w / 4;
    for (int i = 0; i < 4; i++) {
        bool on = jux_panel_tabs[i] == panel_section;
        float f = ease(focus_anim(31000 + i, on, 12));
        float cx = x + tw * i + tw / 2;
        ImU32 col = alpha(mix(J_DIM, J_TEXT, f), a);
        ImU32 ic = alpha(mix(J_DIM, C_ORANGE, f), a);
        float lw = text_size(f_reg, 21, jux_panel_names[i]).x;
        float ix = cx - (lw + 34) / 2 + 12;
        switch (i) {
        case 0: jux_tab_icon(VIDEOS, ix, y0 + th / 2, 22, ic, false); break;
        case 1: icon_picture(ix, y0 + th / 2, 24, ic); break;
        case 2: jux_icon_wave(ix, y0 + th / 2, 24, ic); break;
        case 3: icon_cc(ix, y0 + th / 2, 26, ic); break;
        }
        text(f_reg, 21, ix + 22, y0 + th / 2 - 13, col, jux_panel_names[i]);
        if (on) {
            float target = cx - (lw + 50) / 2;
            if (jux_tab_x < 0)
                jux_tab_x = target, jux_tab_w = lw + 50;
            jux_tab_x = approach(jux_tab_x, target, 14);
            jux_tab_w = approach(jux_tab_w, lw + 50, 14);
        }
    }
    rect(jux_tab_x, y0 + th - 3, jux_tab_w, 3, alpha(C_ORANGE, a), 1.5f);

    /* the rows: the section's title in the design's "Defaults" style, then
     * each line with its control on the right */
    static float scroll;
    float fy = 0, yy = 0;
    for (int i = 0; i < (int)e.size(); i++) {
        if (!focusable.empty() && i == focusable[modal_focus])
            fy = yy;
        yy += jux_line_h(e[i]) - (i == 0 && !e[i].kind ? 30 : 0);
    }
    scroll = approach(scroll, std::max(0.0f, fy - 560), 12);
    float top = y0 + th + 14;
    dl->PushClipRect(P(x, top), P(x + w, y0 + h - 70), true);
    float y = top + 12 - scroll;
    int order = 0;
    for (int i = 0; i < (int)e.size(); i++) {
        const PanelEntry &p = e[i];
        /* each line comes in from the right, one after another */
        float k = ease(((float)(now - jux_panel_at) - 0.03f * std::min(order, 12)) / 0.35f) * a;
        float dx = (1 - k) * 40;
        order++;
        if (!p.kind) {
            /* a section: its name like the design's "Defaults", a line above
             * all but the first */
            if (i > 0) {
                y += 18;
                rect(x + 28, y, w - 56, 1, alpha(J_LINE, k));
            } else {
                y -= 12;
            }
            text(f_semi, 25, x + 36 + dx, y + 24, alpha(J_TEXT, k), p.label.c_str());
            y += i > 0 ? 74 : 62;
            continue;
        }
        bool foc = !focusable.empty() && i == focusable[modal_focus];
        float f = ease(focus_anim(hash_id(p.kind == 5 ? "volume" : p.label, 600 + p.kind * 100 + p.value), foc));
        float rx = x + 18 + dx, rw = w - 36;
        if (f > 0.01f) {
            rect(rx, y, rw, 56, alpha(J_RAISED, f * k), 10);
            glow(rx, y + 14, 4, 28, 2, alpha(C_ORANGE, 0.5f * f * k), 8, 4);
            rect(rx, y + 14, 4, 28, alpha(C_ORANGE, f * k), 2);
        }
        ImU32 lab = alpha(mix(J_DIM, J_TEXT, 0.35f + 0.65f * f), k);
        float lo, hi, rv;
        const char *l0, *l1;
        if (p.kind == 10) {
            /* an equalizer band (the design's EQ side panel): its value, and
             * a slider from -20 to +20 dB */
            if (f > 0.01f)
                rect(rx, y + 56, rw, 112 - 66, alpha(J_RAISED, f * k), 10);
            text(f_reg, 22, rx + 22, y + 15, lab, p.label.c_str(), 0, rw - 280);
            jux_value_box(rx + rw - 22, y + 8, 180, p.shown, f, k);
            jux_center_slider(rx + 22, y + 66, rw - 44, -20, 20, player_eq_amp(p.value), "-20 dB", "+20 dB", f, k);
            y += 112;
            continue;
        }
        if (p.kind == 7 && jux_range(p.value, &lo, &hi, &rv, &l0, &l1)) {
            /* a number in a range: its value box, and the slider under it */
            if (f > 0.01f)
                rect(rx, y + 56, rw, 112 - 66, alpha(J_RAISED, f * k), 10);
            text(f_reg, 22, rx + 22, y + 15, lab, p.label.c_str(), 0, rw - 280);
            jux_value_box(rx + rw - 22, y + 8, 180, p.shown, f, k);
            jux_center_slider(rx + 22, y + 66, rw - 44, lo, hi, rv, l0, l1, f, k);
            y += 112;
            continue;
        }
        if (p.kind == 7) {
            text(f_reg, 22, rx + 22, y + 15, lab, p.label.c_str(), 0, rw - 280);
            /* a switch only for what is on or off; a list of choices (upscaling,
             * sleep timer, equalizer) keeps its box even when it says Off */
            bool toggle = p.value == SET_SUB_BOX || p.value == SET_NIGHT || p.value == SET_REPEAT ||
                          p.value == SET_SHUFFLE;
            if (toggle)
                jux_switch(rx + rw - 110, y + 9, p.shown == "On", k, hash_id(p.label, 777));
            else if (p.shown.empty())
                icon_chevron(rx + rw - 34, y + 28, 16, alpha(mix(J_FAINT, C_ORANGE, f), k), false); /* an action */
            else
                jux_value_box(rx + rw - 22, y + 8, 220, p.shown, f, k);
            if (p.value == SET_SUB_COLOUR) {
                float sx = rx + rw - 22 - 40;
                rect(sx, y + 16, 24, 24, alpha(jux_sub_colour(player_sub_style().colour), k), 4);
                stroke(sx, y + 16, 24, 24, alpha(IM_COL32(0, 0, 0, 255), k), 4, 1.2f);
            }
        } else if (p.kind == 5) {
            /* the volume: the design's slider, with its value */
            char vv[16];
            snprintf(vv, sizeof(vv), "%d%%", player_volume());
            text(f_reg, 22, rx + 22, y + 15, lab, "Volume");
            text(f_time, 21, rx + rw - 22, y + 16, alpha(J_DIM, k), vv, 1);
            float v = player_volume() / 200.0f, bx = rx + 170, bw = rw - 170 - 100;
            rect(bx, y + 19, bw, 18, alpha(J_BAR, k), 6);
            rect(bx + 2, y + 21, (bw - 4) * v, 14, alpha(C_ORANGE2, k), 5);
            rect(bx + bw * 0.5f - 1, y + 15, 2, 26, alpha(J_EDGE, k));
            float kx = bx + 2 + (bw - 4) * v;
            rect(kx - 4 - 2 * f, y + 13 - 2 * f, 8 + 4 * f, 30 + 4 * f, alpha(C_ORANGE, k), 4);
        } else {
            text(f_reg, 22, rx + 22, y + 15, p.active ? alpha(J_TEXT, k) : lab, p.label.c_str(), 0, rw - 90);
            if (p.active) {
                circle(rx + rw - 34, y + 28, 13, alpha(C_ORANGE, k));
                icon_check(rx + rw - 34, y + 28, 16, alpha(J_INK, k));
            }
        }
        y += 66;
    }
    dl->PopClipRect();

    /* the bottom: L1 / R1 change the tab, and what the buttons do */
    rect(x + 28, y0 + h - 70, w - 56, 1, alpha(J_LINE, a));
    jux_key(x + 52, y0 + h - 36, "L1");
    jux_key(x + 98, y0 + h - 36, "R1");
    text(f_reg, 20, x + 128, y0 + h - 48, alpha(J_DIM, a), "Tabs");
    bool setting_focused = !focusable.empty() && e[focusable[modal_focus]].kind == 7;
    if (setting_focused) {
        int id = e[focusable[modal_focus]].value;
        const char *act = id == SET_LOOP ? (loop_a < 0 ? "Mark A" : loop_b < 0 ? "Mark B" : "Clear")
                        : id == SET_SUB_BOX || id == SET_NIGHT || id == SET_SOUND_OUT || id == SET_SHUFFLE ? "Switch"
                        : id == SET_REPEAT ? "Switch" : id == SET_BOOKMARK ? "Add"
                        : id == SET_BM_CLEAR ? "Clear" : id == SET_DISC_MENU || id == SET_POPUP || id == SET_SUB_PICK ? "Open" : "Reset";
        hints(x + w - 30, y0 + h - 36, { { PAD_CROSS, act }, { PAD_CIRCLE, "Close" } }, a);
    } else if (!focusable.empty() && e[focusable[modal_focus]].kind == 10) {
        hints(x + w - 30, y0 + h - 36, { { PAD_CROSS, "Reset" }, { PAD_CIRCLE, "Close" } }, a); /* a band: to 0 dB */
    } else {
        hints(x + w - 30, y0 + h - 36, { { PAD_CROSS, "Select" }, { PAD_CIRCLE, "Close" } }, a);
    }
}
