/*
 * VLC-PS5's interface.
 *
 * Look: VLC 4's dark media-library style (cards with thumbnails, a tab bar,
 * the orange accent) made for a TV and a controller: big focus states that
 * lift and glow, everything animated, PlayStation button hints, and a player
 * overlay with a scrubbable seek bar and a Tracks panel.
 *
 * Layout is in 1920x1080 units, scaled to the screen (2x on a 4K TV). Every
 * moving value eases toward its target with approach(), so motion stays
 * smooth at any refresh rate.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <fribidi/fribidi.h>
#include <unordered_map>

#include "gfx.h"
#include "imgui.h"
#include "library.h"
#include "player.h"
#include "prefs.h"
#include <vlc/vlc.h>

#include "network.h"
#include "lang.h"
#include "osub.h"
#include "web.h"

namespace {

/* ---- look ------------------------------------------------------------------- */

const float PI_F = 3.14159265f;
/* Two looks (Settings > Interface > Look, applied at start): Modern, the black
 * VLC 4 style, and Classic, graphite panels and bevels like a desktop media
 * player of the Windows 7 years, VLC orange as the accent only. */
bool classic_look;
ImU32 C_BG = IM_COL32(11, 11, 15, 255);
ImU32 C_CARD = IM_COL32(28, 28, 35, 255);
ImU32 C_CARD_HI = IM_COL32(40, 40, 50, 255);
ImU32 C_TEXT = IM_COL32(245, 245, 248, 255);
ImU32 C_DIM = IM_COL32(150, 150, 162, 255);
ImU32 C_FAINT = IM_COL32(95, 95, 108, 255);
const ImU32 C_LINE = IM_COL32(62, 66, 74, 255);  /* Classic: panel borders */
const ImU32 C_SHADE = IM_COL32(8, 9, 11, 255);   /* Classic: the dark edge under a bevel */

void apply_look()
{
    /* Modern is the default; an install from before the setup was Classic */
    classic_look = pref_int("look", prefs_existed() ? 1 : 0) == 1;
    if (!classic_look)
        return;
    C_BG = IM_COL32(23, 25, 29, 255);
    C_CARD = IM_COL32(34, 37, 42, 255);
    C_CARD_HI = IM_COL32(46, 50, 57, 255);
    C_TEXT = IM_COL32(236, 238, 241, 255);
    C_DIM = IM_COL32(165, 170, 178, 255);
    C_FAINT = IM_COL32(110, 115, 124, 255);
}
/* The accent: VLC orange, or the theme's (Settings > Theme). */
ImU32 C_ORANGE = IM_COL32(255, 136, 0, 255);
ImU32 C_ORANGE2 = IM_COL32(255, 94, 20, 255);
const ImU32 CONE_ORANGE = IM_COL32(255, 136, 0, 255), CONE_ORANGE2 = IM_COL32(255, 94, 20, 255);

struct Theme {
    std::string name;
    ImU32 accent, accent2;
};
std::vector<Theme> themes = {
    { "VLC orange", IM_COL32(255, 136, 0, 255), IM_COL32(255, 94, 20, 255) },
    { "Ocean", IM_COL32(46, 155, 255, 255), IM_COL32(31, 111, 255, 255) },
    { "Violet", IM_COL32(168, 107, 255, 255), IM_COL32(134, 76, 240, 255) },
    { "Emerald", IM_COL32(46, 209, 138, 255), IM_COL32(26, 168, 108, 255) },
    { "Crimson", IM_COL32(255, 77, 94, 255), IM_COL32(224, 44, 72, 255) },
    { "Rose", IM_COL32(255, 111, 181, 255), IM_COL32(232, 79, 152, 255) },
    { "Gold", IM_COL32(255, 196, 46, 255), IM_COL32(240, 160, 16, 255) },
};
const size_t BUILTIN_THEMES = 7;

ImU32 parse_colour(const std::string &v, ImU32 fallback)
{
    unsigned r, g, b;
    const char *c = v.c_str();
    if (*c == '#')
        c++;
    if (sscanf(c, "%2x%2x%2x", &r, &g, &b) != 3)
        return fallback;
    return IM_COL32(r, g, b, 255);
}

void apply_theme()
{
    if (classic_look) {
        /* Classic: VLC orange only, no themes. */
        C_ORANGE = themes[0].accent;
        C_ORANGE2 = themes[0].accent2;
        return;
    }
    int i = pref_int("theme", 0);
    if (i < 0 || i >= (int)themes.size())
        i = 0;
    C_ORANGE = themes[i].accent;
    C_ORANGE2 = themes[i].accent2;
}

ImU32 alpha(ImU32 c, float a)
{
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    return (c & 0x00ffffff) | ((ImU32)(((c >> 24) & 0xff) * a) << 24);
}

ImU32 mix(ImU32 a, ImU32 b, float t)
{
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

float S = 1;     /* pixels per layout unit */
float dt = 1 / 60.0f;
double now;
ImFont *f_reg, *f_semi, *f_bold, *f_mono;
ImDrawList *dl;

ImVec2 P(float x, float y)
{
    return ImVec2(x * S, y * S);
}

float approach(float cur, float target, float speed)
{
    return cur + (target - cur) * (1 - expf(-speed * dt));
}

float ease(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* VLC's own messages are English: the usual one in our words (translated). */
std::string vlc_message(const std::string &m)
{
    if (m.compare(0, 29, "VLC is unable to open the MRL") == 0)
        return "VLC couldn't open this address: it may be offline, or need a login";
    /* the SMB login (VLC's smb2 module): who asks, then what to type */
    if (m == "SMB authentication required")
        return "Sign in to the share";
    if (m.compare(0, 14, "The computer (") == 0 && m.find(')') != std::string::npos) {
        std::string host = m.substr(14, m.find(')') - 14);
        bool more = m.find("Please provide") != std::string::npos;
        return trf("%s needs a user name and password.", host.c_str()) +
               (more ? std::string(" ") + tr("A Windows PC takes its own account; a NAS, the user you made on it.") : "");
    }
    if (m.compare(0, 14, "Please provide") == 0)
        return "A Windows PC takes its own account; a NAS, the user you made on it.";
    /* libbluray without a JVM (none runs on the console): not "install Java" */
    if (m.compare(0, 30, "This Blu-ray disc requires Java") == 0)
        return "This Blu-ray's menus are made in Java, which the PS5 can't run: it plays without them";
    if (m.compare(0, 6, "Track ") == 0 && m.size() > 6 && isdigit((unsigned char)m[6]))
        return trf("Track %d", atoi(m.c_str() + 6));
    return m;
}

/* Text as it is shown: in the chosen language, and names in Arabic or Hebrew
 * right to left with Arabic letters joined (lang.cc). */
const char *visual(const char *s)
{
    return lang_shown(s);
}

ImVec2 text_size(ImFont *f, float size, const char *s)
{
    s = visual(s);
    ImVec2 v = f->CalcTextSizeA(size * S, FLT_MAX, 0, s);
    return ImVec2(v.x / S, v.y / S);
}

/* align: 0 left, 0.5 centre, 1 right. max_w > 0 cuts with an ellipsis. */
void text(ImFont *f, float size, float x, float y, ImU32 col, const char *s, float align = 0,
          float max_w = 0)
{
    s = visual(s);
    std::string cut;
    if (max_w > 0 && text_size(f, size, s).x > max_w) {
        cut = s;
        while (!cut.empty() && text_size(f, size, (cut + "\xE2\x80\xA6").c_str()).x > max_w) {
            /* a whole UTF-8 character: its continuation bytes, then its first */
            while (!cut.empty() && ((unsigned char)cut.back() & 0xC0) == 0x80)
                cut.pop_back();
            if (!cut.empty())
                cut.pop_back();
        }
        cut += "\xE2\x80\xA6";
        s = cut.c_str();
    }
    float w = align ? text_size(f, size, s).x : 0;
    dl->AddText(f, size * S, P(x - w * align, y), col, s);
}

/* Classic: corners are the old 4 px, never pills. */
float corner(float r)
{
    return classic_look && r > 4 ? 4 : r;
}

void rect(float x, float y, float w, float h, ImU32 col, float r = 0)
{
    dl->AddRectFilled(P(x, y), P(x + w, y + h), col, corner(r) * S);
}

void stroke(float x, float y, float w, float h, ImU32 col, float r, float t)
{
    dl->AddRect(P(x, y), P(x + w, y + h), col, corner(r) * S, 0, t * S);
}

/* A soft shadow or glow: rounded rects growing outward, fading. */
void glow(float x, float y, float w, float h, float r, ImU32 col, float spread, int steps = 8)
{
    if (classic_look)
        return; /* Classic: flat, no glows (and lighter to draw) */
    for (int i = steps; i >= 1; i--) {
        float g = spread * i / steps;
        float a = (1.0f - (float)i / (steps + 1)) / steps * 1.6f;
        rect(x - g, y - g, w + 2 * g, h + 2 * g, alpha(col, a), r + g);
    }
}

void circle(float x, float y, float r, ImU32 col)
{
    dl->AddCircleFilled(P(x, y), r * S, col, 48);
}

/* A smooth radial light: rings of vertex colours along a smooth curve, so no
 * banding (stacked translucent circles show rings). */
void radial(float cx, float cy, float r, ImU32 inner, int rings = 12, int seg = 72)
{
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    ImVec2 c = P(cx, cy);
    auto col_at = [&](int ring_i) {
        float k = 1 - (float)ring_i / rings;
        return alpha(inner, k * k * (3 - 2 * k));
    };
    dl->PrimReserve(seg * 3 + (rings - 1) * seg * 6, 1 + rings * seg);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, col_at(0));
    for (int ri = 1; ri <= rings; ri++) {
        float rr = r * ri / rings * S;
        ImU32 col = col_at(ri);
        for (int i = 0; i < seg; i++) {
            float a = i * 2 * 3.14159265f / seg;
            dl->PrimWriteVtx(ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr), uv, col);
        }
    }
    for (int i = 0; i < seg; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % seg));
    }
    for (int ri = 1; ri < rings; ri++) {
        ImDrawIdx in = (ImDrawIdx)(base + 1 + (ri - 1) * seg), out = (ImDrawIdx)(in + seg);
        for (int i = 0; i < seg; i++) {
            int j = (i + 1) % seg;
            dl->PrimWriteIdx((ImDrawIdx)(in + i));
            dl->PrimWriteIdx((ImDrawIdx)(out + i));
            dl->PrimWriteIdx((ImDrawIdx)(out + j));
            dl->PrimWriteIdx((ImDrawIdx)(in + i));
            dl->PrimWriteIdx((ImDrawIdx)(out + j));
            dl->PrimWriteIdx((ImDrawIdx)(in + j));
        }
    }
}

void ring(float x, float y, float r, ImU32 col, float t)
{
    dl->AddCircle(P(x, y), r * S, col, 48, t * S);
}

void line(float x0, float y0, float x1, float y1, ImU32 col, float t)
{
    dl->AddLine(P(x0, y0), P(x1, y1), col, t * S);
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, ImU32 col)
{
    dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col);
}

/* ---- the Classic look's pieces ---------------------------------------------------- */

/* A vertical gradient (square corners, like the panels it builds). */
void vgrad(float x, float y, float w, float h, ImU32 top, ImU32 bottom)
{
    dl->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), top, top, bottom, bottom);
}

/* A raised panel: gradient, a 1 px light line on top, a dark one below. */
void bevel(float x, float y, float w, float h, ImU32 top, ImU32 bottom)
{
    vgrad(x, y, w, h, top, bottom);
    rect(x, y, w, 1, IM_COL32(255, 255, 255, 22));
    rect(x, y + h - 1, w, 1, C_SHADE);
}

/* A list's selection: an orange-tinted glass box with a thin border, as a
 * desktop file list draws it. f: 0..1 as it fades in. */
void sel_box(float x, float y, float w, float h, float f)
{
    if (f < 0.01f)
        return;
    rect(x, y, w, h, alpha(IM_COL32(255, 140, 20, 255), 0.17f * f), 4);
    rect(x + 1, y + 1, w - 2, h * 0.48f, alpha(IM_COL32(255, 200, 140, 255), 0.08f * f), 3);
    stroke(x, y, w, h, alpha(IM_COL32(255, 150, 40, 255), 0.85f * f), 4, 1.5f);
    stroke(x + 1.5f, y + 1.5f, w - 3, h - 3, alpha(IM_COL32(255, 255, 255, 255), 0.07f * f), 3, 1);
}

/* A sunken field (the address bar, the search box). */
void inset(float x, float y, float w, float h)
{
    rect(x, y, w, h, IM_COL32(14, 15, 18, 255), 3);
    stroke(x, y, w, h, C_LINE, 3, 1);
    rect(x + 2, y + 1, w - 4, 1, IM_COL32(0, 0, 0, 160));
}

/* A dialog's button: metal, and the selection box when focused (hot: the
 * accent of a dangerous one, Delete, in red). */
void classic_button(float x, float y, float w, float h, float f, float a, ImU32 hot)
{
    vgrad(x, y, w, h, alpha(IM_COL32(72, 77, 87, 255), a), alpha(IM_COL32(44, 48, 55, 255), a));
    stroke(x, y, w, h, alpha(IM_COL32(10, 11, 13, 255), a), 4, 1);
    rect(x + 2, y + 1, w - 4, 1, alpha(IM_COL32(255, 255, 255, 34), a));
    if (hot == C_ORANGE) {
        sel_box(x, y, w, h, f * a);
    } else if (f > 0.01f) {
        rect(x, y, w, h, alpha(hot, 0.22f * f * a), 4);
        stroke(x, y, w, h, alpha(hot, 0.9f * f * a), 4, 1.5f);
    }
}

/* A group heading: blue text and a thin line to the right edge. */
void classic_heading(float x, float y, const char *title, size_t count)
{
    char head[96];
    snprintf(head, sizeof(head), "%s (%zu)", tr(title), count);
    text(f_semi, 22, x, y, IM_COL32(150, 186, 226, 255), head);
    float lx = x + text_size(f_semi, 22, head).x + 16;
    rect(lx, y + 14, 1824 - lx, 1, IM_COL32(90, 110, 135, 255));
}

/* Column headers of a Details list: names at x (align 1 = right). */
struct Column {
    const char *name;
    float x, align;
};

void classic_columns(float y, const std::vector<Column> &cols)
{
    const float h = 36;
    bevel(96, y, 1728, h, IM_COL32(48, 52, 59, 255), IM_COL32(36, 39, 45, 255));
    stroke(96, y, 1728, h, C_LINE, 0, 1);
    for (size_t i = 0; i < cols.size(); i++) {
        text(f_reg, 19, cols[i].x, y + 8, C_DIM, cols[i].name, cols[i].align);
        if (i > 0) {
            float sep = cols[i].align > 0 ? cols[i].x - 130 : cols[i].x - 20;
            rect(sep, y + 7, 1, h - 14, C_LINE);
        }
    }
}

/* ---- Classic icons: little coloured pictures, as the old file browsers had ---- */

/* A manila folder, open at the front, a sheet peeking out. */
void ci_folder(float cx, float cy, float s)
{
    const ImU32 back = IM_COL32(205, 150, 46, 255), edge = IM_COL32(150, 104, 22, 255);
    rect(cx - s * 0.5f, cy - s * 0.42f, s * 0.42f, s * 0.14f, back, s * 0.05f);
    rect(cx - s * 0.5f, cy - s * 0.33f, s, s * 0.71f, back, s * 0.05f);
    rect(cx - s * 0.4f, cy - s * 0.27f, s * 0.8f, s * 0.42f, IM_COL32(246, 245, 238, 255));
    rect(cx - s * 0.4f, cy - s * 0.17f, s * 0.6f, s * 0.03f, IM_COL32(200, 200, 196, 255));
    vgrad(cx - s * 0.5f, cy - s * 0.12f, s, s * 0.5f, IM_COL32(255, 222, 128, 255), IM_COL32(240, 176, 52, 255));
    rect(cx - s * 0.5f, cy - s * 0.12f, s, 1.5f, IM_COL32(255, 244, 200, 255));
    stroke(cx - s * 0.5f, cy - s * 0.33f, s, s * 0.71f, edge, s * 0.05f, 1);
}

/* A page with its corner folded; kind 0 video, 1 audio, 2 playlist, 3 other. */
void ci_file(float cx, float cy, float s, int kind)
{
    float w = s * 0.72f, h = s * 0.94f, x = cx - w / 2, y = cy - h / 2, fold = s * 0.22f;
    vgrad(x, y, w - fold, h, IM_COL32(255, 255, 255, 255), IM_COL32(222, 224, 228, 255));
    vgrad(x + w - fold, y + fold, fold, h - fold, IM_COL32(250, 250, 252, 255), IM_COL32(222, 224, 228, 255));
    tri(x + w - fold, y, x + w - fold, y + fold, x + w, y + fold, IM_COL32(190, 194, 200, 255));
    dl->PathLineTo(P(x, y));
    dl->PathLineTo(P(x + w - fold, y));
    dl->PathLineTo(P(x + w, y + fold));
    dl->PathLineTo(P(x + w, y + h));
    dl->PathLineTo(P(x, y + h));
    dl->PathStroke(IM_COL32(120, 126, 136, 255), ImDrawFlags_Closed, 1 * S);
    float bx = cx, by = cy + s * 0.12f;
    switch (kind) {
    case 0: /* a strip of film */
        rect(bx - s * 0.26f, by - s * 0.16f, s * 0.52f, s * 0.32f, IM_COL32(40, 42, 48, 255), 1);
        for (int i = 0; i < 4; i++) {
            rect(bx - s * 0.22f + i * s * 0.13f, by - s * 0.13f, s * 0.06f, s * 0.05f, IM_COL32(220, 220, 220, 255));
            rect(bx - s * 0.22f + i * s * 0.13f, by + s * 0.08f, s * 0.06f, s * 0.05f, IM_COL32(220, 220, 220, 255));
        }
        tri(bx - s * 0.05f, by - s * 0.06f, bx - s * 0.05f, by + s * 0.06f, bx + s * 0.07f, by, C_ORANGE);
        break;
    case 1: /* a note */
        circle(bx - s * 0.1f, by + s * 0.1f, s * 0.08f, C_ORANGE);
        rect(bx - s * 0.04f, by - s * 0.2f, s * 0.04f, s * 0.3f, C_ORANGE);
        rect(bx - s * 0.04f, by - s * 0.2f, s * 0.16f, s * 0.05f, C_ORANGE);
        break;
    case 2: /* lines and a note */
        for (int i = 0; i < 3; i++)
            rect(bx - s * 0.2f, by - s * 0.18f + i * s * 0.1f, s * (i == 2 ? 0.18f : 0.3f), s * 0.035f, IM_COL32(110, 116, 126, 255));
        circle(bx + s * 0.13f, by + s * 0.14f, s * 0.06f, C_ORANGE);
        rect(bx + s * 0.17f, by - s * 0.04f, s * 0.03f, s * 0.18f, C_ORANGE);
        break;
    case 4: /* a photo: sky, a sun, a hill */
        vgrad(bx - s * 0.24f, by - s * 0.2f, s * 0.48f, s * 0.36f, IM_COL32(110, 170, 230, 255), IM_COL32(170, 210, 240, 255));
        circle(bx + s * 0.12f, by - s * 0.09f, s * 0.05f, IM_COL32(255, 210, 80, 255));
        tri(bx - s * 0.24f, by + s * 0.16f, bx - s * 0.06f, by - s * 0.04f, bx + s * 0.1f, by + s * 0.16f, IM_COL32(70, 150, 70, 255));
        tri(bx, by + s * 0.16f, bx + s * 0.12f, by + s * 0.04f, bx + s * 0.24f, by + s * 0.16f, IM_COL32(50, 125, 55, 255));
        stroke(bx - s * 0.24f, by - s * 0.2f, s * 0.48f, s * 0.36f, IM_COL32(90, 96, 106, 255), 0, 1);
        break;
    case 5: /* any other file: a blank page */
        break;
    default:
        for (int i = 0; i < 4; i++)
            rect(bx - s * 0.2f, by - s * 0.22f + i * s * 0.1f, s * 0.4f, s * 0.035f, IM_COL32(160, 166, 176, 255));
    }
}

/* A drive: brushed metal with a green light; usb: a plug on top. */
void ci_drive(float cx, float cy, float s, bool usb)
{
    float w = s, h = s * 0.46f, x = cx - w / 2, y = cy - h / 2 + s * 0.06f;
    vgrad(x, y, w, h, IM_COL32(222, 226, 231, 255), IM_COL32(140, 146, 155, 255));
    stroke(x, y, w, h, IM_COL32(90, 95, 104, 255), s * 0.05f, 1);
    rect(x + w * 0.08f, y + h * 0.62f, w * 0.58f, h * 0.12f, IM_COL32(70, 74, 82, 255));
    circle(x + w * 0.84f, y + h * 0.68f, s * 0.045f, IM_COL32(80, 230, 90, 255));
    if (usb) {
        rect(cx - s * 0.12f, y - s * 0.16f, s * 0.24f, s * 0.16f, IM_COL32(170, 176, 186, 255), 1);
        rect(cx - s * 0.07f, y - s * 0.12f, s * 0.04f, s * 0.05f, IM_COL32(60, 64, 70, 255));
        rect(cx + s * 0.03f, y - s * 0.12f, s * 0.04f, s * 0.05f, IM_COL32(60, 64, 70, 255));
    }
}

/* A computer: a screen with a blue desktop on a little stand. */
void ci_computer(float cx, float cy, float s)
{
    float w = s * 0.92f, h = s * 0.66f, x = cx - w / 2, y = cy - s * 0.44f;
    rect(x, y, w, h, IM_COL32(48, 52, 60, 255), s * 0.05f);
    vgrad(x + s * 0.06f, y + s * 0.06f, w - s * 0.12f, h - s * 0.12f, IM_COL32(80, 150, 220, 255), IM_COL32(28, 76, 140, 255));
    rect(cx - s * 0.07f, y + h, s * 0.14f, s * 0.12f, IM_COL32(110, 116, 126, 255));
    rect(cx - s * 0.26f, y + h + s * 0.11f, s * 0.52f, s * 0.07f, IM_COL32(140, 146, 156, 255), 1);
}

/* A server tower with drive bays. */
void ci_server(float cx, float cy, float s)
{
    float w = s * 0.56f, h = s, x = cx - w / 2, y = cy - h / 2;
    vgrad(x, y, w, h, IM_COL32(96, 102, 112, 255), IM_COL32(44, 47, 54, 255));
    stroke(x, y, w, h, IM_COL32(24, 26, 30, 255), s * 0.04f, 1);
    for (int i = 0; i < 3; i++)
        rect(x + w * 0.14f, y + h * (0.14f + i * 0.14f), w * 0.72f, h * 0.07f, IM_COL32(28, 30, 34, 255));
    circle(x + w * 0.5f, y + h * 0.8f, s * 0.05f, IM_COL32(80, 230, 90, 255));
}

/* An old television: a curved screen in a wooden case, two rabbit ears. */
void ci_tv(float cx, float cy, float s)
{
    float w = s, h = s * 0.7f, x = cx - w / 2, y = cy - h / 2 + s * 0.12f;
    line(cx, y, cx - s * 0.24f, y - s * 0.3f, IM_COL32(150, 156, 166, 255), 1.5f);
    line(cx, y, cx + s * 0.22f, y - s * 0.32f, IM_COL32(150, 156, 166, 255), 1.5f);
    vgrad(x, y, w, h, IM_COL32(150, 96, 52, 255), IM_COL32(96, 58, 28, 255));
    stroke(x, y, w, h, IM_COL32(60, 36, 16, 255), s * 0.06f, 1);
    vgrad(x + s * 0.08f, y + s * 0.08f, w * 0.66f, h - s * 0.16f, IM_COL32(150, 176, 190, 255), IM_COL32(52, 70, 82, 255));
    circle(x + w * 0.86f, y + h * 0.3f, s * 0.05f, IM_COL32(230, 220, 200, 255));
    circle(x + w * 0.86f, y + h * 0.6f, s * 0.05f, IM_COL32(230, 220, 200, 255));
}

/* A radio set: a grille, a dial and an aerial. */
void ci_radio(float cx, float cy, float s)
{
    float w = s, h = s * 0.62f, x = cx - w / 2, y = cy - h / 2 + s * 0.12f;
    line(x + w * 0.8f, y, x + w * 0.96f, y - s * 0.36f, IM_COL32(170, 176, 186, 255), 1.5f);
    vgrad(x, y, w, h, IM_COL32(176, 120, 66, 255), IM_COL32(110, 68, 34, 255));
    stroke(x, y, w, h, IM_COL32(64, 38, 16, 255), s * 0.08f, 1);
    rect(x + w * 0.08f, y + h * 0.18f, w * 0.48f, h * 0.64f, IM_COL32(52, 34, 20, 255), 2);
    for (int i = 0; i < 4; i++)
        rect(x + w * 0.1f, y + h * (0.26f + i * 0.14f), w * 0.44f, h * 0.05f, IM_COL32(150, 112, 70, 255));
    circle(x + w * 0.78f, y + h * 0.5f, s * 0.12f, IM_COL32(236, 214, 170, 255));
    line(x + w * 0.78f, y + h * 0.5f, x + w * 0.84f, y + h * 0.4f, IM_COL32(120, 60, 20, 255), 1.5f);
}

/* A disc: silver, a rainbow sheen, the hole in the middle. */
void ci_disc(float cx, float cy, float s)
{
    float r = s * 0.48f;
    circle(cx, cy, r, IM_COL32(196, 200, 208, 255));
    radial(cx - r * 0.3f, cy - r * 0.3f, r * 0.9f, IM_COL32(255, 255, 255, 140), 6, 32);
    dl->PathArcTo(P(cx, cy), r * 0.72f * S, -2.4f, -0.9f, 12);
    dl->PathStroke(IM_COL32(150, 120, 230, 150), 0, 2 * S);
    dl->PathArcTo(P(cx, cy), r * 0.6f * S, 0.5f, 1.8f, 12);
    dl->PathStroke(IM_COL32(90, 200, 190, 150), 0, 2 * S);
    ring(cx, cy, r, IM_COL32(110, 116, 126, 255), 1);
    circle(cx, cy, r * 0.22f, IM_COL32(150, 156, 166, 255));
    circle(cx, cy, r * 0.1f, IM_COL32(23, 25, 29, 255));
}

/* A compressed folder: the manila folder with a zip down its front. */
void ci_zip(float cx, float cy, float s)
{
    ci_folder(cx, cy, s);
    for (int i = 0; i < 5; i++)
        rect(cx - s * 0.06f + (i % 2) * s * 0.06f, cy - s * 0.1f + i * s * 0.09f, s * 0.06f, s * 0.06f, IM_COL32(90, 95, 104, 255));
    rect(cx - s * 0.07f, cy + s * 0.32f, s * 0.14f, s * 0.06f, IM_COL32(150, 156, 166, 255), 1);
}

/* "Add": a computer with an orange plus. */
void ci_add(float cx, float cy, float s)
{
    ci_computer(cx - s * 0.08f, cy, s * 0.86f);
    circle(cx + s * 0.3f, cy + s * 0.26f, s * 0.2f, C_ORANGE);
    rect(cx + s * 0.21f, cy + s * 0.24f, s * 0.18f, s * 0.045f, IM_COL32(255, 255, 255, 255));
    rect(cx + s * 0.278f, cy + s * 0.17f, s * 0.045f, s * 0.18f, IM_COL32(255, 255, 255, 255));
}

/* ---- icons, drawn so they stay sharp at any size ----------------------------------- */

void icon_play(float cx, float cy, float s, ImU32 col)
{
    tri(cx - s * 0.38f, cy - s * 0.5f, cx - s * 0.38f, cy + s * 0.5f, cx + s * 0.5f, cy, col);
}

void icon_pause(float cx, float cy, float s, ImU32 col)
{
    rect(cx - s * 0.42f, cy - s * 0.5f, s * 0.3f, s, col, s * 0.06f);
    rect(cx + s * 0.12f, cy - s * 0.5f, s * 0.3f, s, col, s * 0.06f);
}

void icon_skip(float cx, float cy, float s, ImU32 col, bool next)
{
    float d = next ? 1 : -1;
    tri(cx - d * s * 0.4f, cy - s * 0.42f, cx - d * s * 0.4f, cy + s * 0.42f, cx + d * s * 0.25f, cy, col);
    rect(next ? cx + s * 0.25f : cx - s * 0.37f, cy - s * 0.42f, s * 0.12f, s * 0.84f, col);
}

void icon_jump(float cx, float cy, float s, ImU32 col, bool forward, const char *label)
{
    float r = s * 0.48f;
    float a0 = forward ? -PI_F * 0.35f : PI_F * 1.35f;
    float a1 = forward ? PI_F * 1.25f : -PI_F * 0.25f;
    dl->PathArcTo(P(cx, cy), r * S, a0, a1, 32);
    dl->PathStroke(col, 0, s * 0.08f * S);
    float ax = cx + cosf(a0) * r, ay = cy + sinf(a0) * r;
    float d = forward ? 1 : -1;
    tri(ax - d * s * 0.16f, ay - s * 0.14f, ax + d * s * 0.12f, ay + s * 0.02f, ax - d * s * 0.16f,
        ay + s * 0.18f, col);
    text(f_bold, s * 0.36f, cx, cy - s * 0.22f, col, label, 0.5f);
}

void icon_speaker(float cx, float cy, float s, ImU32 col)
{
    rect(cx - s * 0.45f, cy - s * 0.16f, s * 0.22f, s * 0.32f, col, s * 0.04f);
    tri(cx - s * 0.25f, cy - s * 0.16f, cx + s * 0.05f, cy - s * 0.42f, cx + s * 0.05f, cy + s * 0.42f, col);
    tri(cx - s * 0.25f, cy + s * 0.16f, cx - s * 0.25f, cy - s * 0.16f, cx + s * 0.05f, cy + s * 0.42f, col);
    dl->PathArcTo(P(cx + s * 0.08f, cy), s * 0.24f * S, -0.9f, 0.9f, 12);
    dl->PathStroke(col, 0, s * 0.07f * S);
    dl->PathArcTo(P(cx + s * 0.08f, cy), s * 0.42f * S, -0.9f, 0.9f, 16);
    dl->PathStroke(col, 0, s * 0.07f * S);
}

void icon_cc(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.48f, cy - s * 0.34f, s * 0.96f, s * 0.68f, col, s * 0.12f, s * 0.07f);
    text(f_bold, s * 0.42f, cx, cy - s * 0.25f, col, "CC", 0.5f);
}

void icon_film(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.5f, cy - s * 0.36f, s, s * 0.72f, col, s * 0.08f, s * 0.07f);
    for (int i = 0; i < 4; i++) {
        float x = cx - s * 0.4f + i * s * 0.27f;
        rect(x, cy - s * 0.3f, s * 0.1f, s * 0.08f, col);
        rect(x, cy + s * 0.22f, s * 0.1f, s * 0.08f, col);
    }
    icon_play(cx + s * 0.04f, cy, s * 0.3f, col);
}

void icon_note(float cx, float cy, float s, ImU32 col)
{
    circle(cx - s * 0.22f, cy + s * 0.28f, s * 0.16f, col);
    circle(cx + s * 0.28f, cy + s * 0.18f, s * 0.16f, col);
    rect(cx - s * 0.1f, cy - s * 0.4f, s * 0.07f, s * 0.7f, col);
    rect(cx + s * 0.4f, cy - s * 0.5f, s * 0.07f, s * 0.7f, col);
    dl->AddQuadFilled(P(cx - s * 0.1f, cy - s * 0.4f), P(cx + s * 0.47f, cy - s * 0.5f),
                      P(cx + s * 0.47f, cy - s * 0.33f), P(cx - s * 0.1f, cy - s * 0.23f), col);
}

void icon_folder(float cx, float cy, float s, ImU32 col)
{
    rect(cx - s * 0.5f, cy - s * 0.38f, s * 0.42f, s * 0.2f, col, s * 0.06f);
    rect(cx - s * 0.5f, cy - s * 0.26f, s, s * 0.64f, col, s * 0.08f);
}

void icon_gear(float cx, float cy, float s, ImU32 col)
{
    for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4;
        dl->AddLine(P(cx + cosf(a) * s * 0.25f, cy + sinf(a) * s * 0.25f),
                    P(cx + cosf(a) * s * 0.48f, cy + sinf(a) * s * 0.48f), col, s * 0.16f * S);
    }
    circle(cx, cy, s * 0.34f, col);
    circle(cx, cy, s * 0.14f, C_BG);
}

void icon_check(float cx, float cy, float s, ImU32 col)
{
    dl->PathLineTo(P(cx - s * 0.4f, cy));
    dl->PathLineTo(P(cx - s * 0.1f, cy + s * 0.3f));
    dl->PathLineTo(P(cx + s * 0.45f, cy - s * 0.32f));
    dl->PathStroke(col, 0, s * 0.14f * S);
}

void icon_picture(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.5f, cy - s * 0.34f, s, s * 0.68f, col, s * 0.08f, s * 0.07f);
    tri(cx - s * 0.36f, cy + s * 0.22f, cx - s * 0.08f, cy - s * 0.12f, cx + s * 0.14f, cy + s * 0.22f, col);
    tri(cx + s * 0.02f, cy + s * 0.22f, cx + s * 0.2f, cy, cx + s * 0.38f, cy + s * 0.22f, col);
    circle(cx + s * 0.24f, cy - s * 0.14f, s * 0.07f, col);
}

void icon_speed(float cx, float cy, float s, ImU32 col)
{
    dl->PathArcTo(P(cx, cy + s * 0.12f), s * 0.46f * S, PI_F, 2 * PI_F, 24);
    dl->PathStroke(col, 0, s * 0.08f * S);
    line(cx, cy + s * 0.12f, cx + s * 0.25f, cy - s * 0.18f, col, s * 0.08f);
    circle(cx, cy + s * 0.12f, s * 0.08f, col);
}

/* Five points: a fan of triangles from the centre (the outline isn't convex). */
void icon_star(float cx, float cy, float s, ImU32 col)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        float a = -PI_F / 2 + i * PI_F / 5, r = (i % 2 ? 0.21f : 0.5f) * s;
        pts[i] = ImVec2(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    for (int i = 0; i < 10; i++) {
        const ImVec2 &a = pts[i], &b = pts[(i + 1) % 10];
        tri(cx, cy, a.x, a.y, b.x, b.y, col);
    }
}

/* Repeat: a loop of two arrows; "1" inside for one file. */
void icon_repeat(float cx, float cy, float s, ImU32 col, int mode)
{
    float t = s * 0.08f;
    stroke(cx - s * 0.42f, cy - s * 0.26f, s * 0.84f, s * 0.52f, col, s * 0.16f, t);
    tri(cx + s * 0.02f, cy - s * 0.4f, cx + s * 0.02f, cy - s * 0.12f, cx + s * 0.2f, cy - s * 0.26f, col);
    tri(cx - s * 0.02f, cy + s * 0.12f, cx - s * 0.02f, cy + s * 0.4f, cx - s * 0.2f, cy + s * 0.26f, col);
    if (mode == 2)
        text(f_bold, s * 0.34f, cx, cy - s * 0.2f, col, "1", 0.5f);
}

/* A playlist: lines and a note. */
void icon_list(float cx, float cy, float s, ImU32 col)
{
    for (int i = 0; i < 3; i++)
        rect(cx - s * 0.48f, cy - s * 0.36f + i * s * 0.26f, s * (i == 2 ? 0.42f : 0.62f), s * 0.09f, col, s * 0.04f);
    circle(cx + s * 0.28f, cy + s * 0.32f, s * 0.13f, col);
    rect(cx + s * 0.36f, cy - s * 0.12f, s * 0.07f, s * 0.44f, col);
}

/* The VLC cone (a: fading with what it sits on). */
void cone(float x, float y, float s, float a = 1)
{
    float cx = x + s * 0.5f;
    rect(x + s * 0.06f, y + s * 0.86f, s * 0.88f, s * 0.14f, alpha(CONE_ORANGE2, a), s * 0.05f);
    tri(cx, y, x + s * 0.12f, y + s * 0.88f, x + s * 0.88f, y + s * 0.88f, alpha(CONE_ORANGE, a));
    /* White bands. */
    auto band = [&](float t0, float t1) {
        float w0 = 0.38f * t0, w1 = 0.38f * t1;
        dl->AddQuadFilled(P(cx - s * w0, y + s * 0.88f * t0), P(cx + s * w0, y + s * 0.88f * t0),
                          P(cx + s * w1, y + s * 0.88f * t1), P(cx - s * w1, y + s * 0.88f * t1),
                          alpha(IM_COL32(255, 255, 255, 235), a));
    };
    band(0.30f, 0.42f);
    band(0.62f, 0.74f);
}

/* PlayStation button glyph in a dark disc. */
void glyph(float cx, float cy, float r, uint32_t button, float a = 1)
{
    circle(cx, cy, r, alpha(IM_COL32(255, 255, 255, 40), a));
    ImU32 c = alpha(C_TEXT, a);
    float s = r * 0.52f;
    switch (button) {
    case PAD_CROSS:
        line(cx - s, cy - s, cx + s, cy + s, c, r * 0.16f);
        line(cx - s, cy + s, cx + s, cy - s, c, r * 0.16f);
        break;
    case PAD_CIRCLE:
        ring(cx, cy, s * 1.05f, c, r * 0.15f);
        break;
    case PAD_SQUARE:
        stroke(cx - s * 0.9f, cy - s * 0.9f, s * 1.8f, s * 1.8f, c, 0, r * 0.15f);
        break;
    case PAD_TRIANGLE:
        dl->AddTriangle(P(cx, cy - s * 1.05f), P(cx - s * 1.05f, cy + s * 0.75f),
                        P(cx + s * 1.05f, cy + s * 0.75f), c, r * 0.15f * S);
        break;
    case PAD_OPTIONS:
        for (int i = -1; i <= 1; i++)
            line(cx - s * 0.9f, cy + i * s * 0.6f, cx + s * 0.9f, cy + i * s * 0.6f, c, r * 0.13f);
        break;
    case PAD_LEFT:
        tri(cx - s * 0.9f, cy, cx + s * 0.6f, cy - s * 0.9f, cx + s * 0.6f, cy + s * 0.9f, c);
        break;
    case PAD_RIGHT:
        tri(cx + s * 0.9f, cy, cx - s * 0.6f, cy - s * 0.9f, cx - s * 0.6f, cy + s * 0.9f, c);
        break;
    case PAD_UP: /* ↑↓: scrolling */
        tri(cx, cy - s * 1.0f, cx - s * 0.7f, cy - s * 0.15f, cx + s * 0.7f, cy - s * 0.15f, c);
        tri(cx, cy + s * 1.0f, cx - s * 0.7f, cy + s * 0.15f, cx + s * 0.7f, cy + s * 0.15f, c);
        break;
    }
}

/* "[glyph] Label" pairs, right-aligned at x. Returns the left edge. */
float hints(float right, float y, const std::vector<std::pair<uint32_t, const char *>> &list, float a = 1)
{
    float x = right;
    for (int i = (int)list.size() - 1; i >= 0; i--) {
        uint32_t b = list[i].first;
        const char *label = list[i].second;
        float tw = text_size(f_semi, 22, label).x;
        x -= tw;
        text(f_semi, 22, x, y - 13, alpha(C_DIM, a), label);
        x -= 12;
        if (b == PAD_L1 || b == PAD_R1 || b == PAD_L2 || b == PAD_R2 || b == PAD_L3 || b == PAD_R3) {
            const char *n = b == PAD_L1 ? "L1" : b == PAD_R1 ? "R1" : b == PAD_L2 ? "L2"
                          : b == PAD_R2 ? "R2" : b == PAD_L3 ? "L3" : "R3";
            float w = 46;
            rect(x - w, y - 15, w, 30, alpha(IM_COL32(255, 255, 255, 40), a), 8);
            text(f_bold, 18, x - w / 2, y - 11, alpha(C_TEXT, a), n, 0.5f);
            x -= w;
        } else {
            glyph(x - 16, y, 16, b, a);
            x -= 32;
        }
        x -= 34;
    }
    return x;
}

/* A dark placeholder tinted by the name, for files without a thumbnail. */
ImU32 tint_for(const std::string &name, float l)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : name)
        h = (h ^ c) * 16777619u;
    float hue = (h % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, l, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
}

/* ---- input -------------------------------------------------------------------- */

uint32_t held, pressed; /* pressed: new presses plus held-direction repeats */
double repeat_at[32];
int repeat_count[32];

void read_input(const PadState &pad)
{
    uint32_t b = pad.buttons;
    /* The left stick works as the d-pad. */
    if (pad.ly < -0.6f) b |= PAD_UP;
    if (pad.ly > 0.6f) b |= PAD_DOWN;
    if (pad.lx < -0.6f) b |= PAD_LEFT;
    if (pad.lx > 0.6f) b |= PAD_RIGHT;
    pressed = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t bit = 1u << i;
        bool repeats = bit & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L2 | PAD_R2);
        if ((b & bit) && !(held & bit)) {
            pressed |= bit;
            repeat_at[i] = now + 0.38;
            repeat_count[i] = 0;
        } else if ((b & bit) && repeats && now >= repeat_at[i]) {
            pressed |= bit;
            repeat_count[i]++;
            /* Faster the longer it's held. */
            repeat_at[i] = now + (repeat_count[i] > 8 ? 0.045 : 0.09);
        }
    }
    held = b;
}

bool hit(uint32_t b)
{
    return pressed & b;
}

/* ---- state -------------------------------------------------------------------- */

enum Tab { HOME, VIDEOS, MUSIC, PLAYLISTS, BROWSE, TABS };
const char *const tab_names[TABS] = { "Home", "Videos", "Music", "Playlists", "Browse" };

enum Screen { LIBRARY, PLAYER, VIEWER };
Screen screen = LIBRARY;
Tab tab = HOME;
float screen_fade = 1;

/* Home: rows of cards. */
struct Row {
    const char *title;
    std::vector<int> items; /* indices into library_items() */
};
std::vector<Row> home_rows;
int home_row, home_col[8];
float home_scroll, home_col_scroll[8];

int video_focus, music_focus, browse_focus;
float video_scroll, music_scroll, browse_scroll;
std::string browse_dir; /* "" = the roots */

/* Focus animations, keyed by a stable id. */
struct Anim {
    uint64_t id;
    float v;
    double seen;
};
std::vector<Anim> anims;

float focus_anim(uint64_t id, bool focused, float speed = 14)
{
    for (Anim &a : anims)
        if (a.id == id) {
            /* Not drawn a moment ago (a menu or folder opened again): it starts
             * from nothing, never from the highlight it had last time (that
             * showed the old choice for a blink before the new one). */
            if (now - a.seen > 0.1)
                a.v = 0;
            a.v = approach(a.v, focused ? 1.0f : 0.0f, speed);
            a.seen = now;
            return a.v;
        }
    anims.push_back({ id, focused ? 0.0f : 0.0f, now });
    return 0;
}

uint64_t hash_id(const std::string &s, uint64_t salt)
{
    uint64_t h = 1469598103934665603ULL ^ salt;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ULL;
    return h;
}

/* Modals. */
enum Modal { NONE, TEXT_VIEW, OPTIONS, RESUME, INFO, TRACKS, DETAILS, SETTINGS, SPEAKERS, PHONE, SEARCH, LINK, CONFIRM_DELETE,
             ADD_SHARE, LOGIN, QUESTION, OSUB_SETUP, OSUB_RESULTS, PL_PICK, PL_NAME };
Modal modal = NONE;
float modal_anim;
int modal_focus;
std::string pending_path; /* RESUME: what to open */
int64_t pending_resume;
const char *info_title;
std::vector<std::string> info_lines;
std::string details_for; /* DETAILS: the file */
std::string delete_path;  /* CONFIRM_DELETE: the file or folder */
Modal delete_back = NONE; /* where Cancel goes */
int settings_cat;        /* SETTINGS: the category in focus */
bool settings_inside;    /* SETTINGS: focus in the right column */

/* Toast. */
std::string toast_text;
double toast_until;

void toast(const std::string &s, double secs = 2.5)
{
    toast_text = s;
    toast_until = now + secs;
}

/* Player. */
std::string playing_path, playing_name;
std::vector<int> playlist; /* the list it was started from */
int playlist_pos;
float osd_anim;
double osd_until;
int osd_row = 1, osd_col = 2; /* row 0: seek bar; row 1: buttons */
bool scrubbing;
int64_t scrub_ms;
double scrub_commit_at;
double last_seek_at = -10; /* ✕ right after a seek is "confirm", not pause */
float pulse_anim;    /* the big play/pause flash */
bool pulse_paused;
/* Picture: how the video fills the screen. Fit / Fill / Stretch only differ
 * when the video's shape isn't the screen's, so the forced shapes and the zoom
 * are there for 16:9 videos too. */
enum { PIC_FIT, PIC_FILL, PIC_STRETCH, PIC_16_9, PIC_4_3, PIC_21_9, PIC_ZOOM, PIC_MODES };
int aspect_mode;
const char *const aspect_names[PIC_MODES] = { "Fit (original shape)", "Fill the screen (crop)",
                                              "Stretch", "16:9", "4:3", "21:9 (cinema)",
                                              "Zoom 125%" };
/* Which part of the panel a button opened: -1 all (triangle). */
enum { SEC_ALL = -1, SEC_AUDIO, SEC_SUBS, SEC_SPEED, SEC_PICTURE };
int panel_section = SEC_ALL;
const float speeds[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
/* A-B repeat (ms, -1 unset) and the sleep timer. */
int64_t loop_a = -1, loop_b = -1;
enum { SLEEP_OFF, SLEEP_15, SLEEP_30, SLEEP_45, SLEEP_60, SLEEP_90, SLEEP_END, SLEEP_MODES };
const char *const sleep_names[SLEEP_MODES] = { "Off", "15 min", "30 min", "45 min", "60 min",
                                               "90 min", "End of this file" };
const int sleep_minutes[SLEEP_MODES] = { 0, 15, 30, 45, 60, 90, 0 };
int sleep_mode;
double sleep_at;
double frame_step_at = -10; /* the pause badge keeps out of the way of frame steps */
/* Repeat (VLC's loop button) and shuffle, remembered. */
/* Loop (the overlay's button): the playing video or song again and again.
 * Repeat the list (Settings > Playlists): after a list's last file, its first. */
bool loop_on()
{
    return pref_int("loop", 0) != 0;
}

bool repeat_list_on()
{
    return pref_int("repeat_list", 0) != 0;
}
/* The end of a file: the last picture stays, with Watch again. */
bool ended;
double ended_at;
bool from_playlist;    /* started from a playlist file: it goes on by itself */
PadState cur_pad;      /* this frame's pad (touchpad) */

/* A short buzz, when Settings > Controller vibration is on. */
void buzz(float strength, int ms)
{
    if (pref_int("haptics", 1))
        plat_pad_rumble(strength, ms);
}
bool quit;
float vid_rect[4];
bool show_video;
int64_t last_saved_ms;
double last_save_time;
bool was_active;

std::vector<MediaItem> &L()
{
    return library_items();
}

/* ---- data for the tabs ----------------------------------------------------------- */

std::vector<int> videos_list, music_list, pictures_list;

void rebuild_lists()
{
    auto &items = L();
    videos_list.clear();
    music_list.clear();
    pictures_list.clear();
    for (int i = 0; i < (int)items.size(); i++)
        if (!items[i].text && !items[i].archive && !items[i].other)
            (items[i].image ? pictures_list : items[i].audio ? music_list : videos_list).push_back(i);
    home_rows.clear();
    Row cont = { "Continue watching", {} }, recent = { "Recently added", {} },
        music = { "Music", {} }, all = { "All videos", {} }, fav = { "Favourites", {} },
        pictures = { "Pictures", {} };
    for (int i = 0; i < (int)items.size(); i++)
        if (items[i].favourite)
            fav.items.push_back(i);
    for (int i : videos_list)
        if (items[i].resume_ms > 0)
            cont.items.push_back(i);
    std::sort(cont.items.begin(), cont.items.end(),
              [&](int a, int b) { return items[a].last_played > items[b].last_played; });
    recent.items = videos_list;
    std::sort(recent.items.begin(), recent.items.end(),
              [&](int a, int b) { return items[a].mtime > items[b].mtime; });
    if (recent.items.size() > 12)
        recent.items.resize(12);
    music.items = music_list;
    pictures.items = pictures_list;
    all.title = "Videos";
    all.items = videos_list;
    /* "Recently added" only when it says something "Videos" doesn't. */
    if (recent.items.size() == videos_list.size() && videos_list.size() <= 12)
        recent.items.clear();
    for (Row *r : { &cont, &fav, &recent, &all, &music, &pictures })
        if (!r->items.empty())
            home_rows.push_back(*r);
    if (home_row >= (int)home_rows.size())
        home_row = home_rows.empty() ? 0 : (int)home_rows.size() - 1;
}

/* Network places in Browse: a server (saved SMB share or DLNA server), a
 * folder or file on it, and "Add a network share". */
enum { NET_NONE, NET_SERVER, NET_DIR, NET_FILE, NET_ADD, NET_ONLINE };

struct BrowseEntry {
    std::string name, path;
    bool dir;
    int item; /* library index for files; -1 for folders and playlists */
    bool playlist;
    int net = NET_NONE; /* path is then an address (smb://, upnp://) */
};
std::vector<BrowseEntry> browse_entries;
/* Inside the network: the places opened, (address, name), outermost first. */
std::vector<std::pair<std::string, std::string>> net_path;
std::string net_focus_after; /* select this address when the listing arrives */

bool audio_name(const std::string &name)
{
    std::string ext = name.substr(name.rfind('.') + 1);
    for (char &c : ext)
        c = (char)tolower((unsigned char)c);
    for (const char *a : { "mp3", "flac", "m4a", "aac", "ogg", "opus", "wav", "wma", "alac", "ape", "mka", "ac3", "dts" })
        if (ext == a)
            return true;
    return false;
}

/* A file on a share by its name, as ci_file() kinds: 0 video, 1 audio,
 * 3 text, 4 image, 5 another file (subtitles, databases...). */
int name_kind(const std::string &name)
{
    if (audio_name(name))
        return 1;
    std::string ext = name.substr(name.rfind('.') + 1);
    for (char &c : ext)
        c = (char)tolower((unsigned char)c);
    for (const char *x : { "jpg", "jpeg", "png", "gif", "bmp", "webp", "tif", "tiff" })
        if (ext == x)
            return 4;
    for (const char *x : { "txt", "nfo", "diz", "log", "md" })
        if (ext == x)
            return 3;
    for (const char *x : { "srt", "ass", "ssa", "sub", "idx", "vtt", "sup", "db", "ini", "sfv", "cue", "exe", "pdf" })
        if (ext == x)
            return 5;
    return 0;
}

/* A list of links (IPTV channels, radio), not a stream: .m3u and .pls, and
 * .m3u8 unless it's online (an online .m3u8 is usually a live HLS stream). */
bool is_playlist_url(const std::string &url)
{
    std::string u = url.substr(0, url.find('?'));
    size_t dot = u.rfind('.');
    if (dot == std::string::npos || u.find('/', dot) != std::string::npos)
        return false;
    const char *x = u.c_str() + dot + 1;
    if (!strcasecmp(x, "m3u8"))
        return u.compare(0, 4, "http") != 0;
    return !strcasecmp(x, "m3u") || !strcasecmp(x, "pls");
}

/* file:///abs/path with everything but plain characters escaped. */
std::string file_uri(std::string path)
{
    std::string uri = "file://";
    if (path[0] != '/') { /* the test build's data folder is relative */
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
            path = std::string(cwd) + "/" + path;
    }
    for (unsigned char c : path) {
        if (isalnum(c) || strchr("/-_.~", c)) {
            uri += (char)c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            uri += hex;
        }
    }
    return uri;
}

void rebuild_browse()
{
    browse_entries.clear();
    auto &items = L();
    if (!net_path.empty()) {
        if (net_list_state() == NET_READY)
            for (const NetEntry &e : net_list_entries()) {
                /* a channel list on a share opens like a folder */
                bool list = !e.dir && is_playlist_url(e.url);
                browse_entries.push_back({ e.name, e.url, e.dir, -1, list, e.dir || list ? NET_DIR : NET_FILE });
            }
        return;
    }
    if (browse_dir.empty()) {
        for (const std::string &r : library_roots()) {
            std::string name = r == library_media_dir() ? "Media" : r;
            browse_entries.push_back({ name, r, true, -1, false });
        }
        for (const NetServer &sv : net_servers())
            browse_entries.push_back({ sv.name, sv.url, true, -1, false, NET_SERVER });
        browse_entries.push_back({ "Add a network share", "", false, -1, false, NET_ADD });
        browse_entries.push_back({ "Free TV channels", "tv:", true, -1, false, NET_ONLINE });
        browse_entries.push_back({ "Internet radio", "radio:", true, -1, false, NET_ONLINE });
        return;
    }
    std::vector<std::string> dirs;
    for (int i = 0; i < (int)items.size(); i++) {
        const std::string &f = items[i].folder;
        if (f == browse_dir) {
            browse_entries.push_back({ items[i].ext.empty() ? items[i].name : items[i].name + "." + items[i].ext,
                                       items[i].path, false, i, false });
        } else if (f.compare(0, browse_dir.size() + 1, browse_dir + "/") == 0) {
            std::string sub = f.substr(browse_dir.size() + 1);
            sub = sub.substr(0, sub.find('/'));
            if (std::find(dirs.begin(), dirs.end(), sub) == dirs.end())
                dirs.push_back(sub);
        }
    }
    std::sort(dirs.begin(), dirs.end());
    std::vector<BrowseEntry> out;
    for (const std::string &d : dirs)
        out.push_back({ d, browse_dir + "/" + d, true, -1, false });
    for (const PlaylistFile &pl : library_playlists())
        if (pl.folder == browse_dir)
            out.push_back({ pl.name, pl.path, false, -1, true });
    out.insert(out.end(), browse_entries.begin(), browse_entries.end());
    browse_entries = out;
}

/* ---- playing -------------------------------------------------------------------------- */

void start_playback(int item, int64_t from_ms, const std::vector<int> &list, bool seamless = false,
                    bool background = false);
/* Music (or radio) going on while the menus are up: ○ from the player keeps it. */
bool music_bg;
double music_started_at;

/* The same file from the start (Loop, Watch again): the last picture stays
 * up until the first new one, no fade, no overlay: no black flash. */
void restart_playing(int item)
{
    player_stop();
    start_playback(item, 0, playlist, true);
}

void start_playback(int item, int64_t from_ms, const std::vector<int> &list, bool seamless, bool background)
{
    music_bg = background;
    music_started_at = now;
    MediaItem &m = L()[item];
    bool same_file = playing_path == m.path; /* Watch again, Loop: what was set stays */
    playing_path = m.path;
    playing_name = m.name;
    playlist = list;
    playlist_pos = (int)(std::find(list.begin(), list.end(), item) - list.begin());
    if (!seamless)
        video_forget_picture();
    library_set_busy(true);
    /* a disc folder: dvd:// or bluray:// (an .iso is found out by VLC itself) */
    std::string target = m.disc && m.ext == "DVD" ? "dvd://" + m.path
                       : m.disc && m.ext == "BLU-RAY" ? "bluray://" + m.path : m.path;
    if (!player_open(target, from_ms)) {
        toast(trf("Couldn't open %s", m.name.c_str()));
        library_set_busy(false);
        return;
    }
    if (!background)
        screen = PLAYER;
    if (!seamless && !background) {
        screen_fade = 0;
        osd_until = now + 3.5;
    }
    osd_row = 1;
    osd_col = 2;
    scrubbing = false;
    if (!same_file)
        loop_a = loop_b = -1;
    ended = false;
    last_saved_ms = from_ms;
    last_save_time = now;
    was_active = false;
    if (from_ms > 0)
        toast(trf("Resuming from %s", format_time(from_ms).c_str()));
}

/* ✕ on a file: ask about its resume point first, if it has one. */
void open_viewer(int item, const std::vector<int> &list);
void open_text(const MediaItem &m);
void open_archive(const MediaItem &m);

void open_item(int item, const std::vector<int> &list)
{
    from_playlist = false;
    MediaItem &m = L()[item];
    if (m.image) {
        open_viewer(item, list);
        return;
    }
    if (m.text) {
        open_text(m);
        return;
    }
    if (m.archive) {
        open_archive(m);
        return;
    }
    if (m.other) {
        /* listed so Browse shows everything, but not something VLC plays:
         * no player screen, just the note */
        toast(trf("Couldn't play %s", (m.ext.empty() ? m.name : m.name + "." + m.ext).c_str()), 2.5);
        return;
    }
    if (m.resume_ms > 0) {
        modal = RESUME;
        modal_focus = 0;
        pending_path = m.path;
        pending_resume = m.resume_ms;
        playlist = list;
        return;
    }
    start_playback(item, 0, list);
}

bool is_link(const std::string &p)
{
    return p.find("://") != std::string::npos;
}

void save_position(bool finished)
{
    if (playing_path.empty() || is_link(playing_path))
        return;
    int64_t t = finished ? 0 : player_time();
    library_set_resume(playing_path, t, player_length());
}

void leave_player(bool finished)
{
    music_bg = false;
    save_position(finished);
    player_stop();
    video_forget_picture();
    library_set_busy(false);
    screen = LIBRARY;
    screen_fade = 0;
    modal = NONE;
    playing_path.clear();
    rebuild_lists();
}

/* The playlist position after (step 1) or before (-1) the current one, with
 * shuffle and Repeat all; -1 if there is none. */
int neighbour_pos(int step)
{
    int n = (int)playlist.size();
    if (n == 0)
        return -1;
    if (step > 0 && pref_int("shuffle", 0) && n > 1) {
        int r;
        do
            r = rand() % n;
        while (r == playlist_pos);
        return r;
    }
    int pos = playlist_pos + step;
    if (pos >= n)
        return repeat_list_on() ? 0 : -1;
    if (pos < 0)
        return repeat_list_on() ? n - 1 : -1;
    return pos;
}

void play_neighbour(int step)
{
    int pos = neighbour_pos(step);
    if (pos < 0) {
        toast(step > 0 ? "That was the last one" : "That's the first one");
        return;
    }
    save_position(false);
    player_stop();
    start_playback(playlist[pos], L()[playlist[pos]].resume_ms, playlist);
}

/* The playing file's library index. */
int current_item()
{
    if (playlist_pos >= 0 && playlist_pos < (int)playlist.size())
        return playlist[playlist_pos];
    MediaItem *m = library_find(playing_path);
    return m ? (int)(m - &L()[0]) : -1;
}

void net_enter(const std::string &url, const std::string &name);

/* A playlist with links (IPTV channels, radio): its entries in Browse, where
 * each one plays as a stream. */
void open_channel_list(const std::string &path)
{
    std::string name = path.substr(path.rfind('/') + 1);
    name = name.substr(0, name.rfind('.'));
    screen = LIBRARY;
    tab = BROWSE;
    net_enter(file_uri(path), name);
}

/* A playlist file: its files, from the first (or a random one with shuffle). */
void play_playlist(const std::string &path)
{
    if (library_playlist_has_links(path)) {
        open_channel_list(path);
        return;
    }
    std::vector<int> list = library_playlist_items(path);
    if (list.empty()) {
        toast("No files of this playlist are in the library", 3);
        return;
    }
    int first = pref_int("shuffle", 0) ? rand() % (int)list.size() : 0;
    from_playlist = true;
    start_playback(list[first], 0, list);
}

/* ---- background, header, footer --------------------------------------------------- */

/* Modern: black, nothing else. Classic: the window's graphite body. */
void background()
{
    if (classic_look)
        vgrad(0, 0, 1920, 1080, IM_COL32(29, 31, 36, 255), IM_COL32(18, 19, 23, 255));
    else
        rect(0, 0, 1920, 1080, IM_COL32(0, 0, 0, 255));
}

/* Classic: the status bar at the bottom: what's here on the left, the
 * controller hints on the right. */
std::string status_text;

void status_bar()
{
    bevel(0, 1004, 1920, 76, IM_COL32(44, 48, 55, 255), IM_COL32(26, 28, 33, 255));
    rect(0, 1004, 1920, 1, IM_COL32(70, 75, 84, 255));
    text(f_reg, 21, 96, 1029, C_DIM, status_text.c_str(), 0, 820);
}

/* The only chrome: the tabs, centred at the top. The selected one sits on an
 * orange pill that glides to the next tab, stretching on the way (the leading
 * edge moves faster than the trailing one) and breathing softly. */
float pill_left = -1, pill_right = -1;

/* VLC 4's thin outline tab icons: house, film, note, and a folder for Browse. */
void tab_icon(int i, float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.085f; /* stroke */
    switch (i) {
    case 0: { /* house */
        dl->PathLineTo(P(cx - s * 0.5f, cy - s * 0.02f));
        dl->PathLineTo(P(cx, cy - s * 0.48f));
        dl->PathLineTo(P(cx + s * 0.5f, cy - s * 0.02f));
        dl->PathStroke(col, 0, t * S);
        dl->PathLineTo(P(cx - s * 0.36f, cy - s * 0.14f));
        dl->PathLineTo(P(cx - s * 0.36f, cy + s * 0.44f));
        dl->PathLineTo(P(cx + s * 0.36f, cy + s * 0.44f));
        dl->PathLineTo(P(cx + s * 0.36f, cy - s * 0.14f));
        dl->PathStroke(col, 0, t * S);
        stroke(cx - s * 0.1f, cy + s * 0.12f, s * 0.2f, s * 0.32f, col, 0, t);
        break;
    }
    case 1: /* film */
        stroke(cx - s * 0.5f, cy - s * 0.38f, s, s * 0.76f, col, s * 0.08f, t);
        line(cx - s * 0.3f, cy - s * 0.38f, cx - s * 0.3f, cy + s * 0.38f, col, t);
        line(cx + s * 0.3f, cy - s * 0.38f, cx + s * 0.3f, cy + s * 0.38f, col, t);
        line(cx - s * 0.5f, cy, cx - s * 0.3f, cy, col, t);
        line(cx + s * 0.3f, cy, cx + s * 0.5f, cy, col, t);
        break;
    case 2: /* note */
        ring(cx - s * 0.26f, cy + s * 0.3f, s * 0.15f, col, t);
        ring(cx + s * 0.3f, cy + s * 0.22f, s * 0.15f, col, t);
        line(cx - s * 0.11f, cy + s * 0.3f, cx - s * 0.11f, cy - s * 0.4f, col, t);
        line(cx + s * 0.45f, cy + s * 0.22f, cx + s * 0.45f, cy - s * 0.48f, col, t);
        line(cx - s * 0.11f, cy - s * 0.4f, cx + s * 0.45f, cy - s * 0.48f, col, t);
        break;
    case 3: /* playlists: lines and a play mark */
        line(cx - s * 0.5f, cy - s * 0.36f, cx + s * 0.5f, cy - s * 0.36f, col, t);
        line(cx - s * 0.5f, cy - s * 0.02f, cx + s * 0.08f, cy - s * 0.02f, col, t);
        line(cx - s * 0.5f, cy + s * 0.32f, cx + s * 0.08f, cy + s * 0.32f, col, t);
        tri(cx + s * 0.24f, cy - s * 0.06f, cx + s * 0.24f, cy + s * 0.42f, cx + s * 0.56f, cy + s * 0.18f, col);
        break;
    case 4: /* folder: tab on top, open front */
        dl->PathLineTo(P(cx - s * 0.5f, cy + s * 0.38f));
        dl->PathLineTo(P(cx - s * 0.5f, cy - s * 0.38f));
        dl->PathLineTo(P(cx - s * 0.12f, cy - s * 0.38f));
        dl->PathLineTo(P(cx + s * 0.0f, cy - s * 0.24f));
        dl->PathLineTo(P(cx + s * 0.42f, cy - s * 0.24f));
        dl->PathLineTo(P(cx + s * 0.42f, cy - s * 0.08f));
        dl->PathStroke(col, 0, t * S);
        dl->PathLineTo(P(cx - s * 0.5f, cy + s * 0.38f));
        dl->PathLineTo(P(cx - s * 0.32f, cy - s * 0.08f));
        dl->PathLineTo(P(cx + s * 0.56f, cy - s * 0.08f));
        dl->PathLineTo(P(cx + s * 0.38f, cy + s * 0.38f));
        dl->PathStroke(col, ImDrawFlags_Closed, t * S);
        break;
    }
}

/* Classic: the title band, its command-bar tabs and the time. */
void tab_bar_classic()
{
    bevel(0, 0, 1920, 108, IM_COL32(58, 63, 72, 255), IM_COL32(30, 33, 38, 255));
    /* The brand, top left, as it was: the cone and "VLC". */
    cone(96, 31, 46);
    text(f_bold, 40, 156, 30, C_TEXT, "VLC");
    const float size = 25, pad_x = 24, gap = 6, y = 28, h = 54, icon = 24, icon_gap = 12;
    float widths[TABS], total = 0;
    for (int i = 0; i < TABS; i++) {
        widths[i] = icon + icon_gap + text_size(f_semi, size, tab_names[i]).x + pad_x * 2;
        total += widths[i] + (i ? gap : 0);
    }
    float x0 = 960 - total / 2;
    auto keycap = [](float cx, const char *k) {
        rect(cx - 22, 41, 44, 28, IM_COL32(40, 44, 50, 255), 4);
        stroke(cx - 22, 41, 44, 28, IM_COL32(84, 90, 100, 255), 4, 1);
        text(f_semi, 17, cx, 45, C_DIM, k, 0.5f);
    };
    keycap(x0 - 40, "L1");
    keycap(x0 + total + 40, "R1");
    float sel_l = 0, sel_r = 0, x = x0;
    for (int i = 0; i < TABS; i++) {
        if (i == tab) {
            sel_l = x;
            sel_r = x + widths[i];
        }
        x += widths[i] + gap;
    }
    if (pill_left < 0) {
        pill_left = sel_l;
        pill_right = sel_r;
    }
    bool moving_right = sel_l > pill_left;
    pill_left = approach(pill_left, sel_l, moving_right ? 10 : 18);
    pill_right = approach(pill_right, sel_r, moving_right ? 18 : 10);
    /* The open tab: a button pressed in, an orange line under it that glides. */
    rect(pill_left, y, pill_right - pill_left, h, IM_COL32(20, 22, 26, 255), 4);
    stroke(pill_left, y, pill_right - pill_left, h, IM_COL32(10, 11, 13, 255), 4, 1);
    rect(pill_left + 2, y + h - 1, pill_right - pill_left - 4, 1, IM_COL32(255, 255, 255, 26));
    rect(pill_left + 10, y + h - 5, pill_right - pill_left - 20, 3, C_ORANGE, 1);
    x = x0;
    for (int i = 0; i < TABS; i++) {
        float a = focus_anim(1000 + i, i == tab, 12);
        ImU32 col = mix(C_DIM, IM_COL32(255, 255, 255, 255), a);
        tab_icon(i, x + pad_x + icon / 2, y + h / 2, icon, col);
        text(f_semi, size, x + pad_x + icon + icon_gap, y + (h - size * 1.25f) / 2, col, tab_names[i]);
        x += widths[i] + gap;
    }
    /* The time, as the taskbar shows it. */
    time_t tt = time(nullptr);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text(f_reg, 26, 1824, 38, C_DIM, clock, 1);
}

void tab_bar()
{
    if (classic_look) {
        tab_bar_classic();
        return;
    }
    const float size = 27, pad_x = 30, gap = 14, y = 34, h = 58, icon = 26, icon_gap = 14;
    float widths[TABS], total = 0;
    for (int i = 0; i < TABS; i++) {
        widths[i] = icon + icon_gap + text_size(f_bold, size, tab_names[i]).x + pad_x * 2;
        total += widths[i] + (i ? gap : 0);
    }
    float x0 = 960 - total / 2;
    /* The brand, top left, as it was: the cone and "VLC". */
    cone(96, 40, 46);
    text(f_bold, 38, 156, 41, C_TEXT, "VLC");
    /* The bar's track: a faint rounded strip with L1 / R1 at its ends. */
    rect(x0 - 70, y - 8, total + 140, h + 16, IM_COL32(255, 255, 255, 14), 10);
    text(f_bold, 16, x0 - 38, y + h / 2 - 10, C_FAINT, "L1", 0.5f);
    text(f_bold, 16, x0 + total + 38, y + h / 2 - 10, C_FAINT, "R1", 0.5f);

    float sel_l = 0, sel_r = 0, x = x0;
    for (int i = 0; i < TABS; i++) {
        if (i == tab) {
            sel_l = x;
            sel_r = x + widths[i];
        }
        x += widths[i] + gap;
    }
    if (pill_left < 0) {
        pill_left = sel_l;
        pill_right = sel_r;
    }
    bool moving_right = sel_l > pill_left;
    pill_left = approach(pill_left, sel_l, moving_right ? 9 : 18);
    pill_right = approach(pill_right, sel_r, moving_right ? 18 : 9);
    float breathe = 0.85f + 0.15f * sinf((float)now * 2.4f);
    glow(pill_left, y, pill_right - pill_left, h, 6, alpha(C_ORANGE, 0.9f * breathe), 22, 8);
    dl->AddRectFilled(P(pill_left, y), P(pill_right, y + h), C_ORANGE, 6 * S); /* square-edged, as asked: a 6 px soften only */

    x = x0;
    for (int i = 0; i < TABS; i++) {
        float a = focus_anim(1000 + i, i == tab, 12);
        ImU32 col = mix(C_DIM, IM_COL32(255, 255, 255, 255), a);
        tab_icon(i, x + pad_x + icon / 2, y + h / 2, icon, col);
        text(i == tab ? f_bold : f_semi, size, x + pad_x + icon + icon_gap, y + (h - size * 1.2f) / 2,
             col, tab_names[i]);
        x += widths[i] + gap;
    }
}

/* VLC's logo (assets/vlc-logo.png), loaded at start. */
ImTextureID logo_tex;
float logo_aspect = 1;

void logo(float cx, float cy, float h, float a = 1)
{
    if (!logo_tex) {
        cone(cx - h / 2, cy - h / 2, h, a);
        return;
    }
    float w = h * logo_aspect;
    dl->AddImage(logo_tex, P(cx - w / 2, cy - h / 2), P(cx + w / 2, cy + h / 2), ImVec2(0, 0),
                 ImVec2(1, 1), alpha(IM_COL32_WHITE, a));
}

/* ---- cards -------------------------------------------------------------------------- */

void ci_file(float cx, float cy, float s, int kind);

/* Classic: a thumbnail as a desktop shows "large icons": a thin frame, the
 * selection box around the picture and its caption, nothing grows or glows. */
void video_card_classic(float x, float y, float w, int item, float f)
{
    MediaItem &m = L()[item];
    float h = w * 9 / 16;
    sel_box(x - 12, y - 12, w + 24, h + 96, f);
    rect(x + 3, y + 4, w, h, IM_COL32(0, 0, 0, 110));
    if (m.thumb && m.audio) {
        vgrad(x, y, w, h, IM_COL32(52, 56, 64, 255), IM_COL32(30, 32, 37, 255));
        dl->AddImage(m.thumb, P(x + (w - h) / 2, y), P(x + (w + h) / 2, y + h));
    } else if (m.thumb) {
        float ta = m.thumb_aspect > 0 ? m.thumb_aspect : 16.0f / 9;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, ca = 16.0f / 9;
        if (ta > ca) {
            u0 = (1 - ca / ta) / 2;
            u1 = 1 - u0;
        } else {
            v0 = (1 - ta / ca) / 2;
            v1 = 1 - v0;
        }
        dl->AddImage(m.thumb, P(x, y), P(x + w, y + h), ImVec2(u0, v0), ImVec2(u1, v1));
    } else {
        vgrad(x, y, w, h, IM_COL32(52, 56, 64, 255), IM_COL32(30, 32, 37, 255));
        if (m.disc)
            ci_disc(x + w / 2, y + h / 2, h * 0.6f);
        else
            ci_file(x + w / 2, y + h / 2, h * 0.5f, m.image ? 4 : m.audio ? 1 : 0);
    }
    stroke(x, y, w, h, IM_COL32(255, 255, 255, 46), 0, 1);
    if (m.favourite)
        icon_star(x + 22, y + 22, 24, C_ORANGE);
    if (m.length_ms > 0) {
        std::string len = format_time(m.length_ms);
        float tw = text_size(f_semi, 18, len.c_str()).x;
        rect(x + w - tw - 22, y + h - 34, tw + 14, 26, IM_COL32(0, 0, 0, 190), 2);
        text(f_semi, 18, x + w - tw - 15, y + h - 31, C_TEXT, len.c_str());
    }
    if (m.resume_ms > 0 && m.length_ms > 0) {
        float frac = std::min(1.0f, (float)m.resume_ms / m.length_ms);
        rect(x, y + h - 4, w, 4, IM_COL32(0, 0, 0, 150));
        rect(x, y + h - 4, w * frac, 4, C_ORANGE);
    }
    text(f_reg, 22, x, y + h + 12, C_TEXT, m.name.c_str(), 0, w);
    std::string meta = m.audio && !m.artist.empty() ? m.artist : trf(m.image ? "%s image" : m.audio ? "%s audio" : "%s video", m.ext.c_str());
    if (m.resume_ms > 0)
        meta += ", " + trf("%s left", format_time(m.length_ms - m.resume_ms).c_str());
    text(f_reg, 18, x, y + h + 42, C_FAINT, meta.c_str(), 0, w);
}

/* A video card at (x, y), w wide (16:9 picture plus two lines of text). */
void video_card(float x, float y, float w, int item, float f)
{
    if (classic_look) {
        video_card_classic(x, y, w, item, f);
        return;
    }
    MediaItem &m = L()[item];
    float h = w * 9 / 16;
    float grow = 1 + 0.07f * ease(f);
    float cw = w * grow, ch = h * grow;
    float cx = x - (cw - w) / 2, cy = y - (ch - h) / 2 - 6 * f;
    glow(cx, cy + 10, cw, ch, 16, IM_COL32(0, 0, 0, 255), 22);
    if (f > 0.01f)
        glow(cx, cy, cw, ch, 16, alpha(C_ORANGE, f * 0.9f), 18);
    if (m.thumb) {
        float ta = m.thumb_aspect > 0 ? m.thumb_aspect : 16.0f / 9;
        /* Cover the 16:9 card, cropping the thumbnail's excess. */
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, ca = 16.0f / 9;
        if (ta > ca) {
            float k = ca / ta;
            u0 = (1 - k) / 2;
            u1 = 1 - u0;
        } else {
            float k = ta / ca;
            v0 = (1 - k) / 2;
            v1 = 1 - v0;
        }
        dl->AddImageRounded(m.thumb, P(cx, cy), P(cx + cw, cy + ch), ImVec2(u0, v0), ImVec2(u1, v1),
                            IM_COL32_WHITE, 16 * S);
    } else {
        dl->AddRectFilledMultiColor(P(cx, cy), P(cx + cw, cy + ch), tint_for(m.name, 0.30f),
                                    tint_for(m.name + "#", 0.22f), tint_for(m.name, 0.12f),
                                    tint_for(m.name + "#", 0.16f));
        stroke(cx, cy, cw, ch, C_BG, 16, 4); /* round off the gradient's corners */
        if (m.audio)
            icon_note(cx + cw / 2, cy + ch / 2, ch * 0.32f, IM_COL32(255, 255, 255, 170));
        else if (m.image)
            icon_picture(cx + cw / 2, cy + ch / 2, ch * 0.34f, IM_COL32(255, 255, 255, 150));
        else
            icon_film(cx + cw / 2, cy + ch / 2, ch * 0.34f, IM_COL32(255, 255, 255, 150));
    }
    if (m.favourite) {
        circle(cx + 30, cy + 30, 20, IM_COL32(0, 0, 0, 170));
        icon_star(cx + 30, cy + 30, 26, C_ORANGE);
    }
    /* Length, bottom right. */
    if (m.length_ms > 0) {
        std::string len = format_time(m.length_ms);
        float tw = text_size(f_semi, 18, len.c_str()).x;
        rect(cx + cw - tw - 26, cy + ch - 40, tw + 16, 28, IM_COL32(0, 0, 0, 170), 8);
        text(f_semi, 18, cx + cw - tw - 18, cy + ch - 36, C_TEXT, len.c_str());
    }
    /* Where it was stopped. */
    if (m.resume_ms > 0 && m.length_ms > 0) {
        float pw = cw - 32, frac = std::min(1.0f, (float)m.resume_ms / m.length_ms);
        rect(cx + 16, cy + ch - 10, pw, 5, IM_COL32(255, 255, 255, 60), 3);
        rect(cx + 16, cy + ch - 10, pw * frac, 5, C_ORANGE, 3);
    }
    if (f > 0.01f)
        stroke(cx - 2, cy - 2, cw + 4, ch + 4, alpha(IM_COL32(255, 255, 255, 255), f), 18, 3);
    text(f_semi, 22, x, y + h + 16 + 4 * f, mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), m.name.c_str(), 0, w);
    std::string meta = m.ext;
    if (m.resume_ms > 0)
        meta += "  \xC2\xB7  " + trf("%s left", format_time(m.length_ms - m.resume_ms).c_str());
    text(f_reg, 18, x, y + h + 46 + 4 * f, C_FAINT, meta.c_str(), 0, w);
}

/* ---- tabs ----------------------------------------------------------------------------- */

/* Nothing to play: VLC on the PC with no file open. The cone, centred on
 * black, and one quiet line about where files come from. */
void empty_state()
{
    logo(960, 540, 170);
    text(f_reg, 21, 960, 900, C_FAINT,
         "Plug in a USB drive, or copy files to /data/homebrew/" VLC_PS5_TITLE_ID "/media", 0.5f);
}

/* L2/R2 in a list sorted by name: to the previous / next first letter. */
char letter_of(const std::string &name)
{
    unsigned char c = name.empty() ? '#' : (unsigned char)name[0];
    return isalpha(c) ? (char)toupper(c) : '#';
}

int letter_jump(const std::vector<std::string> &names, int cur, int dir)
{
    int n = (int)names.size();
    if (n == 0 || cur < 0 || cur >= n)
        return cur;
    char k = letter_of(names[cur]);
    if (dir > 0) {
        for (int i = cur + 1; i < n; i++)
            if (letter_of(names[i]) != k)
                return i;
        return cur;
    }
    int start = cur;
    while (start > 0 && letter_of(names[start - 1]) == k)
        start--;
    if (start < cur)
        return start; /* first to the top of this letter */
    if (start == 0)
        return 0;
    char pk = letter_of(names[start - 1]);
    int i = start - 1;
    while (i > 0 && letter_of(names[i - 1]) == pk)
        i--;
    return i;
}

/* L2/R2 on a list of library items: jump and say where it landed. */
void jump_by_letter(const std::vector<int> &list, int &focus)
{
    if (!hit(PAD_L2) && !hit(PAD_R2))
        return;
    std::vector<std::string> names;
    for (int i : list)
        names.push_back(L()[i].name);
    int to = letter_jump(names, focus, hit(PAD_R2) ? 1 : -1);
    if (to != focus) {
        focus = to;
        toast(std::string(1, letter_of(names[to])), 0.8);
    }
}

/* The file the focus is on in the library, or -1. */
int focused_item()
{
    switch (tab) {
    case HOME: {
        if (home_rows.empty())
            return -1;
        Row &r = home_rows[home_row];
        int c = home_col[home_row % 8];
        return c >= 0 && c < (int)r.items.size() ? r.items[c] : -1;
    }
    case VIDEOS:
        return videos_list.empty() ? -1 : videos_list[std::min(video_focus, (int)videos_list.size() - 1)];
    case MUSIC: {
        int np = 0; /* playlists have their own tab */
        if (music_focus < np || music_list.empty())
            return -1;
        return music_list[std::min(music_focus - np, (int)music_list.size() - 1)];
    }
    case BROWSE:
        if (browse_focus < (int)browse_entries.size() && browse_entries[browse_focus].item >= 0)
            return browse_entries[browse_focus].item;
        return -1;
    default:
        return -1;
    }
}

/* △ favourite, □ details: the same on every tab. */
void ask_delete(const std::string &path, Modal back)
{
    delete_path = path;
    delete_back = back;
    modal = CONFIRM_DELETE;
    modal_focus = 1; /* Cancel first: nothing goes by accident */
}

void library_actions()
{
    if (tab == BROWSE && hit(PAD_SQUARE) && browse_focus < (int)browse_entries.size() &&
        browse_entries[browse_focus].net != NET_NONE) {
        const BrowseEntry &e = browse_entries[browse_focus];
        if (e.net == NET_SERVER && net_is_saved_server(e.path))
            ask_delete(e.path, NONE);
        return;
    }
    if (tab == BROWSE && hit(PAD_SQUARE) && browse_focus < (int)browse_entries.size()) {
        const BrowseEntry &e = browse_entries[browse_focus];
        bool root = false;
        for (const std::string &r : library_roots())
            root = root || r == e.path;
        if (e.dir && !root) {
            ask_delete(e.path, NONE);
            return;
        }
        if (e.playlist) {
            ask_delete(e.path, NONE);
            return;
        }
    }
    int item = focused_item();
    if (item < 0)
        return;
    if (hit(PAD_TRIANGLE) && !L()[item].other) { /* no favourites for files VLC doesn't play */
        std::string path = L()[item].path;
        bool on = library_toggle_favourite(path);
        rebuild_lists();
        rebuild_browse();
        toast(on ? "Added to Favourites" : "Removed from Favourites", 1.6);
        buzz(0.4f, 40);
    } else if (hit(PAD_SQUARE)) {
        details_for = L()[item].path;
        library_details(details_for); /* start reading */
        modal = DETAILS;
        modal_anim = 0;
    }
}

void tab_home()
{
    if (home_rows.empty()) {
        empty_state();
        return;
    }
    /* Home: rows by category (Continue watching, Recently added, Videos,
     * Music), nothing else. */
    if (home_row < 0)
        home_row = 0;
    if (hit(PAD_UP) && home_row > 0)
        home_row--;
    if (hit(PAD_DOWN) && home_row + 1 < (int)home_rows.size())
        home_row++;
    {
        Row &cur = home_rows[home_row];
        int &col = home_col[home_row % 8];
        if (col >= (int)cur.items.size())
            col = (int)cur.items.size() - 1;
        if (hit(PAD_LEFT) && col > 0)
            col--;
        if (hit(PAD_RIGHT) && col + 1 < (int)cur.items.size())
            col++;
        if (hit(PAD_CROSS))
            open_item(cur.items[col], cur.items);
    }

    const float card_w = 352, gap = 34, row_h = 372, top = 150;
    /* Scroll only as far as needed to keep the focused row on screen. */
    float bottom = top + (home_row + 1) * row_h;
    home_scroll = approach(home_scroll, std::max(0.0f, bottom - 1000), 10);
    dl->PushClipRect(P(0, 112), P(1920, 1080), true);
    for (int r = 0; r < (int)home_rows.size(); r++) {
        Row &row = home_rows[r];
        float y = top + r * row_h - home_scroll;
        if (y < -row_h || y > 1080)
            continue;
        bool row_on = r == home_row;
        float ra = focus_anim(2000 + r, row_on, 10);
        if (classic_look) {
            classic_heading(96, y + 8, row.title, row.items.size());
        } else {
            text(f_bold, 30, 96, y, mix(C_DIM, C_TEXT, ra), row.title);
            char count[32];
            snprintf(count, sizeof(count), "%zu", row.items.size());
            text(f_semi, 22, 104 + text_size(f_bold, 30, row.title).x, y + 7, C_FAINT, count);
        }
        int c = home_col[r % 8];
        float want = c * (card_w + gap);
        float visible = 1920 - 96 * 2;
        float scroll_target = std::max(0.0f, want - (visible - card_w) * 0.35f);
        home_col_scroll[r % 8] = approach(home_col_scroll[r % 8], scroll_target, 12);
        for (int i = 0; i < (int)row.items.size(); i++) {
            float x = 96 + i * (card_w + gap) - home_col_scroll[r % 8];
            if (x > 1920 || x + card_w < -100)
                continue;
            bool focused = row_on && i == c && modal == NONE;
            float f = focus_anim(hash_id(L()[row.items[i]].path, 1 + r), focused);
            video_card(x, y + 54, card_w, row.items[i], f);
        }
    }
    dl->PopClipRect();
}

void tab_videos()
{
    if (videos_list.empty()) {
        empty_state();
        return;
    }
    const int cols = 4;
    const float card_w = 396, gap_x = (1920 - 192 - cols * card_w) / (cols - 1), row_h = 330;
    int n = (int)videos_list.size();
    if (video_focus >= n)
        video_focus = n - 1;
    if (hit(PAD_LEFT) && video_focus % cols > 0)
        video_focus--;
    if (hit(PAD_RIGHT) && video_focus % cols < cols - 1 && video_focus + 1 < n)
        video_focus++;
    if (hit(PAD_UP) && video_focus >= cols)
        video_focus -= cols;
    if (hit(PAD_DOWN) && video_focus + cols < n)
        video_focus += cols;
    jump_by_letter(videos_list, video_focus);
    if (hit(PAD_CROSS))
        open_item(videos_list[video_focus], videos_list);
    int frow = video_focus / cols;
    /* Scroll only when the focused row would leave the screen. */
    video_scroll = approach(video_scroll, std::max(0.0f, 206 + (frow + 1) * row_h - 1000), 10);
    if (classic_look) {
        classic_heading(96, 140, "All videos", (size_t)n);
    } else {
        text(f_bold, 30, 96, 140, C_TEXT, "All videos");
        char count[32];
        snprintf(count, sizeof(count), "%d", n);
        text(f_semi, 22, 104 + text_size(f_bold, 30, "All videos").x, 147, C_FAINT, count);
    }
    dl->PushClipRect(P(0, 186), P(1920, 1080), true);
    for (int i = 0; i < n; i++) {
        float x = 96 + (i % cols) * (card_w + gap_x);
        float y = 206 + (i / cols) * row_h - video_scroll;
        if (y > 1080 || y + row_h < 100)
            continue;
        float f = focus_anim(hash_id(L()[videos_list[i]].path, 100), i == video_focus && modal == NONE);
        video_card(x, y, card_w, videos_list[i], f);
    }
    dl->PopClipRect();
}

/* A focusable list row (Music, Browse, menus). */
void list_row(float x, float y, float w, float h, float f, bool even)
{
    if (classic_look) {
        sel_box(x, y, w, h, f);
        return;
    }
    if (even)
        rect(x, y, w, h, IM_COL32(255, 255, 255, 6), 12);
    if (f > 0.01f) {
        glow(x, y, w, h, 14, alpha(C_ORANGE, f * 0.5f), 12, 6);
        rect(x, y, w, h, alpha(C_CARD_HI, f), 14);
        rect(x, y + 12, 5, h - 24, alpha(C_ORANGE, f), 3);
    }
}

void tab_music()
{
    auto &pls = library_playlists();
    int np = 0, ns = (int)music_list.size(), n = np + ns; /* playlists have their own tab */
    if (n == 0) {
        empty_state();
        return;
    }
    if (music_focus >= n)
        music_focus = n - 1;
    if (hit(PAD_UP) && music_focus > 0)
        music_focus--;
    if (hit(PAD_DOWN) && music_focus + 1 < n)
        music_focus++;
    if (music_focus >= np) {
        int song = music_focus - np;
        jump_by_letter(music_list, song);
        music_focus = np + song;
    }
    if (hit(PAD_CROSS)) {
        if (music_focus < np)
            play_playlist(pls[music_focus].path);
        else
            open_item(music_list[music_focus - np], music_list);
    }
    if (classic_look) {
        /* Playlists, then songs, as a Details list. */
        const float top = 190, bottom = 996, rh = 48, head_h = 50;
        const float col_folder = 900, col_type = 1320, col_len = 1792;
        classic_columns(top - 50, { { "Name", 168, 0 }, { "Artist", col_folder, 0 }, { "Album", col_type, 0 }, { "Length", col_len, 1 } });
        std::vector<float> ys(n);
        float yy = 0;
        for (int i = 0; i < n; i++) {
            if (i == 0 || i == np)
                yy += head_h + (i ? 10 : 0);
            ys[i] = yy;
            yy += rh;
        }
        float view = bottom - top;
        float want = std::max(0.0f, std::min(ys[music_focus] + rh / 2 - view * 0.45f, std::max(0.0f, yy - view + 10)));
        music_scroll = approach(music_scroll, want, 14);
        dl->PushClipRect(P(0, top - 6), P(1920, bottom), true);
        for (int i = 0; i < n; i++) {
            float y = top + ys[i] - music_scroll;
            if (i == 0 || i == np)
                classic_heading(112, y - head_h + 16, i < np ? "Playlists" : "Songs", i < np ? (size_t)np : (size_t)ns);
            if (y > bottom || y + rh < top - 60)
                continue;
            if (i < np) {
                const PlaylistFile &pl = pls[i];
                float f = focus_anim(hash_id(pl.path, 210), i == music_focus && modal == NONE, 16);
                sel_box(100, y + 2, 1720, rh - 4, f);
                ci_file(138, y + rh / 2, 30, 2);
                text(f_reg, 23, 168, y + 11, C_TEXT, pl.name.c_str(), 0, col_folder - 200);
                text(f_reg, 21, col_folder, y + 13, C_DIM, "Playlist");
                text(f_reg, 21, col_type, y + 13, C_DIM, pl.folder.substr(pl.folder.rfind('/') + 1).c_str(), 0, col_len - 160 - col_type);
                continue;
            }
            MediaItem &m = L()[music_list[i - np]];
            float f = focus_anim(hash_id(m.path, 200), i == music_focus && modal == NONE, 16);
            sel_box(100, y + 2, 1720, rh - 4, f);
            if (m.thumb)
                dl->AddImage(m.thumb, P(123, y + rh / 2 - 15), P(153, y + rh / 2 + 15));
            else
                ci_file(138, y + rh / 2, 30, 1);
            text(f_reg, 23, 168, y + 11, C_TEXT, m.name.c_str(), 0, col_folder - 200);
            if (m.favourite)
                icon_star(168 + std::min(col_folder - 200, text_size(f_reg, 23, m.name.c_str()).x) + 18, y + rh / 2, 18, C_ORANGE);
            text(f_reg, 21, col_folder, y + 13, C_DIM, m.artist.empty() ? "Unknown artist" : m.artist.c_str(), 0, col_type - col_folder - 30);
            text(f_reg, 21, col_type, y + 13, C_DIM, m.album.empty() ? m.folder.substr(m.folder.rfind('/') + 1).c_str() : m.album.c_str(), 0, col_len - 160 - col_type);
            if (m.length_ms > 0)
                text(f_reg, 21, col_len, y + 13, C_DIM, format_time(m.length_ms).c_str(), 1);
        }
        dl->PopClipRect();
        return;
    }
    const float row_h = 84;
    music_scroll = approach(music_scroll, std::max(0.0f, music_focus * row_h - 300), 12);
    text(f_bold, 30, 96, 140, C_TEXT, "Music");
    dl->PushClipRect(P(0, 196), P(1920, 1080), true);
    for (int i = 0; i < n; i++) {
        float y = 206 + i * row_h - music_scroll;
        if (y > 1080 || y + row_h < 150)
            continue;
        if (i < np) {
            const PlaylistFile &pl = pls[i];
            float f = focus_anim(hash_id(pl.path, 210), i == music_focus && modal == NONE);
            list_row(96, y, 1728, row_h - 10, f, i % 2 == 0);
            rect(120, y + 11, 52, 52, alpha(C_ORANGE, 0.25f + 0.25f * f), 10);
            icon_list(146, y + 37, 30, C_TEXT);
            text(f_semi, 26, 196, y + 10, mix(C_DIM, C_TEXT, 0.7f + 0.3f * f), pl.name.c_str(), 0, 1200);
            std::string sub = std::string(tr("Playlist")) + "  \xC2\xB7  " + pl.folder.substr(pl.folder.rfind('/') + 1);
            text(f_reg, 19, 196, y + 42, C_FAINT, sub.c_str(), 0, 1200);
            continue;
        }
        MediaItem &m = L()[music_list[i - np]];
        float f = focus_anim(hash_id(m.path, 200), i == music_focus && modal == NONE);
        list_row(96, y, 1728, row_h - 10, f, i % 2 == 0);
        if (m.thumb) {
            dl->AddImage(m.thumb, P(120, y + 11), P(172, y + 63));
        } else {
            dl->AddRectFilledMultiColor(P(120, y + 11), P(172, y + 63), tint_for(m.name, 0.55f),
                                        tint_for(m.name + "#", 0.45f), tint_for(m.name, 0.30f),
                                        tint_for(m.name + "#", 0.35f));
            icon_note(146, y + 37, 26, IM_COL32(255, 255, 255, 220));
        }
        text(f_semi, 26, 196, y + 10, mix(C_DIM, C_TEXT, 0.7f + 0.3f * f), m.name.c_str(), 0, 1200);
        if (m.favourite)
            icon_star(196 + std::min(1200.0f, text_size(f_semi, 26, m.name.c_str()).x) + 24, y + 26, 22, C_ORANGE);
        std::string sub = !m.artist.empty() ? m.artist + (m.album.empty() ? "" : "  \xC2\xB7  " + m.album)
                                            : m.ext + "  \xC2\xB7  " + m.folder.substr(m.folder.rfind('/') + 1);
        text(f_reg, 19, 196, y + 42, C_FAINT, sub.c_str(), 0, 1200);
        if (m.length_ms > 0)
            text(f_semi, 22, 1792, y + 24, C_DIM, format_time(m.length_ms).c_str(), 1);
    }
    dl->PopClipRect();
}

/* ---- Playlists ------------------------------------------------------------------- */

/* The list of playlists ("New playlist" first), or one opened: its files. */
int pl_focus, pl_item_focus;
float pl_scroll, pl_item_scroll;
std::string pl_open;          /* the playlist being looked at, "" for the list */
std::vector<int> pl_items;    /* its files that are in the library */
double pl_items_at = -100;    /* read again now and then (drives come and go) */
std::string pl_add_file;      /* PL_PICK / PL_NAME: the file to put in */
std::string pl_rename;        /* PL_NAME: the playlist renamed, "" for a new one */
void icon_plus_circle(float cx, float cy, float s, ImU32 col);
void open_playlist_name(const std::string &rename);

const PlaylistFile *playlist_by_path(const std::string &path)
{
    for (const PlaylistFile &p : library_playlists())
        if (p.path == path)
            return &p;
    return nullptr;
}

void pl_reload()
{
    pl_items = library_playlist_items(pl_open);
    pl_items_at = now;
    if (pl_item_focus >= (int)pl_items.size())
        pl_item_focus = std::max(0, (int)pl_items.size() - 1);
}

/* "3 videos · 1:24:10", for a list of library files. */
std::string pl_summary(const std::vector<int> &list)
{
    int64_t total = 0;
    for (int i : list)
        total += std::max<int64_t>(0, L()[i].length_ms);
    std::string out = list.size() == 1 ? std::string(tr("1 item")) : trf("%d items", (int)list.size());
    if (total > 0)
        out += "  \xC2\xB7  " + format_time(total);
    return out;
}

float text_wrapped(ImFont *f, float size, float x, float y, ImU32 col, const std::string &s_in, float max_w, int max_lines);

/* A playlist's files, looked up again every few seconds (for covers and counts). */
const std::vector<int> &playlist_items_cached(const std::string &path)
{
    struct Cached {
        double at;
        std::vector<int> items;
    };
    static std::unordered_map<std::string, Cached> cache;
    Cached &c = cache[path];
    if (c.at == 0 || now - c.at > 3) {
        c.items = library_playlist_items(path);
        c.at = now;
    }
    return c.items;
}

/* How many entries a playlist names: its library files, or every line of a
 * channel list (links aren't library files). */
int playlist_count_cached(const std::string &path)
{
    struct Cached {
        double at;
        int n;
    };
    static std::unordered_map<std::string, Cached> cache;
    Cached &c = cache[path];
    if (c.at == 0 || now - c.at > 3) {
        c.n = library_playlist_has_links(path) ? library_playlist_count(path) : (int)playlist_items_cached(path).size();
        c.at = now;
    }
    return c.n;
}

/* A picture covering (x, y, w, h): the thumbnail cropped to that shape. */
void thumb_cover(const MediaItem &m, float x, float y, float w, float h, float round, ImDrawFlags corners)
{
    float ta = m.thumb_aspect > 0 ? m.thumb_aspect : (m.audio ? 1.0f : 16.0f / 9), ca = w / h;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (ta > ca) {
        u0 = (1 - ca / ta) / 2;
        u1 = 1 - u0;
    } else {
        v0 = (1 - ta / ca) / 2;
        v1 = 1 - v0;
    }
    dl->AddImageRounded(m.thumb, P(x, y), P(x + w, y + h), ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE,
                        round * S, corners);
}

/* A playlist's cover, as VLC 4 makes it: its first file's picture, or the
 * first four in a 2x2 mosaic, each a 16:9 picture of its own (no squeezing). */
void playlist_cover(const std::string &path, float x, float y, float w, float h, float round)
{
    std::vector<int> pics;
    for (int i : playlist_items_cached(path))
        if (i < (int)L().size() && L()[i].thumb && pics.size() < 4)
            pics.push_back(i);
    if (pics.empty()) {
        dl->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), tint_for(path, 0.30f), tint_for(path + "#", 0.22f),
                                    tint_for(path, 0.12f), tint_for(path + "#", 0.16f));
        if (round > 0)
            stroke(x, y, w, h, C_BG, round, 4); /* round off the gradient's corners */
        icon_list(x + w / 2, y + h / 2, h * 0.34f, IM_COL32(255, 255, 255, 170));
        return;
    }
    if (pics.size() < 4) {
        thumb_cover(L()[pics[0]], x, y, w, h, round, ImDrawFlags_RoundCornersAll);
        return;
    }
    const float gap = 3;
    float cw = (w - gap) / 2, ch = (h - gap) / 2;
    static const ImDrawFlags corner[4] = { ImDrawFlags_RoundCornersTopLeft, ImDrawFlags_RoundCornersTopRight,
                                           ImDrawFlags_RoundCornersBottomLeft, ImDrawFlags_RoundCornersBottomRight };
    rect(x, y, w, h, IM_COL32(0, 0, 0, 255), round);
    for (int k = 0; k < 4; k++)
        thumb_cover(L()[pics[k]], x + (k % 2) * (cw + gap), y + (k / 2) * (ch + gap), cw, ch, round, corner[k]);
}

/* A playlist card, the size of a video card: the cover, the name, what's in
 * it. path "" is the "New playlist" card. */
void playlist_card(float x, float y, float w, const PlaylistFile *pl, float f)
{
    float h = w * 9 / 16;
    std::string sub;
    if (pl) {
        const std::vector<int> &items = playlist_items_cached(pl->path);
        sub = library_playlist_ours(pl->path) ? pl_summary(items)
                                              : std::string(tr("Playlist")) + "  \xC2\xB7  " + pl->folder.substr(pl->folder.rfind('/') + 1);
    }
    const char *name = pl ? pl->name.c_str() : tr("New playlist");
    if (classic_look) {
        sel_box(x - 12, y - 12, w + 24, h + 96, f);
        rect(x + 3, y + 4, w, h, IM_COL32(0, 0, 0, 110));
        if (pl) {
            playlist_cover(pl->path, x, y, w, h, 0);
        } else {
            vgrad(x, y, w, h, IM_COL32(52, 56, 64, 255), IM_COL32(30, 32, 37, 255));
            ci_file(x + w / 2, y + h / 2, h * 0.5f, 2);
            circle(x + w / 2 + h * 0.16f, y + h / 2 + h * 0.16f, h * 0.12f, C_ORANGE);
            rect(x + w / 2 + h * 0.16f - h * 0.07f, y + h / 2 + h * 0.16f - h * 0.015f, h * 0.14f, h * 0.03f, IM_COL32_WHITE);
            rect(x + w / 2 + h * 0.16f - h * 0.015f, y + h / 2 + h * 0.16f - h * 0.07f, h * 0.03f, h * 0.14f, IM_COL32_WHITE);
        }
        stroke(x, y, w, h, IM_COL32(255, 255, 255, 46), 0, 1);
        text(f_reg, 22, x, y + h + 12, C_TEXT, name, 0, w);
        if (pl)
            text(f_reg, 18, x, y + h + 42, C_FAINT, sub.c_str(), 0, w);
        else /* long in some languages: two lines */
            text_wrapped(f_reg, 18, x, y + h + 42, C_FAINT, "Then add videos and songs from anywhere with the Options menu", w, 2);
        return;
    }
    float grow = 1 + 0.07f * ease(f);
    float cw = w * grow, ch = h * grow;
    float cx = x - (cw - w) / 2, cy = y - (ch - h) / 2 - 6 * f;
    glow(cx, cy + 10, cw, ch, 16, IM_COL32(0, 0, 0, 255), 22);
    if (f > 0.01f)
        glow(cx, cy, cw, ch, 16, alpha(C_ORANGE, f * 0.9f), 18);
    if (pl) {
        playlist_cover(pl->path, cx, cy, cw, ch, 16);
        /* how many, bottom right, like a video's length */
        std::string count = std::to_string(playlist_count_cached(pl->path));
        float tw = text_size(f_semi, 18, count.c_str()).x;
        rect(cx + cw - tw - 54, cy + ch - 40, tw + 44, 28, IM_COL32(0, 0, 0, 170), 8);
        icon_list(cx + cw - tw - 38, cy + ch - 26, 16, C_TEXT);
        text(f_semi, 18, cx + cw - tw - 18, cy + ch - 36, C_TEXT, count.c_str());
    } else {
        rect(cx, cy, cw, ch, IM_COL32(255, 255, 255, 12), 16);
        if (f > 0.01f)
            rect(cx, cy, cw, ch, alpha(C_ORANGE, 0.10f * f), 16); /* a light tint when chosen */
        stroke(cx, cy, cw, ch, alpha(C_ORANGE, 0.35f + 0.45f * f), 16, 2);
        icon_plus_circle(cx + cw / 2, cy + ch / 2, ch * 0.32f, mix(C_DIM, C_ORANGE, 0.5f + 0.5f * f));
    }
    if (f > 0.01f)
        stroke(cx - 2, cy - 2, cw + 4, ch + 4, alpha(IM_COL32(255, 255, 255, 255), f), 18, 3);
    text(f_semi, 22, x, y + h + 16 + 4 * f, mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), name, 0, w);
    if (pl)
        text(f_reg, 18, x, y + h + 46 + 4 * f, C_FAINT, sub.c_str(), 0, w);
    else /* long in some languages: two lines */
        text_wrapped(f_reg, 18, x, y + h + 46 + 4 * f, C_FAINT, "Then add videos and songs from anywhere with the Options menu", w, 2);
}

void tab_playlists()
{
    auto &pls = library_playlists();
    if (!pl_open.empty() && !playlist_by_path(pl_open))
        pl_open.clear(); /* deleted, or its drive went */
    if (pl_open.empty()) {
        /* "New playlist", then the playlists, as cards in rows of four */
        const int cols = 4;
        const float card_w = 396, gap_x = (1920 - 192 - cols * card_w) / (cols - 1), row_h = 330;
        int n = 1 + (int)pls.size();
        if (pl_focus >= n)
            pl_focus = n - 1;
        if (hit(PAD_LEFT) && pl_focus % cols > 0)
            pl_focus--;
        if (hit(PAD_RIGHT) && pl_focus % cols < cols - 1 && pl_focus + 1 < n)
            pl_focus++;
        if (hit(PAD_UP) && pl_focus >= cols)
            pl_focus -= cols;
        if (hit(PAD_DOWN) && pl_focus + cols < n)
            pl_focus += cols;
        if (hit(PAD_CROSS)) {
            if (pl_focus == 0) {
                pl_add_file.clear();
                open_playlist_name("");
            } else if (library_playlist_has_links(pls[pl_focus - 1].path)) {
                open_channel_list(pls[pl_focus - 1].path);
            } else {
                pl_open = pls[pl_focus - 1].path;
                pl_item_focus = 0;
                pl_item_scroll = 0;
                pl_reload();
            }
        } else if (hit(PAD_TRIANGLE) && pl_focus > 0) {
            play_playlist(pls[pl_focus - 1].path);
        } else if (hit(PAD_SQUARE) && pl_focus > 0 && library_playlist_ours(pls[pl_focus - 1].path)) {
            ask_delete(pls[pl_focus - 1].path, NONE);
        }
        int frow = pl_focus / cols;
        pl_scroll = approach(pl_scroll, std::max(0.0f, 206 + (frow + 1) * row_h - 1000), 10);
        if (classic_look) {
            classic_heading(96, 140, "Playlists", pls.size());
        } else {
            text(f_bold, 30, 96, 140, C_TEXT, "Playlists");
            if (!pls.empty())
                text(f_semi, 22, 104 + text_size(f_bold, 30, "Playlists").x, 147, C_FAINT, std::to_string(pls.size()).c_str());
        }
        dl->PushClipRect(P(0, 186), P(1920, 1080), true);
        for (int i = 0; i < n; i++) {
            float x = 96 + (i % cols) * (card_w + gap_x);
            float y = 206 + (i / cols) * row_h - pl_scroll;
            if (y > 1080 || y + row_h < 100)
                continue;
            float f = focus_anim(hash_id(i ? pls[i - 1].path : "+new", 230), i == pl_focus && modal == NONE);
            playlist_card(x, y, card_w, i ? &pls[i - 1] : nullptr, f);
        }
        dl->PopClipRect();
        return;
    }

    /* One playlist open. */
    const PlaylistFile *pl = playlist_by_path(pl_open);
    bool ours = library_playlist_ours(pl_open);
    if (now - pl_items_at > 2)
        pl_reload();
    int n = (int)pl_items.size();
    if (hit(PAD_CIRCLE)) {
        for (int i = 0; i < (int)pls.size(); i++)
            if (pls[i].path == pl_open)
                pl_focus = i + 1; /* back on the one just left */
        pl_open.clear();
        return;
    }
    if (n > 0) {
        if (hit(PAD_UP) && pl_item_focus > 0)
            pl_item_focus--;
        if (hit(PAD_DOWN) && pl_item_focus + 1 < n)
            pl_item_focus++;
        std::string cur = L()[pl_items[pl_item_focus]].path;
        if (hit(PAD_CROSS)) {
            std::vector<int> list = pl_items;
            open_item(list[pl_item_focus], list);
            from_playlist = true; /* the next file follows by itself */
            return;
        } else if (hit(PAD_TRIANGLE)) {
            play_playlist(pl_open);
            return;
        } else if (ours && hit(PAD_SQUARE)) {
            if (library_playlist_remove(pl_open, cur))
                toast(trf("Removed from %s", pl->name.c_str()), 1.8);
            pl_reload();
        } else if (ours && (hit(PAD_L2) || hit(PAD_R2))) {
            int step = hit(PAD_R2) ? 1 : -1;
            if (library_playlist_move(pl_open, cur, step)) {
                pl_reload();
                for (int i = 0; i < (int)pl_items.size(); i++)
                    if (L()[pl_items[i]].path == cur)
                        pl_item_focus = i; /* the selection goes with it */
            }
        }
    }
    n = (int)pl_items.size();
    std::string sum = pl_summary(pl_items);
    if (classic_look) {
        const float top = 190, bottom = 996, rh = 48, head_h = 50;
        const float col_type = 1100, col_len = 1792;
        classic_columns(top - 50, { { "Name", 168, 0 }, { "Type", col_type, 0 }, { "Length", col_len, 1 } });
        float view = bottom - top;
        float want = std::max(0.0f, std::min(head_h + pl_item_focus * rh + rh / 2 - view * 0.45f,
                                             std::max(0.0f, head_h + n * rh - view + 10)));
        pl_item_scroll = approach(pl_item_scroll, want, 14);
        dl->PushClipRect(P(0, top - 6), P(1920, bottom), true);
        classic_heading(112, top - pl_item_scroll + 16, pl->name.c_str(), (size_t)n);
        for (int i = 0; i < n; i++) {
            float y = top + head_h + i * rh - pl_item_scroll;
            if (y > bottom || y + rh < top - 60)
                continue;
            MediaItem &m = L()[pl_items[i]];
            float f = focus_anim(hash_id(m.path, 240), i == pl_item_focus && modal == NONE, 16);
            sel_box(100, y + 2, 1720, rh - 4, f);
            text(f_reg, 21, 128, y + 13, C_FAINT, std::to_string(i + 1).c_str(), 0.5f);
            ci_file(176, y + rh / 2, 30, m.audio ? 1 : 0);
            text(f_reg, 23, 206, y + 11, C_TEXT, m.name.c_str(), 0, col_type - 240);
            text(f_reg, 21, col_type, y + 13, C_DIM, trf(m.audio ? "%s audio" : "%s video", m.ext.c_str()).c_str());
            if (m.length_ms > 0)
                text(f_reg, 21, col_len, y + 13, C_DIM, format_time(m.length_ms).c_str(), 1);
        }
        dl->PopClipRect();
        if (n == 0) {
            text(f_semi, 26, 960, 470, C_TEXT, "This playlist is empty", 0.5f);
            text(f_reg, 21, 960, 512, C_DIM, "Open the Options menu on a video or song and choose Add to playlist", 0.5f);
        }
        status_text = sum;
        return;
    }
    /* Rows with a picture the same 16:9 size for every file (160x90):
     * a video fills it, a song's square cover sits in the middle of it. */
    const float row_h = 114, pic_w = 160, pic_h = 90;
    pl_item_scroll = approach(pl_item_scroll, std::max(0.0f, pl_item_focus * row_h - 330), 12);
    text(f_bold, 30, 96, 128, C_TEXT, pl->name.c_str(), 0, 1300);
    text(f_reg, 20, 96, 168, C_FAINT, sum.c_str());
    dl->PushClipRect(P(0, 206), P(1920, 1080), true);
    for (int i = 0; i < n; i++) {
        float y = 216 + i * row_h - pl_item_scroll;
        if (y > 1080 || y + row_h < 150)
            continue;
        MediaItem &m = L()[pl_items[i]];
        float f = focus_anim(hash_id(m.path, 240), i == pl_item_focus && modal == NONE);
        list_row(96, y, 1728, row_h - 12, f, i % 2 == 0);
        float cy = y + (row_h - 12) / 2;
        text(f_semi, 22, 136, cy - 14, mix(C_FAINT, C_ORANGE, f), std::to_string(i + 1).c_str(), 0.5f);
        float px = 172, py = cy - pic_h / 2;
        float ta = m.thumb_aspect > 0 ? m.thumb_aspect : (m.audio ? 1.0f : 16.0f / 9);
        if (m.thumb && (ta < 1.6f || ta > 2.0f)) {
            /* not 16:9 (a song's square cover, 4:3, a phone's tall video):
             * the whole picture, in the middle of a dark frame */
            float fw = std::min(pic_w, pic_h * ta), fh = fw / ta;
            rect(px, py, pic_w, pic_h, IM_COL32(255, 255, 255, 10), 10);
            thumb_cover(m, px + (pic_w - fw) / 2, py + (pic_h - fh) / 2, fw, fh, 8, ImDrawFlags_RoundCornersAll);
        } else if (m.thumb) {
            thumb_cover(m, px, py, pic_w, pic_h, 10, ImDrawFlags_RoundCornersAll); /* 16:9, near enough: fills it */
        } else {
            dl->AddRectFilledMultiColor(P(px, py), P(px + pic_w, py + pic_h), tint_for(m.name, 0.30f),
                                        tint_for(m.name + "#", 0.22f), tint_for(m.name, 0.12f),
                                        tint_for(m.name + "#", 0.16f));
            m.audio ? icon_note(px + pic_w / 2, cy, 34, IM_COL32(255, 255, 255, 170))
                    : icon_film(px + pic_w / 2, cy, 38, IM_COL32(255, 255, 255, 150));
        }
        if (f > 0.01f)
            stroke(px - 2, py - 2, pic_w + 4, pic_h + 4, alpha(IM_COL32(255, 255, 255, 255), f), 12, 2);
        float tx = px + pic_w + 28;
        text(f_semi, 26, tx, cy - 32, mix(C_DIM, C_TEXT, 0.7f + 0.3f * f), m.name.c_str(), 0, 1300 - tx);
        std::string sub = !m.artist.empty() ? m.artist : m.ext + "  \xC2\xB7  " + m.folder.substr(m.folder.rfind('/') + 1);
        text(f_reg, 19, tx, cy + 4, C_FAINT, sub.c_str(), 0, 1300 - tx);
        if (m.length_ms > 0)
            text(f_semi, 22, 1792, cy - 14, C_DIM, format_time(m.length_ms).c_str(), 1);
    }
    dl->PopClipRect();
    if (n == 0) {
        icon_list(960, 470, 70, C_FAINT);
        text(f_semi, 28, 960, 540, C_TEXT, "This playlist is empty", 0.5f);
        text(f_reg, 21, 960, 584, C_DIM, "Open the Options menu on a video or song and choose Add to playlist", 0.5f);
    }
}

/* The hint bar's buttons on the Playlists tab. */
std::vector<std::pair<uint32_t, const char *>> playlist_hints()
{
    auto &pls = library_playlists();
    if (pl_open.empty()) {
        if (pl_focus == 0)
            return { { PAD_CROSS, "Create" } };
        std::vector<std::pair<uint32_t, const char *>> h = { { PAD_CROSS, "Open" }, { PAD_TRIANGLE, "Play" } };
        if (pl_focus - 1 < (int)pls.size() && library_playlist_ours(pls[pl_focus - 1].path))
            h.push_back({ PAD_SQUARE, "Delete" });
        return h;
    }
    std::vector<std::pair<uint32_t, const char *>> h;
    if (!pl_items.empty()) {
        h = { { PAD_CROSS, "Play" }, { PAD_TRIANGLE, "Play all" } };
        if (library_playlist_ours(pl_open)) {
            h.push_back({ PAD_SQUARE, "Remove" });
            h.push_back({ PAD_R2, "Move" });
        }
    }
    h.push_back({ PAD_CIRCLE, "Back" });
    return h;
}

void start_stream(const std::string &url, const std::vector<std::string> &options = {}, bool remember = true,
                  const std::string &name = "", libvlc_media_t *media = nullptr);
void open_add_share();

void spinner(float cx, float cy, float r);

void icon_server(float cx, float cy, float s, ImU32 col)
{
    for (int i = 0; i < 2; i++) {
        float y = cy - s * 0.42f + i * s * 0.46f;
        stroke(cx - s * 0.5f, y, s, s * 0.38f, col, s * 0.08f, 2.5f);
        circle(cx + s * 0.3f, y + s * 0.19f, s * 0.06f, col);
        line(cx - s * 0.36f, y + s * 0.19f, cx, y + s * 0.19f, col, 2.5f);
    }
}

void icon_tv(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.5f, cy - s * 0.36f, s, s * 0.62f, col, s * 0.08f, 2.5f);
    line(cx - s * 0.2f, cy + s * 0.42f, cx + s * 0.2f, cy + s * 0.42f, col, 2.5f);
    line(cx - s * 0.16f, cy - s * 0.5f, cx, cy - s * 0.38f, col, 2.5f);
    line(cx + s * 0.16f, cy - s * 0.5f, cx, cy - s * 0.38f, col, 2.5f);
}

void icon_radio(float cx, float cy, float s, ImU32 col)
{
    circle(cx, cy, s * 0.1f, col);
    for (int i = 1; i <= 2; i++) {
        float r = s * (0.2f + 0.16f * i);
        dl->PathArcTo(P(cx, cy), r * S, -0.75f, 0.75f, 12);
        dl->PathStroke(col, 0, 2.5f * S);
        dl->PathArcTo(P(cx, cy), r * S, (float)M_PI - 0.75f, (float)M_PI + 0.75f, 12);
        dl->PathStroke(col, 0, 2.5f * S);
    }
}

void icon_plus_circle(float cx, float cy, float s, ImU32 col)
{
    dl->AddCircle(P(cx, cy), s * 0.5f * S, col, 32, 2.5f * S);
    line(cx - s * 0.24f, cy, cx + s * 0.24f, cy, col, 2.5f);
    line(cx, cy - s * 0.24f, cx, cy + s * 0.24f, col, 2.5f);
}

/* An archive opens in Browse like a folder: VLC lists what's inside. */
void net_enter(const std::string &url, const std::string &name);

void open_archive(const MediaItem &m)
{
    tab = BROWSE;
    std::string ext = m.ext;
    for (char &c : ext)
        c = (char)tolower((unsigned char)c);
    net_enter(file_uri(m.path), m.name + "." + ext);
}

/* Opens a network place in Browse (a server, or a folder on it). */
void net_enter(const std::string &url, const std::string &name)
{
    net_path.push_back({ url, name });
    net_list(url);
    browse_focus = 0;
    browse_scroll = 0;
    rebuild_browse();
}

/* ---- picking a subtitle file for the video playing (Tracks › Add a
 * subtitle file): Browse, from anywhere it reaches, then back to the video */
std::string link_name(const std::string &url);
bool sub_pick;
bool sub_pick_resume;  /* it was playing: it plays on after */
std::string sub_pick_dir;
std::vector<std::pair<std::string, std::string>> sub_pick_net;

void start_sub_pick()
{
    pressed = 0; /* the ✕ that asked is used up: the player mustn't pause on it too */
    sub_pick = true;
    sub_pick_resume = !player_paused();
    if (sub_pick_resume)
        player_toggle_pause();
    fprintf(stderr, "ui: picking a subtitle file (%s)\n", sub_pick_resume ? "paused the video" : "the video was paused");
    /* what Browse showed before comes back after */
    sub_pick_dir = browse_dir;
    sub_pick_net = net_path;
    modal = NONE;
    screen = LIBRARY;
    screen_fade = 0;
    tab = BROWSE;
    library_set_busy(false);
    /* It starts at the top (Sources): the media folder, drives and shares
     * alike, whatever the video came from; a subtitle can be anywhere. */
    net_path.clear();
    browse_dir.clear();
    net_list_stop();
    rebuild_browse();
    browse_focus = 0;
    browse_scroll = 0;
}

void end_sub_pick(std::string picked) /* a copy: it may be a Browse entry, and Browse is rebuilt below */
{
    pressed = 0;
    sub_pick = false;
    net_path = sub_pick_net;
    browse_dir = sub_pick_dir;
    if (!net_path.empty())
        net_list(net_path.back().first);
    else
        net_list_stop();
    rebuild_browse();
    browse_focus = 0;
    screen = PLAYER;
    screen_fade = 0;
    library_set_busy(true);
    osd_until = now + 3.5;
    /* playing again first: a subtitle added to a paused file left VLC paused
     * after the resume */
    if (sub_pick_resume && player_paused())
        player_toggle_pause();
    if (!picked.empty()) {
        std::string name = picked.find("://") != std::string::npos ? link_name(picked)
                                                                   : picked.substr(picked.rfind('/') + 1);
        if (player_add_subtitle(picked))
            toast(trf("Subtitles: %s", name.c_str()), 2.5);
        else
            toast("Couldn't load that subtitle file", 2.5);
    }
    fprintf(stderr, "ui: subtitle pick done (%s)\n", picked.empty() ? "none" : "picked");
}

/* ✕ on a file while picking: a subtitle file goes in, anything else says so. */
void sub_pick_file(std::string path, std::string name) /* copies, as above */
{
    if (player_is_subtitle_name(name))
        end_sub_pick(path);
    else
        toast("That isn't a subtitle file (.srt, .ass, .vtt...)", 2.5);
}

/* "1.4 GB" */
std::string size_text(int64_t bytes)
{
    char out[32];
    if (bytes >= (int64_t)1 << 30)
        snprintf(out, sizeof(out), "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1 << 20)
        snprintf(out, sizeof(out), "%.0f MB", bytes / 1048576.0);
    else
        snprintf(out, sizeof(out), "%.0f KB", std::max(1.0, bytes / 1024.0));
    return out;
}

/* What a Browse entry is called, and its kind, in the Classic list. */
void classic_label(const BrowseEntry &e, std::string *name, std::string *type, int *group)
{
    *group = 0;
    *name = e.item >= 0 ? L()[e.item].name : e.name;
    if (e.net == NET_SERVER) {
        *type = net_is_saved_server(e.path) ? "Network share (SMB)" : "Media server (DLNA)";
        *group = 1;
    } else if (e.net == NET_ADD) {
        *type = "";
        *group = 1;
    } else if (e.net == NET_ONLINE) {
        *type = e.path == "tv:" ? "Channel lists (iptv-org)" : "Stations (radio-browser.info)";
        *group = 2;
    } else if (e.net == NET_DIR) {
        *type = e.playlist ? "Playlist" : "File folder";
        if (e.playlist && e.name.rfind('.') != std::string::npos && e.name.rfind('.') > 0)
            *name = e.name.substr(0, e.name.rfind('.')); /* like local playlists */
    } else if (e.net == NET_FILE) {
        size_t dot = e.name.rfind('.');
        bool has_ext = dot != std::string::npos && dot > 0 && e.name.size() - dot <= 6;
        bool stream = e.path.compare(0, 4, "http") == 0;
        if (has_ext && !stream) {
            *name = e.name.substr(0, dot);
            std::string ext = e.name.substr(dot + 1);
            for (char &c : ext)
                c = (char)toupper((unsigned char)c);
            int k = name_kind(e.name);
            *type = k == 3 ? std::string(tr("Text document"))
                  : k == 5 ? trf("%s file", ext.c_str())
                           : trf(k == 4 ? "%s image" : k == 1 ? "%s audio" : "%s video", ext.c_str());
        } else {
            *type = "Live stream";
        }
    } else if (e.playlist) {
        *type = "Playlist";
    } else if (e.dir && browse_dir.empty()) {
        /* a drive or a media folder */
        const std::string &r = e.path;
        if (r == library_media_dir()) {
            *name = "Media";
            *type = "VLC's media folder";
        } else if (r.find("/usb") != std::string::npos) {
            *name = trf("USB drive (%s)", r.substr(r.rfind('/') + 1).c_str());
            *type = "Removable drive";
        } else if (r.find("/ext") != std::string::npos) {
            *name = trf("Extended storage (%s)", r.substr(r.rfind('/') + 1).c_str());
            *type = "Drive";
        } else {
            *name = r.substr(r.rfind('/') + 1);
            *type = r;
        }
    } else if (e.dir) {
        *type = "File folder";
    } else if (e.item >= 0 && L()[e.item].disc) {
        const MediaItem &m = L()[e.item];
        *type = m.ext == "ISO" || m.ext == "IMG" ? "Disc image (ISO)" : m.ext == "DVD" ? "DVD (folder)" : "Blu-ray (folder)";
    } else if (e.item >= 0 && L()[e.item].archive) {
        *type = trf("Compressed (%s)", L()[e.item].ext.c_str());
    } else if (e.item >= 0 && L()[e.item].other) {
        *type = L()[e.item].ext.empty() ? std::string(tr("File")) : trf("%s file", L()[e.item].ext.c_str());
    } else if (e.item >= 0) {
        const MediaItem &m = L()[e.item];
        *type = m.text ? (m.ext == "NFO" || m.ext == "DIZ" ? trf("Release info (%s)", m.ext.c_str()) : std::string(tr("Text document")))
                       : trf(m.image ? "%s image" : m.audio ? "%s audio" : "%s video", m.ext.c_str());
    }
}

void classic_icon(const BrowseEntry &e, float cx, float cy, float s)
{
    if (e.net == NET_SERVER)
        ci_server(cx, cy, s);
    else if (e.net == NET_ADD)
        ci_add(cx, cy, s);
    else if (e.net == NET_ONLINE)
        e.path == "tv:" ? ci_tv(cx, cy, s) : ci_radio(cx, cy, s);
    else if (e.net == NET_FILE)
        ci_file(cx, cy, s, name_kind(e.name));
    else if (e.dir && browse_dir.empty() && net_path.empty() && e.net == NET_NONE)
        e.path == library_media_dir() ? ci_folder(cx, cy, s) : ci_drive(cx, cy, s, e.path.find("/usb") != std::string::npos);
    else if (e.dir)
        ci_folder(cx, cy, s);
    else if (e.playlist)
        ci_file(cx, cy, s, 2);
    else if (L()[e.item].disc)
        ci_disc(cx, cy, s);
    else if (L()[e.item].archive)
        ci_zip(cx, cy, s);
    else if (L()[e.item].other)
        ci_file(cx, cy, s, 5);
    else
        ci_file(cx, cy, s, L()[e.item].text ? 3 : L()[e.item].image ? 4 : L()[e.item].audio ? 1 : 0);
}

void spinner(float cx, float cy, float r);

/* Classic Browse: an address bar with the path in segments, a back button and
 * a search box; then the list in Details view (Name, Type, Length, Size), the
 * places on the first level grouped as a desktop shows its drives. */
void browse_draw_classic()
{
    int n = (int)browse_entries.size();
    bool can_back = !browse_dir.empty() || !net_path.empty();
    /* ---- address bar */
    float ay = 122, ah = 48;
    rect(0, 108, 1920, 76, IM_COL32(32, 35, 40, 255));
    rect(0, 183, 1920, 1, C_SHADE);
    /* the round back button */
    circle(118, ay + ah / 2, 21, can_back ? IM_COL32(64, 70, 80, 255) : IM_COL32(40, 44, 50, 255));
    ring(118, ay + ah / 2, 21, IM_COL32(16, 17, 20, 255), 1.5f);
    ImU32 arrow = can_back ? C_ORANGE : IM_COL32(90, 95, 104, 255);
    tri(106, ay + ah / 2, 118, ay + ah / 2 - 10, 118, ay + ah / 2 + 10, arrow);
    rect(117, ay + ah / 2 - 3.5f, 13, 7, arrow);
    /* the path, in segments */
    std::vector<std::string> segs = { sub_pick ? "Pick a subtitle file" : "Sources" };
    /* an archive opened from a folder: the folder's path, then inside it */
    bool in_archive = !net_path.empty() && net_path[0].first.compare(0, 7, "file://") == 0;
    if (!net_path.empty() && !in_archive)
        segs.push_back("Network");
    for (const std::string &r : library_roots()) {
        if ((!net_path.empty() && !in_archive) || browse_dir.empty() || browse_dir.compare(0, r.size(), r) != 0)
            continue;
        BrowseEntry root_entry = { r, r, true, -1, false };
        std::string rn, rt;
        int g;
        std::string keep = browse_dir;
        browse_dir.clear();
        classic_label(root_entry, &rn, &rt, &g);
        browse_dir = keep;
        segs.push_back(rn);
        std::string rest = browse_dir.substr(r.size());
        size_t p;
        while (!rest.empty() && (p = rest.find('/', 1)) != std::string::npos) {
            segs.push_back(rest.substr(1, p - 1));
            rest = rest.substr(p);
        }
        if (rest.size() > 1)
            segs.push_back(rest.substr(1));
        break;
    }
    for (const auto &pl : net_path)
        segs.push_back(pl.second);
    float fx = 152, fw = 1340;
    inset(fx, ay, fw, ah);
    ci_computer(fx + 26, ay + ah / 2 + 2, 26);
    float sx = fx + 52;
    /* long paths keep their end in view */
    size_t first = 0;
    auto width_from = [&](size_t k) {
        float w = 0;
        for (size_t i = k; i < segs.size(); i++)
            w += text_size(f_reg, 23, segs[i].c_str()).x + 34;
        return w;
    };
    while (first + 1 < segs.size() && width_from(first) > fw - 80)
        first++;
    for (size_t i = first; i < segs.size(); i++) {
        bool last = i + 1 == segs.size();
        text(last ? f_semi : f_reg, 23, sx, ay + 11, last ? C_TEXT : C_DIM, segs[i].c_str(), 0, fw - 80);
        sx += text_size(last ? f_semi : f_reg, 23, segs[i].c_str()).x + 12;
        if (!last) {
            tri(sx + 1, ay + ah / 2 - 5, sx + 1, ay + ah / 2 + 5, sx + 8, ay + ah / 2, C_FAINT);
            sx += 22;
        }
    }
    /* the search box (R3 opens Search) */
    inset(1508, ay, 316, ah);
    ring(1530, ay + ah / 2 - 2, 8, C_FAINT, 2);
    line(1536, ay + ah / 2 + 4, 1542, ay + ah / 2 + 10, C_FAINT, 2.5f);
    text(f_reg, 21, 1552, ay + 12, C_FAINT, "Search");
    float kx = 1776;
    rect(kx, ay + 11, 36, 26, IM_COL32(40, 44, 50, 255), 4);
    text(f_semi, 15, kx + 18, ay + 15, C_DIM, "R3", 0.5f);

    /* ---- column headers */
    const float hy = 196, col_type = 1040, col_len = 1560, col_size = 1792;
    classic_columns(hy, { { "Name", 168, 0 }, { "Type", col_type, 0 }, { "Length", col_len, 1 }, { "Size", col_size, 1 } });

    /* ---- the rows; on the first level, groups with a heading */
    const float top = 240, bottom = 996, row_h = 48, head_h = 50;
    bool grouped = browse_dir.empty() && net_path.empty();
    std::vector<float> ys(n);
    std::vector<int> groups(n);
    float yy = 0;
    for (int i = 0; i < n; i++) {
        std::string nm, tp;
        classic_label(browse_entries[i], &nm, &tp, &groups[i]);
        if (grouped && (i == 0 || groups[i] != groups[i - 1]))
            yy += head_h + (i ? 10 : 0);
        ys[i] = yy;
        yy += row_h;
    }
    float view = bottom - top;
    float want = n ? ys[std::min(browse_focus, n - 1)] + row_h / 2 - view * 0.45f : 0;
    want = std::max(0.0f, std::min(want, std::max(0.0f, yy - view + 10)));
    browse_scroll = approach(browse_scroll, want, 14);
    dl->PushClipRect(P(0, top - 6), P(1920, bottom), true);
    static const char *const group_names[] = { "Folders and drives", "Network", "Internet" };
    for (int i = 0; i < n; i++) {
        const BrowseEntry &e = browse_entries[i];
        float y = top + ys[i] - browse_scroll;
        if (grouped && (i == 0 || groups[i] != groups[i - 1])) {
            float gy = y - head_h + 10;
            int count = 0;
            for (int k = i; k < n && groups[k] == groups[i]; k++)
                count++;
            char head[64];
            snprintf(head, sizeof(head), "%s (%d)", tr(group_names[groups[i]]), count);
            text(f_semi, 21, 112, gy + 6, IM_COL32(150, 186, 226, 255), head);
            float lx = 112 + text_size(f_semi, 21, head).x + 16;
            vgrad(lx, gy + 19, 1824 - lx, 1, IM_COL32(90, 110, 135, 255), IM_COL32(90, 110, 135, 255));
        }
        if (y > bottom || y + row_h < top - 60)
            continue;
        float f = focus_anim(hash_id(e.path + e.name, 300), i == browse_focus && modal == NONE, 16);
        sel_box(100, y + 2, 1720, row_h - 4, f);
        classic_icon(e, 138, y + row_h / 2, 32);
        std::string nm, tp;
        int g;
        classic_label(e, &nm, &tp, &g);
        text(f_reg, 23, 168, y + 11, C_TEXT, nm.c_str(), 0, col_type - 200);
        if (e.item >= 0 && L()[e.item].favourite)
            icon_star(168 + std::min(col_type - 200, text_size(f_reg, 23, nm.c_str()).x) + 18, y + row_h / 2, 18, C_ORANGE);
        text(f_reg, 21, col_type, y + 13, C_DIM, tp.c_str(), 0, col_len - 160 - col_type);
        if (e.item >= 0) {
            const MediaItem &m = L()[e.item];
            if (m.length_ms > 0)
                text(f_reg, 21, col_len, y + 13, C_DIM, format_time(m.length_ms).c_str(), 1);
            if (m.size > 0)
                text(f_reg, 21, col_size, y + 13, C_DIM, size_text(m.size).c_str(), 1);
        }
    }
    float mid = (top + bottom) / 2 - 60;
    if (!net_path.empty() && net_list_state() == NET_LOADING) {
        spinner(960, mid, 22);
        text(f_reg, 23, 960, mid + 44, C_DIM, trf("Connecting to %s\xE2\x80\xA6", tr(net_path.back().second.c_str())).c_str(), 0.5f, 1400);
    } else if (!net_path.empty() && net_list_state() == NET_FAILED) {
        text(f_semi, 25, 960, mid, C_TEXT, "Couldn't open this place", 0.5f);
        text(f_reg, 21, 960, mid + 42, C_DIM, vlc_message(net_list_error()).c_str(), 0.5f, 1500);
    } else if (!n) {
        text(f_reg, 23, 960, mid, C_DIM, "This folder is empty.", 0.5f);
    }
    dl->PopClipRect();

    /* what the status bar says */
    char st[64];
    snprintf(st, sizeof(st), tr(n == 1 ? "%d item" : "%d items"), n);
    status_text = st;
    if (browse_focus < n && browse_entries[browse_focus].item >= 0) {
        const MediaItem &m = L()[browse_entries[browse_focus].item];
        status_text += "      " + m.ext + (m.size > 0 ? ", " + size_text(m.size) : std::string()) +
                       (m.length_ms > 0 ? ", " + format_time(m.length_ms) : std::string());
    }
}

/* Music or radio going on behind the menus: a strip with the cover, the title,
 * where it is, and L3 back to it. The next song follows by itself. */
int current_item();
int neighbour_pos(int step);

void music_tick()
{
    if (!music_bg || now - music_started_at < 1.5)
        return;
    if (player_ended() || (!player_active() && !player_buffering())) {
        int cur = current_item();
        int next = neighbour_pos(1);
        if (cur >= 0 && next >= 0 && L()[cur].audio) {
            start_playback(playlist[next], 0, playlist, false, true);
            return;
        }
        music_bg = false;
        player_stop();
        playing_path.clear();
    }
}

void mini_player()
{
    if (!music_bg)
        return;
    MediaItem *m = library_find(playing_path);
    float x = 96, y = classic_look ? 940 : 948, w = 760, h = 56;
    if (classic_look) {
        vgrad(x, y, w, h, IM_COL32(56, 61, 70, 255), IM_COL32(36, 39, 45, 255));
        stroke(x, y, w, h, IM_COL32(10, 11, 13, 255), 4, 1);
        rect(x + 2, y + 1, w - 4, 1, IM_COL32(255, 255, 255, 30));
    } else {
        rect(x, y, w, h, IM_COL32(30, 30, 38, 240), 28);
    }
    if (m && m->thumb)
        dl->AddImage(m->thumb, P(x + 8, y + 6), P(x + 52, y + 50));
    else
        icon_note(x + 30, y + 28, 26, C_ORANGE);
    std::string title = m ? m->name : playing_name;
    std::string sub = m ? (m->artist.empty() ? m->folder.substr(m->folder.rfind('/') + 1) : m->artist) : "Live";
    text(f_semi, 21, x + 66, y + 6, C_TEXT, title.c_str(), 0, w - 240);
    text(f_reg, 17, x + 66, y + 31, C_DIM, sub.c_str(), 0, w - 240);
    int64_t len = player_length(), t = player_time();
    if (len > 0) {
        rect(x + 66, y + h - 5, w - 74, 3, IM_COL32(255, 255, 255, 30));
        rect(x + 66, y + h - 5, (w - 74) * std::min(1.0f, (float)t / len), 3, C_ORANGE);
    }
    if (player_paused())
        icon_pause(x + w - 150, y + 26, 18, C_DIM);
    else
        icon_play(x + w - 148, y + 26, 18, C_ORANGE);
    float kx = x + w - 96;
    rect(kx, y + 14, 38, 26, IM_COL32(255, 255, 255, 34), 6);
    text(f_bold, 15, kx + 19, y + 18, C_TEXT, "L3", 0.5f);
    text(f_reg, 17, kx + 46, y + 17, C_DIM, "Open");
}

void tab_browse()
{
    /* The network listing or the servers found changed: rebuild. */
    static int seen_serial = -1;
    static std::string seen_servers;
    if (!net_path.empty() && net_list_serial() != seen_serial) {
        seen_serial = net_list_serial();
        rebuild_browse();
        if (!net_focus_after.empty())
            for (int i = 0; i < (int)browse_entries.size(); i++)
                if (browse_entries[i].path == net_focus_after) {
                    browse_focus = i;
                    net_focus_after.clear();
                }
    }
    if (net_path.empty() && browse_dir.empty()) {
        std::string sig;
        for (const NetServer &sv : net_servers())
            sig += sv.url + "\n";
        if (sig != seen_servers) {
            /* The selection stays on what it was on; a server just added gets it. */
            std::string keep = browse_focus < (int)browse_entries.size() ? browse_entries[browse_focus].path : "";
            bool first = seen_servers.empty();
            std::string before = seen_servers;
            seen_servers = sig;
            rebuild_browse();
            int to = -1;
            for (int i = 0; i < (int)browse_entries.size(); i++) {
                const BrowseEntry &b = browse_entries[i];
                if (b.path == keep && to < 0)
                    to = i;
                if (!first && b.net == NET_SERVER && net_is_saved_server(b.path) &&
                    before.find(b.path + "\n") == std::string::npos)
                    to = i;
            }
            if (to >= 0)
                browse_focus = to;
        }
    }
    int n = (int)browse_entries.size();
    if (browse_focus >= n)
        browse_focus = n ? n - 1 : 0;
    if (hit(PAD_UP) && browse_focus > 0)
        browse_focus--;
    if (hit(PAD_DOWN) && browse_focus + 1 < n)
        browse_focus++;
    if ((hit(PAD_L2) || hit(PAD_R2)) && n) {
        std::vector<std::string> names;
        for (const BrowseEntry &b : browse_entries)
            names.push_back(b.item < 0 ? b.name : L()[b.item].name);
        int to = letter_jump(names, browse_focus, hit(PAD_R2) ? 1 : -1);
        if (to != browse_focus) {
            browse_focus = to;
            toast(std::string(1, letter_of(names[to])), 0.8);
        }
    }
    if (sub_pick && hit(PAD_CIRCLE) && net_path.empty() && browse_dir.empty()) {
        end_sub_pick(""); /* ○ at the top: back to the video, nothing picked */
        return;
    }
    if (sub_pick && hit(PAD_CROSS) && n && browse_entries[browse_focus].net == NET_FILE) {
        sub_pick_file(browse_entries[browse_focus].path, browse_entries[browse_focus].name);
        return;
    }
    if (hit(PAD_CROSS) && n && browse_entries[browse_focus].net != NET_NONE) {
        BrowseEntry e = browse_entries[browse_focus];
        if (e.net == NET_ADD) {
            open_add_share();
        } else if (e.net == NET_SERVER || e.net == NET_DIR || e.net == NET_ONLINE) {
            net_enter(e.path, e.name);
        } else {
            start_stream(e.path, net_login_options(e.path), false, e.name, net_take_media(e.path));
        }
        return;
    }
    if (hit(PAD_CIRCLE) && !net_path.empty()) {
        /* Back out: the selection stays on the place just left. */
        std::string left = net_path.back().first;
        net_path.pop_back();
        if (net_path.empty()) {
            net_list_stop();
            rebuild_browse();
            for (int i = 0; i < (int)browse_entries.size(); i++)
                if (browse_entries[i].path == left)
                    browse_focus = i;
        } else {
            net_focus_after = left;
            net_list(net_path.back().first);
            browse_focus = 0;
            rebuild_browse();
        }
        return;
    }
    if (hit(PAD_CROSS) && n) {
        BrowseEntry e = browse_entries[browse_focus];
        if (e.dir) {
            browse_dir = e.path;
            browse_focus = 0;
            rebuild_browse();
            return;
        }
        if (sub_pick) {
            std::string path = e.item >= 0 ? L()[e.item].path : e.path;
            sub_pick_file(path, path.substr(path.rfind('/') + 1));
            return;
        }
        if (e.playlist) {
            /* its entries, like a folder: pick a channel or a song */
            net_enter(file_uri(e.path), e.name);
            return;
        }
        std::vector<int> list;
        for (const BrowseEntry &b : browse_entries)
            if (b.item >= 0 && (!L()[b.item].other || b.item == e.item)) /* "next" skips other files */
                list.push_back(b.item);
        open_item(e.item, list);
    }
    if (hit(PAD_CIRCLE) && !browse_dir.empty()) {
        bool is_root = false;
        for (const std::string &r : library_roots())
            if (r == browse_dir)
                is_root = true;
        /* Back out of a folder: the selection stays on that folder. */
        std::string left = browse_dir;
        browse_dir = is_root ? "" : browse_dir.substr(0, browse_dir.rfind('/'));
        rebuild_browse();
        browse_focus = 0;
        for (int i = 0; i < (int)browse_entries.size(); i++)
            if (browse_entries[i].path == left)
                browse_focus = i;
        return;
    }
    if (classic_look) {
        browse_draw_classic();
        return;
    }
    /* Breadcrumb: "Media › Movies", not the raw path. */
    std::string crumb = tr(sub_pick ? "Pick a subtitle file" : "Sources");
    bool in_archive = !net_path.empty() && net_path[0].first.compare(0, 7, "file://") == 0;
    if (!net_path.empty() && !in_archive)
        crumb = tr("Network");
    for (const std::string &r : library_roots()) {
        if ((!net_path.empty() && !in_archive) || browse_dir.compare(0, r.size(), r) != 0)
            continue;
        crumb = r == library_media_dir() ? std::string(tr("Media")) : r.substr(r.rfind('/') + 1);
        std::string rest = browse_dir.substr(r.size());
        size_t p;
        while (!rest.empty() && (p = rest.find('/', 1)) != std::string::npos) {
            crumb += "  \xE2\x80\xBA  " + rest.substr(1, p - 1);
            rest = rest.substr(p);
        }
        if (rest.size() > 1)
            crumb += "  \xE2\x80\xBA  " + rest.substr(1);
        break;
    }
    for (const auto &pl : net_path)
        crumb += "  \xE2\x80\xBA  " + pl.second;
    text(f_bold, 30, 96, 140, C_TEXT, crumb.c_str(), 0, 1700);
    const float row_h = 76;
    browse_scroll = approach(browse_scroll, std::max(0.0f, browse_focus * row_h - 300), 12);
    dl->PushClipRect(P(0, 196), P(1920, 1080), true);
    for (int i = 0; i < n; i++) {
        const BrowseEntry &e = browse_entries[i];
        float y = 206 + i * row_h - browse_scroll;
        if (y > 1080 || y + row_h < 150)
            continue;
        float f = focus_anim(hash_id(e.path + e.name, 300), i == browse_focus && modal == NONE);
        list_row(96, y, 1728, row_h - 10, f, i % 2 == 0);
        if (e.net == NET_SERVER)
            icon_server(146, y + 33, 32, mix(C_DIM, C_ORANGE, f));
        else if (e.net == NET_ONLINE && e.path == "tv:")
            icon_tv(146, y + 33, 34, mix(C_DIM, C_ORANGE, f));
        else if (e.net == NET_ONLINE)
            icon_radio(146, y + 33, 34, mix(C_DIM, C_ORANGE, f));
        else if (e.net == NET_ADD)
            icon_plus_circle(146, y + 33, 32, mix(C_DIM, C_ORANGE, f));
        else if (e.net == NET_FILE) {
            int k = name_kind(e.name);
            if (k == 1)
                icon_note(146, y + 33, 28, C_DIM);
            else if (k == 4)
                icon_picture(146, y + 33, 34, C_DIM);
            else if (k == 3 || k == 5)
                icon_list(146, y + 33, 30, k == 5 ? C_FAINT : C_DIM);
            else
                icon_film(146, y + 33, 34, C_DIM);
        }
        else if (e.dir)
            icon_folder(146, y + 33, 34, mix(C_DIM, C_ORANGE, f));
        else if (e.playlist)
            icon_list(146, y + 33, 30, mix(C_DIM, C_ORANGE, f));
        else if (L()[e.item].audio)
            icon_note(146, y + 33, 28, C_DIM);
        else if (L()[e.item].image)
            icon_picture(146, y + 33, 34, C_DIM);
        else if (L()[e.item].text)
            icon_list(146, y + 33, 30, C_DIM);
        else if (L()[e.item].archive)
            icon_folder(146, y + 33, 34, C_DIM);
        else if (L()[e.item].other)
            icon_list(146, y + 33, 30, C_FAINT);
        else
            icon_film(146, y + 33, 34, C_DIM);
        /* A file on a share: its name, the format in a badge (like local files). */
        std::string net_name, net_ext;
        if (e.net == NET_FILE && e.name.rfind('.') != std::string::npos && e.name.rfind('.') > 0) {
            net_name = e.name.substr(0, e.name.rfind('.'));
            net_ext = e.name.substr(e.name.rfind('.') + 1);
            for (char &c : net_ext)
                c = (char)toupper((unsigned char)c);
        }
        const char *name = !net_name.empty() ? net_name.c_str() : e.item < 0 ? e.name.c_str() : L()[e.item].name.c_str();
        if (!net_ext.empty() && net_ext.size() <= 5) {
            float tw = text_size(f_bold, 16, net_ext.c_str()).x + 20;
            rect(1792 - tw, y + 20, tw, 28, IM_COL32(255, 255, 255, 22), 8);
            text(f_bold, 16, 1792 - tw / 2, y + 25, C_DIM, net_ext.c_str(), 0.5f);
        }
        text(f_semi, 25, 196, y + 18, mix(C_DIM, C_TEXT, 0.7f + 0.3f * f), name, 0, 1300);
        if (e.net == NET_SERVER || e.net == NET_ONLINE) {
            const char *kind = e.net == NET_ONLINE ? (e.path == "tv:" ? "iptv-org" : "radio-browser.info")
                               : net_is_saved_server(e.path) ? "SMB share" : "Media server";
            text(f_reg, 20, 1792, y + 22, C_FAINT, kind, 1);
        }
        if (e.item >= 0 && L()[e.item].favourite)
            icon_star(196 + std::min(1300.0f, text_size(f_semi, 25, name).x) + 24, y + 33, 22, C_ORANGE);
        if (e.item >= 0) {
            const MediaItem &m = L()[e.item];
            float right = 1792;
            if (m.length_ms > 0) {
                text(f_semi, 22, right, y + 20, C_DIM, format_time(m.length_ms).c_str(), 1);
                right -= text_size(f_semi, 22, format_time(m.length_ms).c_str()).x + 28;
            }
            float tw = text_size(f_bold, 16, m.ext.c_str()).x + 20;
            rect(right - tw, y + 20, tw, 28, IM_COL32(255, 255, 255, 22), 8);
            text(f_bold, 16, right - tw / 2, y + 25, C_DIM, m.ext.c_str(), 0.5f);
        }
    }
    if (!net_path.empty() && net_list_state() == NET_LOADING) {
        spinner(960, 420, 26);
        text(f_reg, 24, 960, 476, C_DIM, trf("Connecting to %s\xE2\x80\xA6", tr(net_path.back().second.c_str())).c_str(), 0.5f, 1400);
    } else if (!net_path.empty() && net_list_state() == NET_FAILED) {
        text(f_semi, 26, 960, 400, C_TEXT, "Couldn't open this place", 0.5f);
        text(f_reg, 22, 960, 446, C_DIM, vlc_message(net_list_error()).c_str(), 0.5f, 1500);
    } else if (!n) {
        text(f_reg, 24, 960, 400, C_DIM, net_path.empty() ? "Nothing here" : "This folder is empty", 0.5f);
    }
    dl->PopClipRect();
}

/* ---- modals ---------------------------------------------------------------------------- */

void spinner(float cx, float cy, float r);

void vgrad(float x, float y, float w, float h, ImU32 top, ImU32 bottom);

/* Behind a full-screen page (Search, links, sign in). */
void bevel(float x, float y, float w, float h, ImU32 top, ImU32 bottom);
void cone(float x, float y, float s, float a);

void screen_backdrop(float a)
{
    if (classic_look) {
        /* the window: body, a title band (the page's title sits in it) and a
         * status bar under the hints */
        vgrad(0, 0, 1920, 1080, alpha(IM_COL32(29, 31, 36, 255), a), alpha(IM_COL32(18, 19, 23, 255), a));
        bevel(0, 0, 1920, 108, alpha(IM_COL32(58, 63, 72, 255), a), alpha(IM_COL32(30, 33, 38, 255), a));
        cone(96, 31, 46, a);
        bevel(0, 1004, 1920, 76, alpha(IM_COL32(44, 48, 55, 255), a), alpha(IM_COL32(26, 28, 33, 255), a));
        rect(0, 1004, 1920, 1, alpha(IM_COL32(70, 75, 84, 255), a));
    } else
        rect(0, 0, 1920, 1080, alpha(IM_COL32(6, 6, 9, 255), a));
}

/* Classic: how tall the title band of the next panel is (the dialog's title
 * sits in it). */
float panel_band = 92;

void panel(float x, float y, float w, float h, float a)
{
    if (classic_look) {
        /* A dialog window: graphite body, a title band, a dark outline, a light inner edge. */
        rect(x + 4, y + 6, w, h, alpha(IM_COL32(0, 0, 0, 120), a), 4);
        vgrad(x, y, w, h, alpha(IM_COL32(40, 43, 49, 255), a), alpha(IM_COL32(26, 28, 33, 255), a));
        vgrad(x, y, w, panel_band, alpha(IM_COL32(62, 67, 76, 255), a), alpha(IM_COL32(44, 48, 55, 255), a));
        rect(x, y + panel_band, w, 1, alpha(IM_COL32(10, 11, 13, 255), a));
        rect(x, y + panel_band + 1, w, 1, alpha(IM_COL32(255, 255, 255, 14), a));
        panel_band = 92;
        stroke(x, y, w, h, alpha(IM_COL32(8, 9, 11, 255), a), 4, 1.5f);
        stroke(x + 1.5f, y + 1.5f, w - 3, h - 3, alpha(IM_COL32(255, 255, 255, 24), a), 3, 1);
        return;
    }
    glow(x, y + 16, w, h, 24, alpha(IM_COL32(0, 0, 0, 255), a), 40);
    rect(x, y, w, h, alpha(IM_COL32(24, 24, 31, 250), a), 24);
    stroke(x, y, w, h, alpha(IM_COL32(255, 255, 255, 22), a), 24, 1.5f);
}

void dim_screen(float a)
{
    rect(0, 0, 1920, 1080, alpha(IM_COL32(0, 0, 0, 160), a));
}

/* A centred menu of buttons; returns the chosen index on ✕, -2 on ○, -1 otherwise. */
int menu(const char *title, const std::vector<std::string> &labels, float w = 620)
{
    float a = ease(modal_anim);
    dim_screen(a);
    int n = (int)labels.size();
    float h = 120 + n * 72;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 32, x + 44, y + 36, alpha(C_TEXT, a), title);
    /* round the list: up from the first is the last, down from the last the first */
    if (hit(PAD_UP) && n > 0)
        modal_focus = (modal_focus + n - 1) % n;
    if (hit(PAD_DOWN) && n > 0)
        modal_focus = (modal_focus + 1) % n;
    for (int i = 0; i < n; i++) {
        float ry = y + 100 + i * 72;
        float f = focus_anim(hash_id(labels[i], 400 + (uint64_t)modal), i == modal_focus);
        list_row(x + 24, ry, w - 48, 62, f * a, false);
        text(f_semi, 25, x + 56, ry + 16, alpha(mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), a),
             labels[i].c_str());
    }
    if (hit(PAD_CROSS))
        return modal_focus;
    if (hit(PAD_CIRCLE) || hit(PAD_OPTIONS))
        return -2;
    return -1;
}

/* Preferred languages (ISO 639-2, what VLC matches tracks with). */
struct Language {
    const char *code, *name;
};
const Language languages[] = {
    { "", "Auto" }, { "off", "Off" }, { "eng", "English" }, { "fre", "French" },
    { "spa", "Spanish" }, { "ger", "German" }, { "ita", "Italian" }, { "por", "Portuguese" },
    { "ara", "Arabic" }, { "rus", "Russian" }, { "jpn", "Japanese" }, { "kor", "Korean" },
    { "chi", "Chinese" }, { "tur", "Turkish" }, { "dut", "Dutch" }, { "pol", "Polish" },
};

std::string lang_name(const std::string &code, bool sub)
{
    for (const Language &l : languages)
        if (code == l.code)
            return !sub && code.empty() ? "Auto (the file's default)" : l.name;
    return code;
}

void icon_link(float cx, float cy, float s, ImU32 col);
void icon_search(float cx, float cy, float s, ImU32 col);

/* ---- the on-screen keyboard ---------------------------------------------------
 * Four rows of keys and a row of wide ones; the d-pad moves, ✕ types,
 * □ deletes, △ is a space, R2 is Done. Link mode trades the space bar for
 * "/", ":" and ".com". */
struct Keyboard {
    std::string text;
    int row, col;
    int layer;      /* 0 lower case, 1 upper case, 2 symbols */
    bool link;
    bool shift_once; /* upper case for one letter */
};
Keyboard kb;
const char *const kb_rows[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_" },
    { "!@#$%^&*()", "~`|\\/?<>{}", "[]=+-_:;'\"", ".,!?$%&*#@" },
};
enum { KB_SHIFT, KB_LAYER, KB_SPACE, KB_BACK, KB_DONE, KB_SLASH, KB_COLON, KB_COM };

std::vector<int> kb_bottom()
{
    if (kb.link)
        return { KB_SHIFT, KB_LAYER, KB_SLASH, KB_COLON, KB_COM, KB_BACK, KB_DONE };
    return { KB_SHIFT, KB_LAYER, KB_SPACE, KB_BACK, KB_DONE };
}

float kb_bottom_weight(int k)
{
    return k == KB_SPACE ? 4.0f : k == KB_DONE ? 2.0f : k == KB_COM ? 1.6f : 1.4f;
}

void kb_open(const std::string &text, bool link)
{
    kb = Keyboard{};
    kb.text = text;
    kb.link = link;
    kb.row = 1;
}

void kb_type(const std::string &t)
{
    if (kb.text.size() < 400)
        kb.text += t;
    if (kb.shift_once && kb.layer == 1) {
        kb.layer = 0;
        kb.shift_once = false;
    }
}

void kb_backspace()
{
    if (kb.text.empty())
        return;
    /* a whole UTF-8 character: its continuation bytes, then its first */
    while (!kb.text.empty() && ((unsigned char)kb.text.back() & 0xC0) == 0x80)
        kb.text.pop_back();
    if (!kb.text.empty())
        kb.text.pop_back();
}

/* Draws the keyboard at (x, y), w wide; handles the buttons when it has the
 * focus. Returns true on Done. *leave_up: ↑ was pressed on the top row. */
bool kb_frame(float x, float y, float w, bool focused, float a, bool *leave_up)
{
    const float gap = 10, kh = 62;
    float kw = (w - 9 * gap) / 10;
    std::vector<int> bottom = kb_bottom();
    float total_weight = 0;
    for (int k : bottom)
        total_weight += kb_bottom_weight(k);
    float unit = (w - (bottom.size() - 1) * gap) / total_weight;
    auto key_center = [&](int r, int c) {
        if (r < 4)
            return x + c * (kw + gap) + kw / 2;
        float cx = x;
        for (int i = 0; i < c; i++)
            cx += kb_bottom_weight(bottom[i]) * unit + gap;
        return cx + kb_bottom_weight(bottom[c]) * unit / 2;
    };
    bool done = false;
    if (leave_up)
        *leave_up = false;
    if (focused) {
        int cols = kb.row < 4 ? 10 : (int)bottom.size();
        if (hit(PAD_LEFT))
            kb.col = (kb.col + cols - 1) % cols;
        if (hit(PAD_RIGHT))
            kb.col = (kb.col + 1) % cols;
        if (hit(PAD_UP) || hit(PAD_DOWN)) {
            int to = kb.row + (hit(PAD_DOWN) ? 1 : -1);
            if (to < 0) {
                if (leave_up)
                    *leave_up = true;
            } else if (to <= 4) {
                /* the key whose middle is nearest */
                float cx = key_center(kb.row, kb.col);
                int n = to < 4 ? 10 : (int)bottom.size(), best = 0;
                for (int c = 1; c < n; c++)
                    if (fabsf(key_center(to, c) - cx) < fabsf(key_center(to, best) - cx))
                        best = c;
                kb.row = to;
                kb.col = best;
            }
        }
        if (hit(PAD_SQUARE))
            kb_backspace();
        if (hit(PAD_TRIANGLE))
            kb_type(" ");
        if (hit(PAD_R2))
            done = true;
        if (hit(PAD_CROSS)) {
            if (kb.row < 4) {
                kb_type(std::string(1, kb_rows[kb.layer][kb.row][kb.col]));
            } else {
                switch (bottom[kb.col]) {
                case KB_SHIFT:
                    kb.layer = kb.layer == 1 ? 0 : 1;
                    kb.shift_once = kb.layer == 1;
                    break;
                case KB_LAYER: kb.layer = kb.layer == 2 ? 0 : 2; break;
                case KB_SPACE: kb_type(" "); break;
                case KB_BACK: kb_backspace(); break;
                case KB_SLASH: kb_type("/"); break;
                case KB_COLON: kb_type(":"); break;
                case KB_COM: kb_type(".com"); break;
                case KB_DONE: done = true; break;
                }
            }
        }
    }
    for (int r = 0; r < 5; r++) {
        int n = r < 4 ? 10 : (int)bottom.size();
        float kx = x;
        for (int c = 0; c < n; c++) {
            float wdt = r < 4 ? kw : kb_bottom_weight(bottom[c]) * unit;
            float ky = y + r * (kh + gap);
            bool foc = focused && kb.row == r && kb.col == c;
            float f = focus_anim(hash_id(std::to_string(r * 16 + c), 960), foc, 18);
            bool special = r == 4;
            ImU32 tc;
            if (classic_look) {
                vgrad(kx, ky, wdt, kh, alpha(special ? IM_COL32(66, 71, 80, 255) : IM_COL32(58, 62, 70, 255), a),
                      alpha(special ? IM_COL32(40, 43, 49, 255) : IM_COL32(34, 37, 42, 255), a));
                stroke(kx, ky, wdt, kh, alpha(IM_COL32(12, 13, 15, 255), a), 3, 1);
                rect(kx + 2, ky + 1, wdt - 4, 1, alpha(IM_COL32(255, 255, 255, 30), a));
                sel_box(kx, ky, wdt, kh, f * a);
                tc = alpha(C_TEXT, a);
            } else {
                ImU32 fill = mix(special ? IM_COL32(48, 48, 60, 255) : IM_COL32(36, 36, 46, 255), C_ORANGE, f);
                rect(kx, ky, wdt, kh, alpha(fill, a), 12);
                tc = alpha(f > 0.5f ? IM_COL32(255, 255, 255, 255) : C_TEXT, a);
            }
            float cx = kx + wdt / 2, cy = ky + kh / 2;
            if (r < 4) {
                char ch[2] = { kb_rows[kb.layer][r][c], 0 };
                text(f_semi, 28, cx, ky + 14, tc, ch, 0.5f);
            } else {
                const char *label = "";
                switch (bottom[c]) {
                case KB_SHIFT: label = kb.layer == 1 ? "abc" : "ABC"; break;
                case KB_LAYER: label = kb.layer == 2 ? "abc" : "#+="; break;
                case KB_SPACE: label = "space"; break;
                case KB_BACK: label = "\xE2\x8C\xAB"; break;
                case KB_SLASH: label = "/"; break;
                case KB_COLON: label = ":"; break;
                case KB_COM: label = ".com"; break;
                case KB_DONE: label = "Done"; break;
                }
                if (bottom[c] == KB_BACK) {
                    /* ⌫ drawn: a tag pointing left with an x */
                    float s2 = 20;
                    dl->PathLineTo(P(cx - s2, cy));
                    dl->PathLineTo(P(cx - s2 * 0.4f, cy - s2 * 0.6f));
                    dl->PathLineTo(P(cx + s2, cy - s2 * 0.6f));
                    dl->PathLineTo(P(cx + s2, cy + s2 * 0.6f));
                    dl->PathLineTo(P(cx - s2 * 0.4f, cy + s2 * 0.6f));
                    dl->PathStroke(tc, ImDrawFlags_Closed, 2.5f * S);
                    line(cx - 2, cy - 6, cx + 10, cy + 6, tc, 2.5f);
                    line(cx - 2, cy + 6, cx + 10, cy - 6, tc, 2.5f);
                } else {
                    text(f_semi, 22, cx, ky + 18, tc, label, 0.5f);
                }
            }
            kx += wdt + gap;
        }
    }
    return done;
}

/* The text being typed, in its box, with a blinking caret. */
void text_box(float x, float y, float w, const char *placeholder, float a, bool focused)
{
    if (classic_look) {
        inset(x, y, w, 72);
        if (focused)
            stroke(x, y, w, 72, alpha(C_ORANGE, 0.8f * a), 3, 1.5f);
    } else {
        rect(x, y, w, 72, alpha(IM_COL32(30, 30, 40, 255), a), 16);
        stroke(x, y, w, 72, alpha(focused ? C_ORANGE : IM_COL32(255, 255, 255, 30), a), 16, 2);
    }
    const char *shown = kb.text.empty() ? tr(placeholder) : kb.text.c_str(); /* translated whole, then trimmed */
    /* Long text: its end stays in view. */
    std::string t = shown;
    while (text_size(f_semi, 28, t.c_str()).x > w - 70 && t.size() > 1)
        t.erase(0, 1);
    text(f_semi, 28, x + 24, y + 19, alpha(kb.text.empty() ? C_FAINT : C_TEXT, a), t.c_str());
    if (focused && fmod(now, 1.0) < 0.55 && !kb.text.empty()) {
        float cx = x + 24 + text_size(f_semi, 28, t.c_str()).x + 3;
        rect(cx, y + 18, 3, 36, alpha(C_ORANGE, a), 1);
    }
}

/* ---- Search and Open a network link -------------------------------------------- */

struct Found {
    int item;       /* library index, or -1 */
    int playlist;   /* playlist index, or -1 */
    std::string link;
};
std::vector<Found> found;
int found_focus;
bool in_list;       /* focus on the list (above the keyboard) */
std::string found_for = "\x01";

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

void update_search()
{
    if (found_for == kb.text)
        return;
    found_for = kb.text;
    found.clear();
    std::string q = lower_ascii(kb.text);
    while (!q.empty() && q.back() == ' ')
        q.pop_back();
    if (q.empty())
        return;
    auto &pls = library_playlists();
    for (int i = 0; i < (int)pls.size(); i++)
        if (lower_ascii(pls[i].name).find(q) != std::string::npos)
            found.push_back({ -1, i, "" });
    for (int i = 0; i < (int)L().size() && found.size() < 60; i++)
        if (lower_ascii(L()[i].name).find(q) != std::string::npos ||
            lower_ascii(L()[i].folder.substr(L()[i].folder.rfind('/') + 1)).find(q) != std::string::npos)
            found.push_back({ i, -1, "" });
    found_focus = 0;
}

/* Recent links, newest first: cache/links.txt. */
std::vector<std::string> recent_links()
{
    std::vector<std::string> out;
    if (FILE *f = fopen((std::string(plat_data_dir()) + "/cache/links.txt").c_str(), "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), f) && out.size() < 8) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            if (!l.empty())
                out.push_back(l);
        }
        fclose(f);
    }
    return out;
}

void remember_link(const std::string &url)
{
    std::vector<std::string> links = recent_links();
    links.erase(std::remove(links.begin(), links.end(), url), links.end());
    links.insert(links.begin(), url);
    if (FILE *f = fopen((std::string(plat_data_dir()) + "/cache/links.txt").c_str(), "w")) {
        for (size_t i = 0; i < links.size() && i < 8; i++)
            fprintf(f, "%s\n", links[i].c_str());
        fclose(f);
    }
}

/* What a link is called on screen: its file name, or its host. */
std::string link_name(const std::string &url)
{
    std::string u = url.substr(0, url.find('?'));
    while (!u.empty() && u.back() == '/')
        u.pop_back();
    size_t slash = u.rfind('/'), scheme = u.find("://");
    std::string last = slash != std::string::npos && slash > scheme + 2 ? u.substr(slash + 1) : u.substr(scheme + 3);
    std::string out;
    for (size_t i = 0; i < last.size(); i++) { /* %20 and friends */
        if (last[i] == '%' && i + 2 < last.size()) {
            out += (char)strtol(last.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += last[i];
        }
    }
    return out.empty() ? url : out;
}

void start_playback(int item, int64_t from_ms, const std::vector<int> &list, bool seamless, bool background);

/* The open stream's options, name and whether it's a recent link (Watch again). */
std::vector<std::string> stream_options;
bool stream_remember;
std::string stream_name;

void start_stream(const std::string &url, const std::vector<std::string> &options, bool remember,
                  const std::string &name, libvlc_media_t *media)
{
    if (is_playlist_url(url)) {
        /* A channel list (typed, or a file on a share): its entries in Browse. */
        if (remember)
            remember_link(url);
        screen = LIBRARY;
        tab = BROWSE;
        net_enter(url, name.empty() ? link_name(url) : name);
        return;
    }
    music_bg = false;
    std::vector<std::string> opts = options;
    std::string bare = url.substr(0, url.find('?'));
    if (bare.size() > 4 && (!strcasecmp(bare.c_str() + bare.size() - 4, ".iso") || !strcasecmp(bare.c_str() + bare.size() - 4, ".img")) && url.compare(0, 7, "file://") != 0 &&
        std::find(opts.begin(), opts.end(), ":demux=dvd,any") == opts.end())
        /* A disc image on a share or a link: VLC's DVD reader only tries it by
         * itself on fast-seeking sources (not SMB), so ask for it first; a
         * Blu-ray image falls through to the Blu-ray reader ("any"). */
        opts.push_back(":demux=dvd,any");
    stream_options = opts;
    stream_remember = remember;
    stream_name = name;
    playing_path = url;
    playing_name = name.empty() ? link_name(url) : name;
    playlist.clear();
    playlist_pos = -1;
    from_playlist = false;
    video_forget_picture();
    library_set_busy(true);
    bool opened;
    if (media) {
        for (const std::string &o : opts)
            libvlc_media_add_option(media, o.c_str());
        opened = player_open_media(media, url, 0, opts);
    } else {
        opened = player_open(url, 0, opts);
    }
    if (!opened) {
        toast("Couldn't open that link", 3);
        library_set_busy(false);
        return;
    }
    if (remember)
        remember_link(url);
    screen = PLAYER;
    screen_fade = 0;
    osd_until = now + 3.5;
    scrubbing = false;
    loop_a = loop_b = -1;
    ended = false;
    last_save_time = now;
    was_active = false;
}

void open_search()
{
    kb_open("", false);
    found_for = "\x01";
    in_list = false;
    modal = SEARCH;
}

void open_link()
{
    std::vector<std::string> links = recent_links();
    kb_open(links.empty() ? "http://" : "", true);
    in_list = false;
    found_focus = 0;
    modal = LINK;
}

void open_add_share()
{
    kb_open("", true);
    in_list = false;
    modal = ADD_SHARE;
}

/* "Add a network share": its address, typed. */
void modal_add_share()
{
    float a = ease(modal_anim);
    screen_backdrop(a);
    const float x = 260, w = 1400;
    text(f_bold, 36, x, classic_look ? 32 : 64, alpha(C_TEXT, a), "Add a network share");
    text_box(x, 128, w, "192.168.1.20  or  \\\\NAS\\Movies  or  smb://nas/Movies", a, true);
    static const char *const help[] = {
        "The address of a Windows PC, a Mac or a NAS on the same network, with the shared folder if you like.",
        "VLC asks for the login if the share needs one, and remembers it on this PS5.",
        "Media servers on the network (DLNA, like most NAS) show up in Browse by themselves.",
    };
    for (int i = 0; i < 3; i++) {
        circle(x + 10, 262 + i * 46, 5, alpha(C_ORANGE, a));
        text(f_reg, 22, x + 30, 248 + i * 46, alpha(C_DIM, a), help[i], 0, w - 40);
    }
    bool done = kb_frame(x, 640, w, true, a, nullptr);
    if (done) {
        std::string t = kb.text;
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        if (t.empty()) {
            toast("Type the address of the computer or NAS", 3);
        } else {
            std::string url = net_add_server(t);
            modal = NONE;
            tab = BROWSE;
            browse_dir.clear();
            net_path.clear();
            net_enter(url, url.substr(url.find("://") + 3));
            return;
        }
    }
    if (hit(PAD_CIRCLE)) {
        modal = NONE;
        return;
    }
    hints(1824, 1040, { { PAD_SQUARE, "Delete" }, { PAD_TRIANGLE, "Space" }, { PAD_R2, "Add" }, { PAD_CIRCLE, "Close" } }, a);
}

/* "Add to playlist": "New playlist" first, then the ones made in VLC. */
std::string trimmed(std::string s);

void open_playlist_pick(const std::string &file)
{
    pl_add_file = file;
    modal = PL_PICK;
    modal_focus = 0;
    modal_anim = 0;
}

void open_playlist_name(const std::string &rename)
{
    pl_rename = rename;
    const PlaylistFile *pl = rename.empty() ? nullptr : playlist_by_path(rename);
    kb_open(pl ? pl->name : "", false);
    in_list = false;
    modal = PL_NAME;
    modal_anim = 0;
}

void add_to_playlist(const std::string &playlist)
{
    const PlaylistFile *pl = playlist_by_path(playlist);
    std::string name = pl ? pl->name : "";
    if (library_playlist_add(playlist, pl_add_file))
        toast(trf("Added to %s", name.c_str()), 2);
    else
        toast(trf("Already in %s", name.c_str()), 2);
    if (playlist == pl_open)
        pl_reload();
}

void modal_pl_pick()
{
    std::vector<const PlaylistFile *> ours;
    for (const PlaylistFile &p : library_playlists())
        if (library_playlist_ours(p.path))
            ours.push_back(&p);
    int n = 1 + (int)ours.size();
    if (modal_focus >= n)
        modal_focus = n - 1;
    if (hit(PAD_UP) && n > 0)
        modal_focus = (modal_focus + n - 1) % n;
    if (hit(PAD_DOWN) && n > 0)
        modal_focus = (modal_focus + 1) % n;
    if (hit(PAD_CIRCLE)) {
        modal = NONE;
        return;
    }
    MediaItem *m = library_find(pl_add_file);
    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 640, row = 76;
    int shown = std::min(n, 7);
    float h = 150 + shown * row + 70;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 30, x + 40, y + 30, alpha(C_TEXT, a), "Add to playlist");
    if (m)
        text(f_reg, 21, x + 40, y + 74, alpha(C_DIM, a), m->name.c_str(), 0, w - 80);
    int first = std::max(0, std::min(modal_focus - shown / 2, n - shown));
    for (int k = 0; k < shown; k++) {
        int i = first + k;
        float ry = y + 120 + k * row;
        const char *label = i == 0 ? tr("New playlist") : ours[i - 1]->name.c_str();
        float f = focus_anim(hash_id(i == 0 ? "+new" : ours[i - 1]->path, 950), i == modal_focus, 14);
        if (classic_look) {
            sel_box(x + 20, ry, w - 40, row - 10, f * a);
        } else if (f > 0.01f) {
            glow(x + 20, ry, w - 40, row - 10, 16, alpha(C_ORANGE, 0.35f * f * a), 12, 6);
            rect(x + 20, ry, w - 40, row - 10, alpha(C_CARD_HI, f * a), 16);
        }
        ImU32 ic = alpha(f > 0.5f ? IM_COL32(255, 255, 255, 255) : C_DIM, a);
        if (!classic_look)
            rect(x + 36, ry + 9, 48, 48, alpha(mix(IM_COL32(255, 255, 255, 14), C_ORANGE, 0.85f * f), a), 13);
        if (i == 0)
            icon_plus_circle(x + 60, ry + 33, 28, ic);
        else
            icon_list(x + 60, ry + 33, 26, ic);
        text(f_semi, 24, x + 104, ry + 19, alpha(mix(C_DIM, C_TEXT, 0.55f + 0.45f * f), a), label, 0, w - 140);
    }
    hints(x + w - 40, y + h - 40, { { PAD_CROSS, "Add" }, { PAD_CIRCLE, "Cancel" } }, a);
    if (hit(PAD_CROSS)) {
        if (modal_focus == 0) {
            open_playlist_name("");
        } else {
            add_to_playlist(ours[modal_focus - 1]->path);
            modal = NONE;
        }
    }
}

/* A playlist's name, typed: a new one (and the file waiting to go in), or a
 * new name for one. */
void modal_pl_name()
{
    float a = ease(modal_anim);
    screen_backdrop(a);
    const float x = 260, w = 1400;
    text(f_bold, 36, x, classic_look ? 32 : 64, alpha(C_TEXT, a), pl_rename.empty() ? "New playlist" : "Rename playlist");
    text_box(x, 128, w, "Name the playlist", a, true);
    MediaItem *m = pl_add_file.empty() ? nullptr : library_find(pl_add_file);
    if (m)
        text(f_reg, 22, x, 248, alpha(C_DIM, a), trf("%s goes in first", m->name.c_str()).c_str(), 0, w);
    else
        text(f_reg, 22, x, 248, alpha(C_DIM, a), "Saved in the media folder's Playlists, as a file VLC on a computer opens too", 0, w);
    bool done = kb_frame(x, 640, w, true, a, nullptr);
    if (done) {
        std::string t = trimmed(kb.text);
        if (t.empty()) {
            toast("Type a name", 2.5);
        } else if (pl_rename.empty()) {
            std::string path = library_playlist_create(t);
            if (path.empty()) {
                toast("A playlist with this name is already there, or the disk can't be written", 3);
            } else {
                modal = NONE;
                if (!pl_add_file.empty()) {
                    add_to_playlist(path);
                } else {
                    toast(trf("Made %s", t.c_str()), 2);
                    tab = PLAYLISTS; /* and on it */
                    pl_open.clear();
                    for (int i = 0; i < (int)library_playlists().size(); i++)
                        if (library_playlists()[i].path == path)
                            pl_focus = i + 1;
                }
                rebuild_browse();
                return;
            }
        } else {
            std::string path = library_playlist_rename(pl_rename, t);
            if (path.empty()) {
                toast("A playlist with this name is already there, or the disk can't be written", 3);
            } else {
                if (pl_open == pl_rename)
                    pl_open = path;
                modal = NONE;
                rebuild_browse();
                return;
            }
        }
    }
    if (hit(PAD_CIRCLE)) {
        modal = NONE;
        return;
    }
    hints(1824, 1040, { { PAD_SQUARE, "Delete" }, { PAD_TRIANGLE, "Space" }, { PAD_R2, "Save" }, { PAD_CIRCLE, "Cancel" } }, a);
}

/* Text cut into lines that fit max_w; returns the height used. */
float text_wrapped(ImFont *f, float size, float x, float y, ImU32 col, const std::string &s_in, float max_w, int max_lines)
{
    /* Translated whole, then cut into lines between words; Chinese, Japanese
     * and Korean (no spaces) may break after any of their characters. */
    std::string s = tr(s_in.c_str());
    auto cjk = [](unsigned char c) { return (c >= 0xE3 && c <= 0xED) || c == 0xEF; };
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < s.size() && (int)lines.size() < max_lines) {
        if (s[i] == '\n') {
            lines.push_back(cur);
            cur.clear();
            i++;
            continue;
        }
        size_t j = i;
        if (cjk((unsigned char)s[i]))
            j = std::min(s.size(), i + 3);
        else
            while (j < s.size() && s[j] != ' ' && s[j] != '\n' && !cjk((unsigned char)s[j]))
                j++;
        std::string unit = s.substr(i, j - i), with = cur + unit;
        if (!cur.empty() && text_size(f, size, with.c_str()).x > max_w) {
            while (!cur.empty() && cur.back() == ' ')
                cur.pop_back();
            lines.push_back(cur);
            cur = unit;
        } else {
            cur = with;
        }
        i = j;
        while (i < s.size() && s[i] == ' ') {
            if (!cur.empty())
                cur += ' ';
            i++;
        }
    }
    if (!cur.empty() && (int)lines.size() < max_lines)
        lines.push_back(cur);
    for (size_t k = 0; k < lines.size(); k++)
        text(f, size, x, y + k * size * 1.4f, col, lines[k].c_str(), 0, max_w);
    return lines.size() * size * 1.4f;
}

/* VLC asks for a login (an SMB share): user name and password, typed. */
std::string login_user, login_pass;
int login_field;        /* 0 user, 1 password */
bool login_remember = true;
int login_serial;
Modal ask_back = NONE;  /* what was open when VLC asked */

void field_box(float x, float y, float w, const char *label, const std::string &value, bool secret, bool active, float a)
{
    text(f_semi, 20, x, y, alpha(C_DIM, a), label);
    if (classic_look) {
        inset(x, y + 32, w, 66);
        if (active)
            stroke(x, y + 32, w, 66, alpha(C_ORANGE, 0.8f * a), 3, 1.5f);
    } else {
        rect(x, y + 32, w, 66, alpha(IM_COL32(30, 30, 40, 255), a), 14);
        stroke(x, y + 32, w, 66, alpha(active ? C_ORANGE : IM_COL32(255, 255, 255, 30), a), 14, 2);
    }
    std::string shown = secret ? std::string() : value;
    if (secret)
        for (size_t i = 0; i < value.size(); i++)
            shown += "\xE2\x80\xA2";
    text(f_semi, 26, x + 22, y + 50, alpha(C_TEXT, a), shown.c_str(), 0, w - 60);
    if (active && fmod(now, 1.0) < 0.55) {
        float cx = x + 22 + std::min(w - 60, text_size(f_semi, 26, shown.c_str()).x) + 3;
        rect(cx, y + 48, 3, 34, alpha(C_ORANGE, a), 1);
    }
}

void modal_login()
{
    NetAsk ask = net_ask();
    if (!ask.serial || !ask.login) {
        modal = ask_back; /* VLC gave up asking */
        return;
    }
    if (login_serial != ask.serial) {
        login_serial = ask.serial;
        login_user = ask.user;
        login_pass.clear();
        login_field = login_user.empty() ? 0 : 1;
        kb_open(login_field == 0 ? login_user : login_pass, false);
    }
    float a = ease(modal_anim);
    screen_backdrop(a);
    const float x = 260, w = 1400;
    text(f_bold, 36, x, classic_look ? 32 : 56, alpha(C_TEXT, a), ask.title.empty() ? "Sign in" : vlc_message(ask.title).c_str(), 0, w);
    text_wrapped(f_reg, 22, x, 112, alpha(C_DIM, a), vlc_message(ask.text), w, 3);
    /* the field being typed in holds kb.text */
    (login_field == 0 ? login_user : login_pass) = kb.text;
    field_box(x, 210, (w - 40) / 2, "User name", login_user, false, login_field == 0, a);
    field_box(x + (w + 40) / 2, 210, (w - 40) / 2, "Password", login_pass, true, login_field == 1, a);
    /* Remember: L1 */
    float ry = 350;
    rect(x, ry, 30, 30, alpha(login_remember ? C_ORANGE : IM_COL32(255, 255, 255, 30), a), 8);
    if (login_remember)
        icon_check(x + 15, ry + 15, 20, alpha(IM_COL32(255, 255, 255, 255), a));
    text(f_reg, 22, x + 46, ry + 2, alpha(C_TEXT, a), "Remember this login on this PS5  (L1)");
    if (hit(PAD_L1))
        login_remember = !login_remember;
    bool done = kb_frame(x, 640, w, true, a, nullptr);
    if (done) {
        if (login_field == 0) {
            login_field = 1;
            kb_open(login_pass, false);
        } else {
            net_answer_login(login_user, login_pass, login_remember);
            modal = ask_back;
            return;
        }
    }
    if (hit(PAD_R1) || hit(PAD_L2) || hit(PAD_R3)) {
        login_field = 1 - login_field;
        kb_open(login_field == 0 ? login_user : login_pass, false);
    }
    if (hit(PAD_CIRCLE)) {
        net_answer_login("", "", false);
        modal = ask_back;
        return;
    }
    hints(1824, 1040, { { PAD_R1, login_field == 0 ? "Password" : "User name" }, { PAD_R2, login_field == 0 ? "Next" : "Sign in" },
                        { PAD_SQUARE, "Delete" }, { PAD_CIRCLE, "Cancel" } }, a);
}

/* VLC asks a question (an HTTPS site whose certificate isn't trusted). */
void modal_question()
{
    NetAsk ask = net_ask();
    if (!ask.serial || ask.login) {
        modal = ask_back;
        return;
    }
    std::vector<std::pair<int, std::string>> buttons;
    for (int i = 0; i < 2; i++)
        if (!ask.actions[i].empty())
            buttons.push_back({ i + 1, ask.actions[i] });
    buttons.push_back({ 0, ask.cancel.empty() ? "Cancel" : ask.cancel });
    int nb = (int)buttons.size();
    if (modal_focus >= nb)
        modal_focus = nb - 1;
    if (hit(PAD_LEFT) && modal_focus > 0)
        modal_focus--;
    if (hit(PAD_RIGHT) && modal_focus + 1 < nb)
        modal_focus++;
    float a = ease(modal_anim);
    dim_screen(a);
    float w = 1100, h = 440, x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 30, x + 48, y + 40, alpha(C_TEXT, a), vlc_message(ask.title).c_str(), 0, w - 96);
    text_wrapped(f_reg, 21, x + 48, y + 96, alpha(C_DIM, a), vlc_message(ask.text), w - 96, 6);
    float bw = (w - 96 - (nb - 1) * 24) / nb;
    for (int i = 0; i < nb; i++) {
        float bx = x + 48 + i * (bw + 24), by = y + h - 120;
        float f = focus_anim(530 + i, modal_focus == i);
        if (f > 0.01f)
            glow(bx, by, bw, 70, 35, alpha(C_ORANGE, f * a * 0.7f), 14, 6);
        if (classic_look)
            classic_button(bx, by, bw, 70, f, a, C_ORANGE);
        else
            rect(bx, by, bw, 70, alpha(mix(IM_COL32(255, 255, 255, 26), C_ORANGE, f), a), 35);
        text(f_bold, 24, bx + bw / 2, by + 20, alpha(C_TEXT, a), buttons[i].second.c_str(), 0.5f, bw - 30);
    }
    if (hit(PAD_CROSS)) {
        net_answer_question(buttons[modal_focus].first);
        modal = ask_back;
    } else if (hit(PAD_CIRCLE)) {
        net_answer_question(0);
        modal = ask_back;
    }
}

/* ---- Subtitle downloads (OpenSubtitles, with the user's own API key) ---------- */

std::string os_key, os_user, os_pass;
int os_field;           /* 0 key, 1 user name, 2 password */
Modal os_back = NONE;
bool os_checking;       /* a key / account check is running */
std::string os_status;
bool os_status_ok;

std::string &os_text(int f)
{
    return f == 0 ? os_key : f == 1 ? os_user : os_pass;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    while (!s.empty() && s[0] == ' ')
        s.erase(0, 1);
    return s;
}

void open_osub_setup(Modal back)
{
    os_key = pref_str("osub_key", "");
    os_user = pref_str("osub_user", "");
    os_pass = pref_str("osub_pass", "");
    os_field = 0;
    kb_open(os_key, false);
    os_back = back;
    if (!os_checking)
        os_status.clear();
    modal = OSUB_SETUP;
}

void osub_check()
{
    os_status.clear();
    if (pref_str("osub_key", "").empty()) {
        os_status = "Enter your API key first";
        os_status_ok = false;
        return;
    }
    osub_sign_in();
    os_checking = true;
}

/* A running check's answer: shown in the form, or as a note elsewhere. */
void osub_check_tick()
{
    if (!os_checking)
        return;
    OsubState s = osub_state();
    if (s != OSUB_DONE && s != OSUB_FAILED)
        return;
    os_checking = false;
    os_status = osub_message();
    os_status_ok = s == OSUB_DONE;
    if (modal != OSUB_SETUP)
        toast("OpenSubtitles: " + std::string(tr(os_status.c_str())), 4);
}

void modal_osub_setup()
{
    float a = ease(modal_anim);
    screen_backdrop(a);
    const float x = 260, w = 1400;
    text(f_bold, 36, x, classic_look ? 32 : 56, alpha(C_TEXT, a), "Subtitle downloads");
    text_wrapped(f_reg, 22, x, 108, alpha(C_DIM, a),
                 "VLC finds subtitles on OpenSubtitles.com with your own free API key: make an account there, then "
                 "Profile \xE2\x80\xBA API consumers \xE2\x80\xBA New consumer. A long key is easier to paste from the "
                 "phone page (Settings \xE2\x80\xBA System \xE2\x80\xBA Phone access). Signing in is optional: it gives more downloads a day.",
                 w, 3);
    /* the field being typed in holds kb.text */
    os_text(os_field) = kb.text;
    field_box(x, 230, w, "API key", os_key, false, os_field == 0, a);
    field_box(x, 360, (w - 40) / 2, "User name (optional)", os_user, false, os_field == 1, a);
    field_box(x + (w + 40) / 2, 360, (w - 40) / 2, "Password (optional)", os_pass, true, os_field == 2, a);
    osub_check_tick();
    if (os_checking) {
        spinner(x + 14, 516, 12);
        text(f_reg, 22, x + 40, 503, alpha(C_DIM, a), "Checking with OpenSubtitles\xE2\x80\xA6");
    } else if (!os_status.empty()) {
        if (os_status_ok)
            icon_check(x + 14, 516, 22, alpha(C_ORANGE, a));
        else
            circle(x + 14, 516, 6, alpha(IM_COL32(240, 90, 90, 255), a));
        text(f_reg, 22, x + 40, 503, alpha(os_status_ok ? C_TEXT : C_DIM, a), os_status.c_str(), 0, w - 40);
    }
    auto save = [] {
        pref_set("osub_key", trimmed(os_key));
        pref_set("osub_user", trimmed(os_user));
        pref_set("osub_pass", os_pass);
    };
    bool done = kb_frame(x, 640, w, true, a, nullptr);
    if (done) {
        if (os_field < 2) {
            os_field++;
            kb_open(os_text(os_field), false);
        } else {
            save();
            osub_check();
        }
    }
    if (hit(PAD_R1) || hit(PAD_L1)) {
        os_field = (os_field + (hit(PAD_R1) ? 1 : 2)) % 3;
        kb_open(os_text(os_field), false);
    }
    if (hit(PAD_CIRCLE)) {
        save();
        modal = os_back;
        return;
    }
    hints(1824, 1040, { { PAD_R1, "Next field" }, { PAD_R2, os_field < 2 ? "Next" : "Save and check" },
                        { PAD_SQUARE, "Delete" }, { PAD_CIRCLE, "Save and close" } }, a);
}

/* The results for the playing video. */
std::vector<SubResult> os_results;
bool os_searching, os_downloading;
bool os_typing;  /* typing what to search for */
std::string os_msg, os_name;
int os_lang;    /* into languages[] */

/* Our ISO 639-2 codes in OpenSubtitles' terms. */
const char *osub_lang(const char *code)
{
    static const char *const map[][2] = {
        { "eng", "en" }, { "fre", "fr" }, { "spa", "es" }, { "ger", "de" }, { "ita", "it" },
        { "por", "pt-br,pt-pt" }, { "ara", "ar" }, { "rus", "ru" }, { "jpn", "ja" }, { "kor", "ko" },
        { "chi", "zh-cn,zh-tw" }, { "tur", "tr" }, { "dut", "nl" }, { "pol", "pl" },
    };
    for (const auto &m : map)
        if (!strcmp(code, m[0]))
            return m[1];
    return "en";
}

bool osub_local()
{
    return !playing_path.empty() && playing_path.find("://") == std::string::npos;
}

void osub_start_search()
{
    os_results.clear();
    os_msg.clear();
    modal_focus = 0;
    os_searching = true;
    osub_search(osub_local() ? playing_path : "", os_name, osub_lang(languages[os_lang].code));
}

void open_osub_results()
{
    if (!osub_has_key()) {
        open_osub_setup(TRACKS);
        return;
    }
    if (os_checking || os_searching || os_downloading) {
        modal = OSUB_RESULTS;
        return;
    }
    /* the subtitle language of the settings, else English */
    int n = (int)(sizeof(languages) / sizeof(languages[0]));
    os_lang = 2;
    for (int k = 2; k < n; k++)
        if (player_sub_language() == languages[k].code)
            os_lang = k;
    /* what it's called: the file's name without its extension */
    os_name = playing_name;
    size_t dot = os_name.rfind('.');
    if (dot != std::string::npos && os_name.size() - dot <= 5)
        os_name.erase(dot);
    for (char &c : os_name)
        if (c == '.' || c == '_')
            c = ' ';
    os_typing = false;
    modal = OSUB_RESULTS;
    osub_start_search();
}

void osub_download_focused()
{
    if (modal_focus >= (int)os_results.size())
        return;
    std::string save_base;
    if (osub_local()) {
        save_base = playing_path;
        size_t dot = save_base.rfind('.'), slash = save_base.rfind('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            save_base.erase(dot);
    }
    /* the app's cache when the video's folder can't be written (or it's on the network) */
    std::string dir = std::string(plat_data_dir()) + "/cache/subtitles";
    mkdir(dir.c_str(), 0777);
    std::string safe;
    for (char c : os_name)
        safe += strchr("/\\:*?\"<>|", c) ? '_' : c;
    if (safe.empty())
        safe = "subtitles";
    os_downloading = true;
    osub_download(os_results[modal_focus], save_base, dir + "/" + safe);
}

void modal_osub_results()
{
    if (os_typing) {
        float a = ease(modal_anim);
        screen_backdrop(a);
        const float x = 260, w = 1400;
        text(f_bold, 36, x, classic_look ? 32 : 56, alpha(C_TEXT, a), "Search subtitles");
        text_box(x, 128, w, "Type the title of the film or series", a, true);
        text_wrapped(f_reg, 22, x, 240, alpha(C_DIM, a),
                     "A title and a year find the most, like: The Matrix 1999. For a series, add the episode: Friends S01E02.",
                     w, 3);
        if (kb_frame(x, 640, w, true, a, nullptr)) {
            std::string t = trimmed(kb.text);
            if (!t.empty()) {
                os_name = t;
                os_typing = false;
                osub_start_search();
            }
        }
        if (hit(PAD_CIRCLE))
            os_typing = false;
        hints(1824, 1040, { { PAD_SQUARE, "Delete" }, { PAD_TRIANGLE, "Space" }, { PAD_R2, "Search" },
                            { PAD_CIRCLE, "Back" } }, a);
        return;
    }
    int n = (int)os_results.size(), nl = (int)(sizeof(languages) / sizeof(languages[0]));
    bool busy = os_searching || os_downloading || os_checking;
    if (os_checking) {
        osub_check_tick();
    } else if (os_searching || os_downloading) {
        OsubState s = osub_state();
        if (s == OSUB_DONE || s == OSUB_FAILED) {
            os_msg = osub_message();
            if (os_searching) {
                os_searching = false;
                os_results = osub_results();
                n = (int)os_results.size();
            } else {
                os_downloading = false;
                std::string path = osub_saved_path();
                if (s == OSUB_DONE && !path.empty() && player_add_subtitle(path)) {
                    toast(os_msg.empty() ? std::string(tr("Subtitles downloaded"))
                                         : std::string(tr("Subtitles downloaded")) + "  \xC2\xB7  " + os_msg, 3);
                    modal = NONE;
                    return;
                }
                toast(os_msg.empty() ? "Couldn't load the subtitles" : os_msg, 4);
            }
        }
    }
    if (!busy) {
        /* round the list: up from the first is the last, down from the last the first */
        if (hit(PAD_UP) && n > 0)
            modal_focus = (modal_focus + n - 1) % n;
        if (hit(PAD_DOWN) && n > 0)
            modal_focus = (modal_focus + 1) % n;
        if (hit(PAD_LEFT) || hit(PAD_RIGHT)) {
            /* another language: Auto and Off aren't ones */
            int k = os_lang + (hit(PAD_RIGHT) ? 1 : -1);
            os_lang = k < 2 ? nl - 1 : k >= nl ? 2 : k;
            osub_start_search();
        }
        if (hit(PAD_CROSS) && n > 0)
            osub_download_focused();
        if (hit(PAD_SQUARE)) {
            open_osub_setup(OSUB_RESULTS);
            return;
        }
        if (hit(PAD_TRIANGLE)) {
            os_typing = true;
            kb_open(os_name, false);
            return;
        }
    }
    if (hit(PAD_CIRCLE)) {
        modal = TRACKS; /* a running job finishes in the background */
        return;
    }

    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 1240, h = 820;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel_band = 92;
    panel(x, y, w, h, a);
    float px = x + 48, pw = w - 96;
    icon_cc(px + 20, y + 46, 40, alpha(C_ORANGE, a));
    text(f_bold, 30, px + 56, y + 28, alpha(C_TEXT, a), "Download subtitles");
    /* the language: ←/→ */
    std::string lang = std::string("\xE2\x80\xB9  ") + languages[os_lang].name + "  \xE2\x80\xBA";
    text(f_semi, 24, px + pw, y + 32, alpha(C_TEXT, a), lang.c_str(), 1);
    text(f_reg, 21, px, y + 116, alpha(C_DIM, a), os_name.c_str(), 0, pw);
    rect(px, y + 160, pw, 1, alpha(IM_COL32(255, 255, 255, 24), a));

    const float list_top = y + 176, row_h = 72;
    const int shown = 8;
    if (os_searching || (os_checking && n == 0)) {
        spinner(960, list_top + 220, 22);
        text(f_reg, 22, 960, list_top + 262, alpha(C_DIM, a), "Searching OpenSubtitles\xE2\x80\xA6", 0.5f);
    } else if (n == 0) {
        text_wrapped(f_reg, 22, px, list_top + 20, alpha(C_DIM, a),
                     os_msg.empty() ? "No subtitles found" : os_msg, pw, 3);
        text(f_reg, 20, px, list_top + 120, alpha(C_FAINT, a), "\xE2\x86\x90 \xE2\x86\x92 another language");
    }
    int first = std::max(0, std::min(modal_focus - shown + 1, n - shown));
    for (int i = first; i < n && i < first + shown; i++) {
        const SubResult &r = os_results[i];
        float ry = list_top + (i - first) * row_h;
        bool foc = i == modal_focus;
        float f = focus_anim(hash_id("os" + std::to_string(i), 975), foc, 16);
        if (classic_look)
            sel_box(px - 12, ry, pw + 24, row_h - 8, f * a);
        else if (f > 0.01f)
            rect(px - 12, ry, pw + 24, row_h - 8, alpha(C_CARD_HI, f * a), 12);
        /* the language tag */
        std::string tag = r.language;
        for (char &c : tag)
            c = (char)toupper((unsigned char)c);
        stroke(px, ry + 14, 84, 34, alpha(IM_COL32(255, 255, 255, 60), a), 4, 1.5f);
        text(f_bold, 18, px + 42, ry + 20, alpha(C_DIM, a), tag.c_str(), 0.5f, 80);
        float tx = px + 104, right = 260;
        if (r.hash_match) {
            /* made for exactly this file */
            float bw = text_size(f_semi, 17, "Matches this file").x + 24;
            rect(px + pw - bw, ry + 16, bw, 30, alpha(C_ORANGE, 0.9f * a), 4);
            text(f_semi, 17, px + pw - bw / 2, ry + 21, alpha(IM_COL32(20, 10, 0, 255), a), "Matches this file", 0.5f);
            right = bw + 24;
        } else {
            std::string dl = trf("%d downloads", r.downloads);
            text(f_reg, 19, px + pw, ry + 20, alpha(C_FAINT, a), dl.c_str(), 1);
        }
        text(f_semi, 23, tx, ry + 18, alpha(mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), a), r.release.c_str(), 0, pw - (tx - px) - right);
    }
    if (os_downloading) {
        float bw = 420, bh = 120, bx = 960 - bw / 2, by = list_top + 200;
        panel(bx, by, bw, bh, a);
        spinner(bx + 60, by + bh / 2, 18);
        text(f_semi, 24, bx + 100, by + bh / 2 - 15, alpha(C_TEXT, a), "Downloading\xE2\x80\xA6");
    }
    if (busy)
        hints(1824, 1040, { { PAD_CIRCLE, "Back" } }, a);
    else if (n == 0)
        hints(1824, 1040, { { PAD_TRIANGLE, "Search by name" }, { PAD_LEFT, "Language" }, { PAD_SQUARE, "API key" },
                            { PAD_CIRCLE, "Back" } }, a);
    else
        hints(1824, 1040, { { PAD_CROSS, "Download" }, { PAD_TRIANGLE, "Search by name" }, { PAD_LEFT, "Language" },
                            { PAD_SQUARE, "API key" }, { PAD_CIRCLE, "Back" } }, a);
}

/* Search (titles and folders) or a network link: a list above, the keyboard
 * below. ↑ from the keyboard's top row goes into the list. */
void modal_text_screen()
{
    bool search = modal == SEARCH;
    std::vector<std::string> links = search ? std::vector<std::string>() : recent_links();
    if (search)
        update_search();
    int n = search ? (int)found.size() : (int)links.size();
    if (found_focus >= n)
        found_focus = n ? n - 1 : 0;
    float a = ease(modal_anim);
    screen_backdrop(a); /* its own screen */
    const float x = 260, w = 1400;
    text(f_bold, 36, x, classic_look ? 32 : 64, alpha(C_TEXT, a), search ? "Search" : "Open a network link");
    text_box(x, 128, w, search ? "Type a title or a folder" : "http://, udp://, rtp://, ftp:// ...", a, !in_list);

    /* The list: results, or recent links. */
    const float list_top = 230, row_h = 62;
    int shown = 6;
    if (in_list) {
        if (hit(PAD_UP) && found_focus > 0)
            found_focus--;
        if (hit(PAD_DOWN)) {
            if (found_focus + 1 < n)
                found_focus++;
            else
                in_list = false;
        }
        if (hit(PAD_CIRCLE)) {
            in_list = false;
            pressed &= ~PAD_CIRCLE;
        }
    }
    int first = std::max(0, std::min(found_focus - shown + 1, n - shown));
    if (n == 0) {
        const char *empty = search ? (kb.text.empty() ? "Results show up here as you type" : "Nothing found")
                                   : "Links you open show up here";
        text(f_reg, 22, x + 4, list_top + 18, alpha(C_FAINT, a), empty);
    }
    for (int i = first; i < n && i < first + shown; i++) {
        float ry = list_top + (i - first) * row_h;
        bool foc = in_list && i == found_focus;
        float f = focus_anim(hash_id((search ? "s" : "l") + std::to_string(i), 970), foc, 16);
        if (f > 0.01f)
            rect(x - 12, ry, w + 24, row_h - 8, alpha(mix(C_CARD_HI, C_CARD_HI, f), f * a), 12);
        if (foc)
            rect(x - 12, ry + 10, 5, row_h - 28, alpha(C_ORANGE, a), 3);
        std::string title, sub;
        if (search) {
            const Found &r = found[i];
            if (r.playlist >= 0) {
                icon_list(x + 22, ry + 26, 28, alpha(C_DIM, a));
                title = library_playlists()[r.playlist].name;
                sub = "Playlist";
            } else {
                const MediaItem &m = L()[r.item];
                if (m.audio)
                    icon_note(x + 22, ry + 26, 26, alpha(C_DIM, a));
                else
                    icon_film(x + 22, ry + 26, 30, alpha(C_DIM, a));
                title = m.name;
                sub = m.ext + "  \xC2\xB7  " + m.folder.substr(m.folder.rfind('/') + 1);
            }
        } else {
            icon_link(x + 22, ry + 26, 28, alpha(C_DIM, a));
            title = link_name(links[i]);
            sub = links[i];
        }
        text(f_semi, 24, x + 64, ry + 4, alpha(mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), a), title.c_str(), 0, w - 400);
        text(f_reg, 18, x + 64, ry + 32, alpha(C_FAINT, a), sub.c_str(), 0, w - 120);
    }

    bool leave_up = false;
    bool done = kb_frame(x, 640, w, !in_list, a, &leave_up);
    if (leave_up && n > 0) {
        in_list = true;
        found_focus = std::min(n - 1, first + shown - 1);
    }
    if (in_list && hit(PAD_CROSS) && n > 0) {
        if (search) {
            Found r = found[found_focus];
            modal = NONE;
            if (r.playlist >= 0) {
                play_playlist(library_playlists()[r.playlist].path);
            } else {
                std::vector<int> list;
                for (const Found &x2 : found)
                    if (x2.item >= 0 && (!L()[x2.item].other || x2.item == r.item))
                        list.push_back(x2.item);
                open_item(r.item, list);
            }
        } else {
            std::string url = links[found_focus];
            modal = NONE;
            start_stream(url);
        }
        return;
    }
    if (done) {
        if (search) {
            if (!found.empty()) {
                in_list = true;
                found_focus = 0;
            }
        } else if (kb.text.find("://") != std::string::npos && kb.text.size() > 8) {
            std::string url = kb.text;
            modal = NONE;
            start_stream(url);
            return;
        } else {
            toast("A link starts with http://, https://, udp:// or similar", 3);
        }
    }
    if (!in_list && hit(PAD_CIRCLE)) {
        modal = NONE;
        return;
    }
    if (in_list && !search && hit(PAD_SQUARE)) {
        /* □ on a recent link: forget it */
        links.erase(links.begin() + found_focus);
        if (FILE *f = fopen((std::string(plat_data_dir()) + "/cache/links.txt").c_str(), "w")) {
            for (const std::string &l : links)
                fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
    }
    if (in_list)
        hints(1824, 1040, search ? std::vector<std::pair<uint32_t, const char *>>{ { PAD_CROSS, "Play" }, { PAD_CIRCLE, "Keyboard" } }
                                 : std::vector<std::pair<uint32_t, const char *>>{ { PAD_CROSS, "Open" }, { PAD_SQUARE, "Forget" }, { PAD_CIRCLE, "Keyboard" } }, a);
    else
        hints(1824, 1040, { { PAD_SQUARE, "Delete" }, { PAD_TRIANGLE, "Space" }, { PAD_R2, search ? "Results" : "Open" }, { PAD_CIRCLE, "Close" } }, a);
}

/* What the info window returns to when closed (Settings opens About). */
Modal info_back = NONE;

bool info_about;    /* the info window is About */

void show_about()
{
    modal = INFO;
    info_about = true;
}

/* About: who made it and what it's built with, the logo on the right. */
void modal_about()
{
    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 1300, h = 700;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel_band = 92;
    panel(x, y, w, h, a);
    text(f_bold, 30, x + 48, y + 30, alpha(C_TEXT, a), "About VLC");
    /* the logo: the empty right side */
    float lx = x + w - 230, ly = y + 92 + (h - 92) / 2 - 20;
    logo(lx, ly, 260, a);

    float tx = x + 48, ty = y + 128, tw = w - 48 - 440;
    text(f_bold, 40, tx, ty, alpha(C_TEXT, a), "VLC for PS5");
    float vx = tx + text_size(f_bold, 40, "VLC for PS5").x + 18;
    text(f_semi, 24, vx, ty + 12, alpha(C_ORANGE, a), "v" VLC_PS5_VERSION);
    text(f_reg, 21, tx, ty + 58, alpha(C_DIM, a), "VLC media player 3.0.24 Vetinari (libVLC)");

    /* the credits */
    static const char *const credits[][2] = {
        { "PS5 port", "AZiZ" },
        { "VLC", "VideoLAN and the VLC contributors" },
    };
    float cy = ty + 124;
    for (const auto &c : credits) {
        text(f_reg, 22, tx, cy, alpha(C_DIM, a), c[0]);
        text(f_semi, 22, tx + 180, cy, alpha(C_TEXT, a), c[1], 0, tw - 180);
        cy += 40;
    }
    rect(tx, cy + 14, tw, 1, alpha(IM_COL32(255, 255, 255, 24), a));
    text(f_semi, 19, tx, cy + 34, alpha(C_DIM, a), "Built with");
    text_wrapped(f_reg, 18, tx, cy + 64, alpha(C_FAINT, a),
                 std::string("FFmpeg 8.1.2, dav1d 1.5.4, libmatroska 1.7.2, libebml 1.4.6, libdvbpsi 1.3.3, libass 0.17.5, "
                 "FreeType 2.13.3, HarfBuzz 10.4.0, FriBidi 1.0.16, GnuTLS 3.8.13, Nettle 3.10.2, GMP 6.3.0, "
                 "libsmb2 6.1, libupnp 1.14.31, libxml2 2.15.3, libdvdread 6.1.3, libdvdnav 6.1.1, libbluray 1.4.1, "
                 "libarchive 3.8.9, zlib 1.3.1, Mesa RADV (Vulkan), Dear ImGui 1.92.9, stb_image. ") +
                     tr("Fonts: Selawik, Inter, Noto, DejaVu (OFL). TV lists: iptv-org. Radio: radio-browser.info. Subtitles: OpenSubtitles.com."),
                 tw, 6);
    text(f_reg, 17, tx, y + h - 52, alpha(C_FAINT, a),
         "GPL-3.0. An unofficial port, not affiliated with VideoLAN or Sony.", 0, tw);
    hints(x + w - 40, y + h - 40, { { PAD_CIRCLE, "Close" } }, a);
    if (hit(PAD_CIRCLE) || hit(PAD_CROSS)) {
        modal = info_back;
        info_back = NONE;
        info_about = false;
    }
}

/* Options (the OPTIONS button): a few actions, each with its icon. The
 * choices live in Settings. */
enum { OI_SETTINGS, OI_RESCAN, OI_RESUME, OI_TRACKS, OI_STOP, OI_PHONE, OI_SEARCH, OI_LINK, OI_ADD_PL, OI_RENAME_PL,
       OI_DELETE_PL };

void icon_search(float cx, float cy, float s, ImU32 col)
{
    ring(cx - s * 0.08f, cy - s * 0.08f, s * 0.3f, col, s * 0.09f);
    line(cx + s * 0.14f, cy + s * 0.14f, cx + s * 0.42f, cy + s * 0.42f, col, s * 0.11f);
}

void icon_link(float cx, float cy, float s, ImU32 col)
{
    /* two links of a chain */
    float t = s * 0.08f;
    stroke(cx - s * 0.48f, cy - s * 0.14f, s * 0.54f, s * 0.28f, col, s * 0.14f, t);
    stroke(cx - s * 0.06f, cy - s * 0.14f, s * 0.54f, s * 0.28f, col, s * 0.14f, t);
}

void icon_phone(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.26f, cy - s * 0.46f, s * 0.52f, s * 0.92f, col, s * 0.1f, s * 0.08f);
    rect(cx - s * 0.08f, cy + s * 0.3f, s * 0.16f, s * 0.05f, col, s * 0.02f);
    /* waves: it talks to the console */
    dl->PathArcTo(P(cx + s * 0.26f, cy - s * 0.3f), s * 0.2f * S, -0.9f, 0.4f, 12);
    dl->PathStroke(col, 0, s * 0.07f * S);
}

void option_icon(int icon, float cx, float cy, float s, ImU32 col)
{
    switch (icon) {
    case OI_SETTINGS: icon_gear(cx, cy, s * 0.8f, col); break;
    case OI_RESCAN: icon_jump(cx, cy, s * 0.95f, col, true, ""); break;
    case OI_RESUME: icon_play(cx + s * 0.06f, cy, s * 0.66f, col); break;
    case OI_TRACKS: icon_cc(cx, cy, s * 0.9f, col); break;
    case OI_PHONE: icon_phone(cx, cy, s, col); break;
    case OI_SEARCH: icon_search(cx, cy, s, col); break;
    case OI_LINK: icon_link(cx, cy, s, col); break;
    case OI_ADD_PL: icon_plus_circle(cx, cy, s * 0.95f, col); break;
    case OI_RENAME_PL: icon_list(cx, cy, s * 0.9f, col); break;
    default: rect(cx - s * 0.26f, cy - s * 0.26f, s * 0.52f, s * 0.52f, col, s * 0.08f); break;
    }
}

void modal_options()
{
    bool in_player = screen == PLAYER;
    struct Item {
        const char *label;
        int icon;
    };
    std::vector<Item> items;
    if (in_player)
        items = { { "Resume", OI_RESUME }, { "Audio, subtitles and more", OI_TRACKS },
                  { "Send files from a phone or PC", OI_PHONE }, { "Settings", OI_SETTINGS }, { "Stop playback", OI_STOP } };
    else
        items = { { "Search", OI_SEARCH }, { "Open a network link", OI_LINK },
                  { "Send files from a phone or PC", OI_PHONE }, { "Settings", OI_SETTINGS },
                  { "Rescan media", OI_RESCAN } };
    /* the file in front of you: into a playlist */
    int target = in_player ? current_item() : focused_item();
    std::string pl_target; /* Playlists tab: the one opened or selected */
    if (!in_player && tab == PLAYLISTS) {
        if (!pl_open.empty())
            pl_target = pl_open;
        else if (pl_focus > 0 && pl_focus - 1 < (int)library_playlists().size())
            pl_target = library_playlists()[pl_focus - 1].path;
        if (!library_playlist_ours(pl_target))
            pl_target.clear();
    }
    if (target >= 0 && !L()[target].other && !L()[target].text && !L()[target].archive && !L()[target].image &&
        !is_link(L()[target].path))
        items.insert(items.begin() + 1, { "Add to playlist", OI_ADD_PL });
    if (!pl_target.empty()) {
        items.insert(items.begin(), { "Delete playlist", OI_DELETE_PL });
        items.insert(items.begin(), { "Rename playlist", OI_RENAME_PL });
    }
    int n = (int)items.size();
    if (modal_focus >= n)
        modal_focus = n - 1;
    /* round the list: up from the first is the last, down from the last the first */
    if (hit(PAD_UP) && n > 0)
        modal_focus = (modal_focus + n - 1) % n;
    if (hit(PAD_DOWN) && n > 0)
        modal_focus = (modal_focus + 1) % n;
    int chosen = hit(PAD_CROSS) ? modal_focus : -1;
    if (hit(PAD_CIRCLE) || hit(PAD_OPTIONS)) {
        modal = NONE;
        return;
    }

    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 560, row = 84;
    float h = 112 + n * row + 4;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 30, x + 40, y + 34, alpha(C_TEXT, a), "Options");
    for (int i = 0; i < n; i++) {
        float ry = y + 104 + i * row;
        float f = focus_anim(hash_id(items[i].label, 940), i == modal_focus, 14);
        if (classic_look) {
            sel_box(x + 20, ry, w - 40, row - 12, f * a);
        } else if (f > 0.01f) {
            glow(x + 20, ry, w - 40, row - 12, 16, alpha(C_ORANGE, 0.35f * f * a), 12, 6);
            rect(x + 20, ry, w - 40, row - 12, alpha(C_CARD_HI, f * a), 16);
        }
        if (!classic_look)
            rect(x + 36, ry + 10, 52, 52, alpha(mix(IM_COL32(255, 255, 255, 14), C_ORANGE, 0.85f * f), a), 14);
        option_icon(items[i].icon, x + 62, ry + 36, 30,
                    alpha(f > 0.5f ? IM_COL32(255, 255, 255, 255) : C_DIM, a));
        text(f_semi, 25, x + 108, ry + 21, alpha(mix(C_DIM, C_TEXT, 0.55f + 0.45f * f), a), items[i].label);
    }
    if (chosen < 0)
        return;
    modal = NONE;
    switch (items[chosen].icon) {
    case OI_SETTINGS:
        modal = SETTINGS;
        modal_focus = 0;
        settings_inside = false;
        break;
    case OI_RESCAN: {
        library_rescan();
        rebuild_lists();
        rebuild_browse();
        char msg[64];
        snprintf(msg, sizeof(msg), tr("Found %zu files"), L().size());
        toast(msg);
        break;
    }
    case OI_PHONE:
        modal = PHONE;
        break;
    case OI_SEARCH:
        open_search();
        break;
    case OI_LINK:
        open_link();
        break;
    case OI_TRACKS:
        modal = TRACKS;
        modal_focus = 0;
        panel_section = SEC_ALL;
        break;
    case OI_STOP:
        leave_player(false);
        break;
    case OI_ADD_PL:
        open_playlist_pick(L()[target].path);
        break;
    case OI_RENAME_PL:
        pl_add_file.clear();
        open_playlist_name(pl_target);
        break;
    case OI_DELETE_PL:
        ask_delete(pl_target, NONE);
        break;
    default: /* Resume */
        break;
    }
}

void modal_info()
{
    if (info_about) {
        modal_about();
        return;
    }
    float a = ease(modal_anim);
    dim_screen(a);
    float w = 1100, h = 170 + info_lines.size() * 38;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 32, x + 48, y + 38, alpha(C_TEXT, a), info_title);
    for (size_t i = 0; i < info_lines.size(); i++)
        text(f_reg, 23, x + 48, y + 100 + i * 38, alpha(C_DIM, a), info_lines[i].c_str());
    hints(x + w - 40, y + h - 40, { { PAD_CIRCLE, "Close" } }, a);
    if (hit(PAD_CIRCLE) || hit(PAD_CROSS)) {
        modal = info_back;
        info_back = NONE;
    }
}

void setup_begin(bool with_logo);

/* ---- Settings: rows of label and value, ←/→ change, ✕ acts ---------------- */

enum {
    ST_DECODING, ST_ALANG, ST_SLANG, ST_SOUND, ST_NIGHT, ST_SPEAKERS, ST_THEME, ST_HAPTICS,
    ST_SWIPES, ST_REPEAT, ST_SHUFFLE, ST_DEBUG, ST_STORAGE, ST_VERSION, ST_RELOAD, ST_ABOUT,
    ST_WEB, ST_LOOK, ST_OSUB, ST_UILANG, ST_SETUP, ST_BDREGION, ST_COUNT
};
const char *const st_labels[ST_COUNT] = {
    "Decoding", "Audio language", "Subtitle language", "Sound output", "Night mode",
    "Speaker test", "Theme", "Controller vibration", "Touchpad swipes", "Repeat the list", "Shuffle",
    "Detailed log", "Storage", "Version", "Reload drives", "About VLC", "Phone access", "Look",
    "Subtitle downloads", "Language", "Run setup again", "Blu-ray region",
};
/* One line under the focused setting: what it does. */
const char *const st_help[ST_COUNT] = {
    "Auto switches to fast decoding when a video can't keep up",
    "The audio track VLC picks first, when a file has several",
    "The subtitles VLC turns on by itself",
    "Surround sends 5.1 and 7.1 files to every speaker of a home cinema",
    "Quieter explosions, clearer voices: for late evenings",
    "A tone goes round your speakers, one after another",
    "The accent colour. Add your own with vlc-theme.txt on a drive",
    "Small ticks for favourites, chapters, screenshots and seeks",
    "On the touchpad: swipe across to seek, up and down for the volume",
    "After the last file of a folder or playlist, start again from the first",
    "Play the list in a random order",
    "Writes VLC's warnings and stats to vlc-ps5.log, for bug reports",
    "What VLC can read on this console",
    "VLC for PS5 v" VLC_PS5_VERSION ", VLC media player 3.0.24",
    "Starts VLC again: it then sees drives plugged in since (when sandboxed)",
    "Version, credits and licences",
    "The page that sends files and takes your OpenSubtitles key, on your home network",
    "Classic: panels and folders like a desktop player. Modern: black, VLC 4 style. VLC restarts",
    "Your own free OpenSubtitles API key (and account), here or from the phone page",
    "The language of VLC's menus",
    "Language, look and playback, chosen again",
    "A: the Americas and East Asia. B: Europe, Africa and Oceania. C: China, Russia and India",
};

/* Categories: an icon, a name, and their settings. */
struct SettingsCat {
    const char *name;
    int icon;
    std::vector<int> rows;
};
enum { SI_PLAY, SI_SOUND, SI_THEME, SI_LIST, SI_GEAR };
const SettingsCat settings_cats[] = {
    { "Playback", SI_PLAY, { ST_DECODING, ST_ALANG, ST_SLANG, ST_OSUB, ST_BDREGION } },
    { "Sound", SI_SOUND, { ST_SOUND, ST_NIGHT, ST_SPEAKERS } },
    { "Interface", SI_THEME, { ST_UILANG, ST_LOOK, ST_THEME, ST_SWIPES } },
    { "Playlists", SI_LIST, { ST_REPEAT, ST_SHUFFLE } },
    { "System", SI_GEAR, { ST_WEB, ST_STORAGE, ST_RELOAD, ST_SETUP, ST_DEBUG, ST_ABOUT } },
};
const int SETTINGS_CATS = 5;

/* ---- settings icons: thin outlines, like the tab bar's ---------------------- */

void si_palette(float cx, float cy, float s, ImU32 col)
{
    ring(cx, cy, s * 0.44f, col, s * 0.08f);
    circle(cx - s * 0.16f, cy - s * 0.14f, s * 0.08f, col);
    circle(cx + s * 0.14f, cy - s * 0.18f, s * 0.08f, col);
    circle(cx + s * 0.2f, cy + s * 0.1f, s * 0.08f, col);
    circle(cx - s * 0.1f, cy + s * 0.18f, s * 0.11f, C_ORANGE);
}

void si_chip(float cx, float cy, float s, ImU32 col)
{
    stroke(cx - s * 0.3f, cy - s * 0.3f, s * 0.6f, s * 0.6f, col, s * 0.08f, s * 0.08f);
    rect(cx - s * 0.12f, cy - s * 0.12f, s * 0.24f, s * 0.24f, col, s * 0.03f);
    for (int i = -1; i <= 1; i++) {
        float o = i * s * 0.16f;
        line(cx + o, cy - s * 0.3f, cx + o, cy - s * 0.46f, col, s * 0.07f);
        line(cx + o, cy + s * 0.3f, cx + o, cy + s * 0.46f, col, s * 0.07f);
        line(cx - s * 0.3f, cy + o, cx - s * 0.46f, cy + o, col, s * 0.07f);
        line(cx + s * 0.3f, cy + o, cx + s * 0.46f, cy + o, col, s * 0.07f);
    }
}

void si_globe(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.07f;
    ring(cx, cy, s * 0.44f, col, t);
    dl->AddEllipse(P(cx, cy), ImVec2(s * 0.18f * S, s * 0.44f * S), col, 0, 32, t * S);
    line(cx - s * 0.44f, cy, cx + s * 0.44f, cy, col, t);
}

void si_moon(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    dl->PathArcTo(P(cx, cy), s * 0.42f * S, PI_F * 0.3f, PI_F * 1.7f, 32);
    dl->PathArcTo(P(cx + s * 0.18f, cy - s * 0.04f), s * 0.3f * S, PI_F * 1.55f, PI_F * 0.45f, 24);
    dl->PathStroke(col, ImDrawFlags_Closed, t * S);
}

void si_surround(float cx, float cy, float s, ImU32 col)
{
    circle(cx, cy, s * 0.1f, col);
    for (int i = 0; i < 6; i++) {
        float a = -PI_F / 2 + i * PI_F / 3;
        ring(cx + cosf(a) * s * 0.36f, cy + sinf(a) * s * 0.36f, s * 0.09f, col, s * 0.06f);
    }
}

void si_pad(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    stroke(cx - s * 0.48f, cy - s * 0.24f, s * 0.96f, s * 0.48f, col, s * 0.22f, t);
    line(cx - s * 0.3f, cy, cx - s * 0.12f, cy, col, t);
    line(cx - s * 0.21f, cy - s * 0.09f, cx - s * 0.21f, cy + s * 0.09f, col, t);
    circle(cx + s * 0.16f, cy - s * 0.04f, s * 0.05f, col);
    circle(cx + s * 0.28f, cy + s * 0.06f, s * 0.05f, col);
}

void si_touch(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    stroke(cx - s * 0.46f, cy - s * 0.3f, s * 0.92f, s * 0.6f, col, s * 0.1f, t);
    line(cx - s * 0.24f, cy, cx + s * 0.24f, cy, col, t);
    tri(cx + s * 0.3f, cy, cx + s * 0.18f, cy - s * 0.09f, cx + s * 0.18f, cy + s * 0.09f, col);
    tri(cx - s * 0.3f, cy, cx - s * 0.18f, cy - s * 0.09f, cx - s * 0.18f, cy + s * 0.09f, col);
}

void si_shuffle(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    line(cx - s * 0.44f, cy - s * 0.24f, cx + s * 0.3f, cy + s * 0.24f, col, t);
    line(cx - s * 0.44f, cy + s * 0.24f, cx + s * 0.3f, cy - s * 0.24f, col, t);
    tri(cx + s * 0.46f, cy - s * 0.24f, cx + s * 0.26f, cy - s * 0.36f, cx + s * 0.26f, cy - s * 0.12f, col);
    tri(cx + s * 0.46f, cy + s * 0.24f, cx + s * 0.26f, cy + s * 0.12f, cx + s * 0.26f, cy + s * 0.36f, col);
}

void si_doc(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    stroke(cx - s * 0.32f, cy - s * 0.44f, s * 0.64f, s * 0.88f, col, s * 0.08f, t);
    for (int i = 0; i < 3; i++)
        line(cx - s * 0.18f, cy - s * 0.2f + i * s * 0.2f, cx + s * 0.18f, cy - s * 0.2f + i * s * 0.2f, col, t);
}

void si_drive(float cx, float cy, float s, ImU32 col)
{
    float t = s * 0.08f;
    stroke(cx - s * 0.46f, cy - s * 0.2f, s * 0.92f, s * 0.4f, col, s * 0.1f, t);
    circle(cx + s * 0.28f, cy, s * 0.06f, col);
    line(cx - s * 0.3f, cy, cx + s * 0.08f, cy, col, t);
}

void si_info(float cx, float cy, float s, ImU32 col)
{
    ring(cx, cy, s * 0.44f, col, s * 0.08f);
    circle(cx, cy - s * 0.2f, s * 0.06f, col);
    line(cx, cy - s * 0.06f, cx, cy + s * 0.24f, col, s * 0.09f);
}

void category_icon(int icon, float cx, float cy, float s, ImU32 col)
{
    switch (icon) {
    case SI_PLAY: icon_play(cx + s * 0.06f, cy, s * 0.7f, col); break;
    case SI_SOUND: icon_speaker(cx, cy, s * 0.9f, col); break;
    case SI_THEME: si_palette(cx, cy, s, col); break;
    case SI_LIST: icon_list(cx, cy, s * 0.9f, col); break;
    default: icon_gear(cx, cy, s * 0.8f, col); break;
    }
}

void setting_icon(int id, float cx, float cy, float s, ImU32 col)
{
    switch (id) {
    case ST_DECODING: si_chip(cx, cy, s, col); break;
    case ST_ALANG: case ST_UILANG: si_globe(cx, cy, s, col); break;
    case ST_SLANG: case ST_OSUB: icon_cc(cx, cy, s * 0.9f, col); break;
    case ST_BDREGION: si_globe(cx, cy, s, col); break;
    case ST_SOUND: icon_speaker(cx, cy, s * 0.85f, col); break;
    case ST_NIGHT: si_moon(cx, cy, s, col); break;
    case ST_SPEAKERS: si_surround(cx, cy, s, col); break;
    case ST_THEME: si_palette(cx, cy, s, col); break;
    case ST_LOOK: /* a little window: title bar and a pane */
        stroke(cx - s * 0.46f, cy - s * 0.36f, s * 0.92f, s * 0.72f, col, s * 0.06f, s * 0.07f);
        rect(cx - s * 0.46f, cy - s * 0.36f, s * 0.92f, s * 0.18f, col, s * 0.06f);
        rect(cx - s * 0.3f, cy - s * 0.06f, s * 0.22f, s * 0.3f, col);
        break;
    case ST_HAPTICS: si_pad(cx, cy, s, col); break;
    case ST_SWIPES: si_touch(cx, cy, s, col); break;
    case ST_REPEAT: icon_repeat(cx, cy, s, col, 0); break;
    case ST_SHUFFLE: si_shuffle(cx, cy, s, col); break;
    case ST_DEBUG: si_doc(cx, cy, s, col); break;
    case ST_STORAGE: si_drive(cx, cy, s, col); break;
    case ST_RELOAD: icon_jump(cx, cy, s * 0.95f, col, true, ""); break;
    case ST_SETUP: icon_play(cx + s * 0.06f, cy, s * 0.66f, col); break;
    case ST_WEB: icon_phone(cx, cy, s, col); break;
    default: si_info(cx, cy, s, col); break;
    }
}

void cycle_language(bool sub, int step)
{
    std::string cur = sub ? player_sub_language() : player_audio_language();
    int i = 0, n = (int)(sizeof(languages) / sizeof(languages[0]));
    for (int k = 0; k < n; k++)
        if (cur == languages[k].code)
            i = k;
    do
        i = (i + (step < 0 ? n - 1 : 1)) % n;
    while (!sub && !strcmp(languages[i].code, "off"));
    if (sub)
        player_set_languages(player_audio_language(), languages[i].code);
    else
        player_set_languages(languages[i].code, player_sub_language());
}

std::string st_value(int id)
{
    static const char *const dec[] = { "Auto (fast when needed)", "Always fast", "Full quality" };
    switch (id) {
    case ST_DECODING: return dec[player_decode_mode()];
    case ST_ALANG: return lang_name(player_audio_language(), false);
    case ST_SLANG: return lang_name(player_sub_language(), true);
    case ST_SOUND: return player_audio_output() ? "Surround 5.1 / 7.1" : "Stereo";
    case ST_NIGHT: return player_night_mode() ? "On" : "Off";
    case ST_SPEAKERS: return "";
    case ST_THEME: {
        int i = pref_int("theme", 0);
        return i >= 0 && i < (int)themes.size() ? themes[i].name : themes[0].name;
    }
    case ST_HAPTICS: return pref_int("haptics", 1) ? "On" : "Off";
    case ST_SWIPES: return pref_int("swipes", 1) ? "On" : "Off";
    case ST_REPEAT: return repeat_list_on() ? "On" : "Off";
    case ST_SHUFFLE: return pref_int("shuffle", 0) ? "On" : "Off";
    case ST_DEBUG: return player_debug_log() ? "On" : "Off";
    case ST_STORAGE: return plat_full_access() ? "Full access" : "App folder and drives";
    case ST_VERSION: return VLC_PS5_VERSION;
    case ST_RELOAD: return "Restart";
    case ST_LOOK: return classic_look ? "Classic" : "Modern";
    case ST_UILANG: return lang_native_name(lang_current());
    case ST_OSUB: {
        if (pref_str("osub_key", "").empty())
            return "Not set";
        std::string u = pref_str("osub_user", "");
        return u.empty() ? std::string(tr("API key set")) : trf("API key, %s", u.c_str());
    }
    case ST_WEB: return web_running() ? "On" : "Off";
    case ST_ABOUT: return "v" VLC_PS5_VERSION;
    case ST_BDREGION: return trf("Region %s", player_bluray_region().c_str());
    default: return "";
    }
}

void st_change(int id, int step)
{
    int dir = step < 0 ? -1 : 1;
    switch (id) {
    case ST_DECODING:
        player_set_decode_mode((DecodeMode)((player_decode_mode() + 3 + dir) % 3));
        break;
    case ST_ALANG: cycle_language(false, dir); break;
    case ST_SLANG: cycle_language(true, dir); break;
    case ST_BDREGION: {
        static const char *const regions[3] = { "A", "B", "C" };
        std::string r = player_bluray_region();
        int i = r == "B" ? 1 : r == "C" ? 2 : 0;
        player_set_bluray_region(regions[(i + 3 + dir) % 3]);
        toast("Blu-ray region: from the next disc you open", 2.5);
        break;
    }
    case ST_SOUND:
        player_set_audio_output(!player_audio_output());
        toast(player_audio_output() ? "Surround: 5.1 / 7.1 files from the next one you open" : "Stereo from the next file", 3);
        break;
    case ST_NIGHT: player_set_night_mode(!player_night_mode()); break;
    case ST_SPEAKERS:
        if (step == 0) {
            if (player_speaker_test(0)) {
                modal = SPEAKERS;
                modal_focus = 0;
            } else {
                toast("The console didn't give VLC an 8-channel output", 3);
            }
        }
        break;
    case ST_THEME: {
        int n = (int)themes.size(), i = pref_int("theme", 0);
        pref_set("theme", (i + n + dir) % n);
        apply_theme();
        break;
    }
    case ST_HAPTICS:
        pref_set("haptics", !pref_int("haptics", 1));
        buzz(0.6f, 80);
        break;
    case ST_SWIPES: pref_set("swipes", !pref_int("swipes", 1)); break;
    case ST_REPEAT: pref_set("repeat_list", !repeat_list_on()); break;
    case ST_SHUFFLE: pref_set("shuffle", !pref_int("shuffle", 0)); break;
    case ST_WEB:
        if (web_running()) {
            web_stop();
        } else {
            web_start();
            if (!web_running())
                toast("Couldn't open a network port for the phone page", 3);
        }
        pref_set("web", web_running());
        break;
    case ST_LOOK:
        /* The whole interface is built for one look: VLC starts again in the other. */
        pref_set("look", classic_look ? 0 : 1);
        plat_restart();
        pref_set("look", classic_look ? 1 : 0);
        toast("Couldn't restart: close VLC and open it again to change the look", 4);
        break;
    case ST_OSUB:
        if (step == 0)
            open_osub_setup(SETTINGS);
        break;
    case ST_SETUP:
        if (step == 0)
            setup_begin(false);
        break;
    case ST_UILANG: {
        int n = lang_count();
        lang_set((lang_current() + n + dir) % n);
        break;
    }
    case ST_RELOAD:
        if (step == 0) {
            plat_restart();
            toast("Couldn't restart: close VLC and open it again", 4);
        }
        break;
    case ST_ABOUT:
        if (step == 0) {
            info_back = SETTINGS;
            show_about();
        }
        break;
    case ST_DEBUG:
        pref_set("debug", !player_debug_log());
        player_set_debug_log(!player_debug_log());
        break;
    }
}

void modal_settings()
{
    /* Classic has VLC orange only: no Theme row. */
    SettingsCat cat = settings_cats[settings_cat];
    if (classic_look)
        cat.rows.erase(std::remove(cat.rows.begin(), cat.rows.end(), (int)ST_THEME), cat.rows.end());
    int rows = (int)cat.rows.size();
    if (!settings_inside) {
        if (hit(PAD_UP) && settings_cat > 0)
            settings_cat--;
        if (hit(PAD_DOWN) && settings_cat + 1 < SETTINGS_CATS)
            settings_cat++;
        if (hit(PAD_RIGHT) || hit(PAD_CROSS)) {
            settings_inside = true;
            modal_focus = 0;
        }
        if (hit(PAD_CIRCLE) || hit(PAD_OPTIONS)) {
            modal = NONE;
            return;
        }
    } else {
        if (modal_focus >= rows)
            modal_focus = rows - 1;
        int id = cat.rows[modal_focus];
        bool fixed = id == ST_STORAGE || id == ST_VERSION;
        /* round the list: up from the first is the last, down from the last the first */
        if (hit(PAD_UP) && rows > 0)
            modal_focus = (modal_focus + rows - 1) % rows;
        if (hit(PAD_DOWN) && rows > 0)
            modal_focus = (modal_focus + 1) % rows;
        bool action = id == ST_SPEAKERS || id == ST_RELOAD || id == ST_ABOUT || id == ST_OSUB || id == ST_SETUP;
        if (!fixed && !action && (hit(PAD_LEFT) || hit(PAD_RIGHT)))
            st_change(id, hit(PAD_RIGHT) ? 1 : -1);
        else if (hit(PAD_LEFT) && action)
            settings_inside = false;
        if (!fixed && hit(PAD_CROSS))
            st_change(id, 0);
        if (hit(PAD_CIRCLE))
            settings_inside = false; /* and draw this frame as usual: no blink */
        if (modal != SETTINGS)
            return;
    }

    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 1260, h = 760, rail = 360;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    /* The rail: categories. */
    if (classic_look) {
        /* the navigation pane: a little darker, a line on its right */
        vgrad(x + 1.5f, y + 1.5f, rail, h - 3, alpha(IM_COL32(36, 39, 45, 255), a), alpha(IM_COL32(24, 26, 31, 255), a));
        rect(x + rail, y + 1.5f, 1, h - 3, alpha(IM_COL32(10, 11, 13, 255), a));
    } else {
        dl->AddRectFilled(P(x, y), P(x + rail, y + h), alpha(IM_COL32(16, 16, 22, 255), a), 24 * S,
                          ImDrawFlags_RoundCornersLeft);
    }
    text(f_bold, 30, x + 40, y + 36, alpha(C_TEXT, a), "Settings");
    for (int i = 0; i < SETTINGS_CATS; i++) {
        float ry = y + 112 + i * 80;
        bool sel = i == settings_cat;
        float f = focus_anim(hash_id(settings_cats[i].name, 920), sel, 12);
        float fo = focus_anim(hash_id(settings_cats[i].name, 921), sel && !settings_inside, 14);
        if (classic_look) {
            /* the open category: a quiet box while its settings have the focus */
            sel_box(x + 20, ry, rail - 40, 64, f * a * (0.45f + 0.55f * fo));
        } else if (f > 0.01f) {
            if (fo > 0.01f)
                glow(x + 20, ry, rail - 40, 64, 14, alpha(C_ORANGE, 0.45f * fo * a), 12, 6);
            rect(x + 20, ry, rail - 40, 64, alpha(mix(C_CARD_HI, C_ORANGE, fo), f * a), 14);
        }
        ImU32 col = classic_look ? alpha(mix(C_DIM, C_TEXT, f), a)
                                 : alpha(mix(C_DIM, fo > 0.5f ? IM_COL32(255, 255, 255, 255) : C_TEXT, f), a);
        category_icon(settings_cats[i].icon, x + 66, ry + 32, 30, col);
        text(sel ? f_bold : f_semi, 24, x + 104, ry + 17, col, settings_cats[i].name);
    }
    /* The settings of the category. */
    float px = x + rail + 48, pw = w - rail - 96;
    text(f_bold, 30, px, y + 36, alpha(C_TEXT, a), cat.name);
    /* The rows scroll in their own area, above the button hints, so a long
     * category never runs into them. */
    const float row_h = 96, list_top = y + 104, list_bottom = y + h - 84;
    float view = list_bottom - list_top, content = rows * row_h;
    static float scroll;
    static int scroll_cat = -1;
    if (scroll_cat != settings_cat) {
        scroll = 0;
        scroll_cat = settings_cat;
    }
    float want = settings_inside ? (modal_focus + 1) * row_h - view : 0;
    want = std::max(0.0f, std::min(want, std::max(0.0f, content - view)));
    scroll = approach(scroll, want, 14);
    dl->PushClipRect(P(px - 40, list_top - 6), P(px + pw + 40, list_bottom), true);
    for (int r = 0; r < rows; r++) {
        int id = cat.rows[r];
        float ry = list_top + 8 + r * row_h - scroll;
        bool foc = settings_inside && r == modal_focus;
        float f = focus_anim(hash_id(st_labels[id], 930), foc, 14);
        if (classic_look) {
            sel_box(px - 16, ry, pw + 32, 80, f * a);
        } else if (f > 0.01f) {
            glow(px - 16, ry, pw + 32, 80, 16, alpha(C_ORANGE, 0.35f * f * a), 12, 6);
            rect(px - 16, ry, pw + 32, 80, alpha(C_CARD_HI, f * a), 16);
        }
        /* Icon in a soft square. */
        if (!classic_look)
            rect(px, ry + 14, 52, 52, alpha(mix(IM_COL32(255, 255, 255, 14), C_ORANGE, 0.85f * f), a), 14);
        setting_icon(id, px + 26, ry + 40, 30, alpha(f > 0.5f ? IM_COL32(255, 255, 255, 255) : C_DIM, a));
        text(f_semi, 25, px + 76, ry + (foc ? 12 : 26), alpha(mix(C_DIM, C_TEXT, 0.55f + 0.45f * f), a), st_labels[id]);
        std::string v = st_value(id);
        if (foc) {
            float room = pw - 76 - text_size(f_semi, 22, id == ST_SPEAKERS || id == ST_ABOUT ? "Open" : v.c_str()).x - 90;
            text(f_reg, 18, px + 76, ry + 46, alpha(C_FAINT, a * f), st_help[id], 0, room);
        }
        float vx = px + pw;
        bool fixed = id == ST_STORAGE || id == ST_VERSION;
        if (id == ST_SPEAKERS || id == ST_RELOAD || id == ST_ABOUT || id == ST_SETUP) {
            const char *act = id == ST_RELOAD ? "Restart" : "Open";
            text(f_semi, 22, vx, ry + 27, alpha(foc ? C_TEXT : C_DIM, a), act, 1);
        } else if (fixed || id == ST_OSUB) {
            text(f_semi, 22, vx, ry + 27, alpha(id == ST_OSUB && v != "Not set" ? C_TEXT : C_DIM, a), v.c_str(), 1);
        } else {
            /* ‹ value › when focused. */
            float tw = text_size(f_semi, 22, v.c_str()).x;
            if (f > 0.05f) {
                tri(vx - 2, ry + 32, vx - 2, ry + 48, vx + 8, ry + 40, alpha(C_ORANGE, f * a));
                tri(vx - tw - 34, ry + 32, vx - tw - 34, ry + 48, vx - tw - 44, ry + 40, alpha(C_ORANGE, f * a));
            }
            if (id == ST_THEME)
                circle(vx - tw - 64 - 14 * f, ry + 40, 9, alpha(C_ORANGE, a));
            text(f_semi, 22, vx - 18 * f, ry + 27, alpha(v == "Off" ? C_DIM : C_TEXT, a), v.c_str(), 1);
        }
    }
    dl->PopClipRect();
    if (content > view) {
        /* A thin bar: there is more below / above. */
        float bh = view * view / content, by2 = list_top + (view - bh) * (scroll / (content - view));
        rect(px + pw + 30, by2, 4, bh, alpha(IM_COL32(255, 255, 255, 60), a), 2);
    }
    int fid = settings_inside && modal_focus < rows ? cat.rows[modal_focus] : -1;
    if (fid == ST_STORAGE || fid == ST_VERSION)
        hints(x + w - 40, y + h - 36, { { PAD_CIRCLE, "Back" } }, a);
    else if (settings_inside)
        hints(x + w - 40, y + h - 36, { { PAD_CROSS, fid == ST_SPEAKERS || fid == ST_ABOUT ? "Open" : fid == ST_RELOAD ? "Restart" : "Change" }, { PAD_CIRCLE, "Back" } }, a);
    else
        hints(x + w - 40, y + h - 36, { { PAD_CROSS, "Open" }, { PAD_CIRCLE, "Close" } }, a);
}

/* ---- Speaker test: the room from above, a tone going round ---------------- */

void modal_speakers()
{
    /* Console channels: L R C LFE Ls Rs Lb Rb. Round the room clockwise. */
    static const int order[8] = { 0, 2, 1, 5, 7, 6, 4, 3 };
    static const char *const names[8] = { "Front left", "Front right", "Centre", "Subwoofer",
                                          "Surround left", "Surround right", "Back left", "Back right" };
    static const char *const short_names[8] = { "L", "R", "C", "LFE", "SL", "SR", "BL", "BR" };
    static const float pos[8][2] = { { -0.42f, -0.78f }, { 0.42f, -0.78f }, { 0, -0.9f },
                                     { 0.78f, -0.62f }, { -0.95f, 0.05f }, { 0.95f, 0.05f },
                                     { -0.5f, 0.82f }, { 0.5f, 0.82f } };
    static double next_at;
    static bool autoplay = true;
    if (modal_focus < 0 || modal_focus > 7)
        modal_focus = 0;
    bool moved = false;
    if (hit(PAD_RIGHT)) {
        modal_focus = (modal_focus + 1) % 8;
        moved = true;
    }
    if (hit(PAD_LEFT)) {
        modal_focus = (modal_focus + 7) % 8;
        moved = true;
    }
    if (moved)
        autoplay = false;
    if (hit(PAD_CROSS))
        autoplay = !autoplay;
    if (autoplay && now >= next_at) {
        next_at = now + 1.6;
        modal_focus = (modal_focus + 1) % 8;
    }
    int ch = order[modal_focus];
    player_speaker_test(ch);
    if (hit(PAD_CIRCLE)) {
        player_speaker_test(-1);
        modal = SETTINGS;
        settings_cat = 1;
        settings_inside = true;
        modal_focus = 2;
        return;
    }
    float a = ease(modal_anim);
    dim_screen(a);
    float w = 1100, h = 860, x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 32, x + 44, y + 32, alpha(C_TEXT, a), "Speaker test");
    text(f_reg, 21, x + 44, y + 80, alpha(C_DIM, a),
         "Each speaker plays a tone in turn. If one comes from the wrong place, tell us which.");
    float cx = 960, cy = y + 470, rx = 400, ry = 300;
    /* The listener. */
    circle(cx, cy, 34, alpha(IM_COL32(255, 255, 255, 30), a));
    circle(cx, cy - 6, 12, alpha(C_DIM, a));
    rect(cx - 20, cy + 10, 40, 16, alpha(C_DIM, a), 8);
    for (int k = 0; k < 8; k++) {
        int c = order[k];
        float sx = cx + pos[c][0] * rx, sy = cy + pos[c][1] * ry;
        bool on = c == ch;
        float f = focus_anim(hash_id(short_names[c], 950), on, 10);
        if (f > 0.02f) {
            float pulse = 1 + 0.15f * sinf((float)now * 9);
            ring(sx, sy, (46 + 18 * f) * pulse, alpha(C_ORANGE, 0.5f * f * a), 3);
            glow(sx - 40, sy - 40, 80, 80, 40, alpha(C_ORANGE, 0.6f * f * a), 18, 6);
        }
        circle(sx, sy, 40, alpha(mix(C_CARD_HI, C_ORANGE, f), a));
        text(f_bold, 22, sx, sy - 14, alpha(mix(C_DIM, C_BG, f), a), short_names[c], 0.5f);
    }
    text(f_bold, 30, 960, y + h - 130, alpha(C_TEXT, a), names[ch], 0.5f);
    hints(x + w - 40, y + h - 40,
          { { PAD_CROSS, autoplay ? "Stop going round" : "Go round" },
            { PAD_CIRCLE, "Done" } }, a);
}

/* Options > Send files from a phone: a QR code of the page's address, what
 * the page does, and the upload under way. */
void modal_phone()
{
    if (hit(PAD_CIRCLE) || hit(PAD_CROSS) || hit(PAD_OPTIONS)) {
        modal = NONE;
        return;
    }
    std::string url = web_running() ? web_address() : "";
    float a = ease(modal_anim);
    dim_screen(a);
    const float w = 1000, h = 520;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel_band = 132;
    panel(x, y, w, h, a);
    rect(x + 48, y + 48, 64, 64, alpha(C_ORANGE, a), 16);
    icon_phone(x + 80, y + 80, 40, alpha(IM_COL32(255, 255, 255, 255), a));
    text(f_bold, 32, x + 136, y + 62, alpha(C_TEXT, a), "Send files from your phone or PC");
    if (!web_running()) {
        text(f_reg, 24, x + 48, y + 160, alpha(C_DIM, a), "Phone access is off. Turn it on in Settings > System.", 0, w - 96);
    } else if (url.empty()) {
        text(f_reg, 24, x + 48, y + 160, alpha(C_DIM, a), "The PS5 isn't connected to a network. Connect it, then come back.", 0, w - 96);
    } else {
        text(f_reg, 24, x + 48, y + 150, alpha(C_DIM, a), "On a phone or computer on the same Wi-Fi, open this address in the browser:", 0, w - 96);
        rect(x + 48, y + 200, w - 96, 96, alpha(C_CARD_HI, a), 18);
        text(f_bold, 44, x + w / 2, y + 222, alpha(C_ORANGE, a), url.c_str(), 0.5f, w - 140);
        static const char *const what[] = { "Send videos, music and subtitles to the PS5",
                                            "Add your OpenSubtitles API key" };
        for (int i = 0; i < 2; i++) {
            float ly = y + 330 + i * 42;
            circle(x + 58, ly + 14, 5, alpha(C_ORANGE, a));
            text(f_reg, 22, x + 78, ly, alpha(C_TEXT, a), what[i], 0, w - 130);
        }
    }
    WebUpload u = web_upload_state();
    if (u.active && u.total > 0) {
        float frac = (float)u.done / u.total, by = y + h - 108;
        text(f_semi, 20, x + 48, by, alpha(C_DIM, a), trf("Receiving %s", u.name.c_str()).c_str(), 0, w - 200);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
        text(f_semi, 20, x + w - 48, by, alpha(C_TEXT, a), pct, 1);
        rect(x + 48, by + 34, w - 96, 8, alpha(IM_COL32(255, 255, 255, 40), a), 4);
        rect(x + 48, by + 34, (w - 96) * frac, 8, alpha(C_ORANGE, a), 4);
    }
    hints(x + w - 40, y + h - 36, { { PAD_CIRCLE, "Close" } }, a);
}

/* The page's subtitle-download card, and what arrived from it. */
void web_frame()
{
    static double next_status;
    if (now >= next_status) {
        next_status = now + 0.5;
        WebOsubInfo info;
        info.has_key = osub_has_key();
        /* the key check as the TV saw it (osub_state() hands a result over
         * once: asking here would take it from the TV) */
        info.checking = os_checking;
        info.failed = !os_checking && !os_status.empty() && !os_status_ok;
        info.user = pref_str("osub_user", "");
        info.message = os_checking ? "" : os_status;
        web_set_osub_info(info);
    }
    std::string ok_key, ok_user, ok_pass;
    if (web_take_opensubtitles(&ok_key, &ok_user, &ok_pass)) {
        pref_set("osub_key", trimmed(ok_key));
        pref_set("osub_user", trimmed(ok_user));
        pref_set("osub_pass", ok_pass);
        if (modal == OSUB_SETUP)
            open_osub_setup(os_back); /* the form shows what arrived */
        toast("OpenSubtitles key received from your phone or PC", 3);
        osub_check();
    }
    if (modal != OSUB_SETUP && modal != OSUB_RESULTS)
        osub_check_tick();
    if (web_take_finished()) {
        toast("A file arrived from your phone or PC", 2.5);
        if (screen != PLAYER) {
            library_rescan();
            rebuild_lists();
            rebuild_browse();
        }
    }
}

/* Where the page may put files: the app's media folder, /data/vlc, drives. */
void web_places_update()
{
    std::vector<WebPlace> places;
    for (const std::string &r : library_roots()) {
        std::string name = r;
        if (r == library_media_dir())
            name = "VLC media folder";
        else if (!r.compare(0, 8, "/mnt/usb"))
            name = trf("USB drive %d", atoi(r.c_str() + 8) + 1);
        else if (!r.compare(0, 8, "/mnt/ext"))
            name = "Extended storage";
        else if (r == "/data/vlc")
            name = "Console storage (/data/vlc)";
        places.push_back({ name, r });
    }
    web_set_places(places);
}

/* While a file arrives and the phone window is closed: a small pill. */
void upload_pill()
{
    WebUpload u = web_upload_state();
    static float k;
    k = approach(k, u.active && modal != PHONE ? 1.0f : 0.0f, 10);
    if (k < 0.01f || u.total <= 0)
        return;
    float frac = (float)u.done / u.total, w = 520, x = 48, y = 1080 - 120 + (1 - k) * 30;
    rect(x, y, w, 76, alpha(IM_COL32(24, 24, 31, 240), k), 18);
    icon_phone(x + 38, y + 38, 34, alpha(C_ORANGE, k));
    text(f_semi, 20, x + 72, y + 12, alpha(C_TEXT, k), trf("Receiving %s", u.name.c_str()).c_str(), 0, w - 160);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
    text(f_semi, 20, x + w - 24, y + 12, alpha(C_DIM, k), pct, 1);
    rect(x + 72, y + 50, w - 96, 6, alpha(IM_COL32(255, 255, 255, 40), k), 3);
    rect(x + 72, y + 50, (w - 96) * frac, 6, alpha(C_ORANGE, k), 3);
}

void modal_details()
{
    const MediaDetails &d = library_details(details_for);
    float a = ease(modal_anim);
    dim_screen(a);
    size_t rows = d.ready ? d.rows.size() : 4;
    float w = 1240, h = 190 + rows * 46;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    MediaItem *m = library_find(details_for);
    text(f_bold, 32, x + 48, y + 38, alpha(C_TEXT, a), m ? m->name.c_str() : "Details", 0, w - 96);
    if (!d.ready) {
        spinner(x + 70, y + 140, 18);
        text(f_reg, 23, x + 104, y + 126, alpha(C_DIM, a), "Reading the file...");
    }
    for (size_t i = 0; d.ready && i < d.rows.size(); i++) {
        float ry = y + 104 + i * 46;
        text(f_semi, 22, x + 48, ry, alpha(C_FAINT, a), d.rows[i].first.c_str());
        text(f_reg, 23, x + 230, ry - 1, alpha(C_TEXT, a), d.rows[i].second.c_str(), 0, w - 290);
    }
    hints(x + w - 40, y + h - 40, { { PAD_SQUARE, "Delete" }, { PAD_CIRCLE, "Close" } }, a);
    if (hit(PAD_SQUARE)) {
        ask_delete(details_for, DETAILS);
        return;
    }
    if (hit(PAD_CIRCLE) || hit(PAD_CROSS))
        modal = NONE;
}

/* "Delete …? This can't be undone." Cancel is focused first. */
void modal_confirm_delete()
{
    if (delete_path.find("://") != std::string::npos) {
        /* A saved network share: forgotten, nothing on the server changes. */
        if (hit(PAD_LEFT))
            modal_focus = 0;
        if (hit(PAD_RIGHT))
            modal_focus = 1;
        float a = ease(modal_anim);
        dim_screen(a);
        float w = 820, h = 330, x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
        panel(x, y, w, h, a);
        std::string name = delete_path.substr(delete_path.find("://") + 3);
        text(f_bold, 30, x + 48, y + 40, alpha(C_TEXT, a), trf("Forget %s?", name.c_str()).c_str(), 0, w - 96);
        text(f_reg, 23, x + 48, y + 96, alpha(C_DIM, a), "VLC forgets the share and its login. Nothing on it changes.", 0, w - 96);
        const char *labels[2] = { "Forget", "Cancel" };
        for (int i = 0; i < 2; i++) {
            float bw = 340, bx = x + 48 + i * (bw + 44), by = y + 186;
            float f = focus_anim(510 + i, modal_focus == i);
            if (f > 0.01f)
                glow(bx, by, bw, 76, 38, alpha(C_ORANGE, f * a * 0.7f), 14, 6);
            if (classic_look)
                classic_button(bx, by, bw, 76, f, a, C_ORANGE);
            else
                rect(bx, by, bw, 76, alpha(mix(IM_COL32(255, 255, 255, 26), C_ORANGE, f), a), 38);
            text(f_bold, 26, bx + bw / 2, by + 22, alpha(C_TEXT, a), labels[i], 0.5f);
        }
        if (hit(PAD_CIRCLE) || (hit(PAD_CROSS) && modal_focus == 1)) {
            modal = delete_back;
        } else if (hit(PAD_CROSS)) {
            net_forget_server(delete_path);
            rebuild_browse();
            toast(trf("Forgot %s", name.c_str()), 3);
            modal = NONE;
        }
        return;
    }
    struct stat st;
    bool folder = stat(delete_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    static std::string counted_for;
    static int count;
    if (counted_for != delete_path) {
        counted_for = delete_path;
        count = folder ? library_count_files(delete_path) : 1;
    }
    if (hit(PAD_LEFT))
        modal_focus = 0;
    if (hit(PAD_RIGHT))
        modal_focus = 1;
    float a = ease(modal_anim);
    dim_screen(a);
    float w = 820, h = 330, x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    std::string name = delete_path.substr(delete_path.rfind('/') + 1);
    text(f_bold, 30, x + 48, y + 40, alpha(C_TEXT, a), trf("Delete %s?", name.c_str()).c_str(), 0, w - 96);
    char what[160];
    if (folder)
        snprintf(what, sizeof(what), tr(count == 1 ? "The folder and the file in it are deleted for good."
                                                   : "The folder and the %d files in it are deleted for good."), count);
    else
        snprintf(what, sizeof(what), "The file is deleted from the PS5 for good.");
    text(f_reg, 23, x + 48, y + 96, alpha(C_DIM, a), what, 0, w - 96);
    const char *labels[2] = { "Delete", "Cancel" };
    for (int i = 0; i < 2; i++) {
        float bw = 340, bx = x + 48 + i * (bw + 44), by = y + 186;
        float f = focus_anim(510 + i, modal_focus == i);
        ImU32 hot = i == 0 ? IM_COL32(230, 60, 70, 255) : C_ORANGE;
        if (f > 0.01f)
            glow(bx, by, bw, 76, 38, alpha(hot, f * a * 0.7f), 14, 6);
        if (classic_look)
            classic_button(bx, by, bw, 76, f, a, hot);
        else
            rect(bx, by, bw, 76, alpha(mix(IM_COL32(255, 255, 255, 26), hot, f), a), 38);
        text(f_bold, 26, bx + bw / 2, by + 22, alpha(C_TEXT, a), labels[i], 0.5f);
    }
    if (hit(PAD_CIRCLE) || (hit(PAD_CROSS) && modal_focus == 1)) {
        modal = delete_back;
        return;
    }
    if (hit(PAD_CROSS) && modal_focus == 0) {
        bool ok = library_delete(delete_path);
        rebuild_lists();
        rebuild_browse();
        if (browse_focus >= (int)browse_entries.size())
            browse_focus = browse_entries.empty() ? 0 : (int)browse_entries.size() - 1;
        toast(ok ? trf("Deleted %s", name.c_str()) : trf("Couldn't delete all of %s (read-only drive?)", name.c_str()), 3);
        modal = NONE;
    }
}

void modal_resume()
{
    MediaItem *m = library_find(pending_path);
    if (!m) {
        modal = NONE;
        return;
    }
    float a = ease(modal_anim);
    dim_screen(a);
    float w = 760, h = 330;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    panel(x, y, w, h, a);
    text(f_bold, 32, x + 48, y + 40, alpha(C_TEXT, a), m->name.c_str(), 0, w - 96);
    std::string where = m->length_ms > 0 ? trf("You stopped at %s of %s", format_time(pending_resume).c_str(),
                                               format_time(m->length_ms).c_str())
                                         : trf("You stopped at %s", format_time(pending_resume).c_str());
    text(f_reg, 24, x + 48, y + 92, alpha(C_DIM, a), where.c_str());
    if (m->length_ms > 0) {
        float frac = std::min(1.0f, (float)pending_resume / m->length_ms);
        rect(x + 48, y + 142, w - 96, 6, alpha(IM_COL32(255, 255, 255, 50), a), 3);
        rect(x + 48, y + 142, (w - 96) * frac, 6, alpha(C_ORANGE, a), 3);
    }
    if (hit(PAD_LEFT))
        modal_focus = 0;
    if (hit(PAD_RIGHT))
        modal_focus = 1;
    const char *labels[2] = { "Resume", "Start over" };
    for (int i = 0; i < 2; i++) {
        float bw = 316, bx = x + 48 + i * (bw + 32), by = y + 200;
        float f = focus_anim(500 + i, modal_focus == i);
        if (f > 0.01f)
            glow(bx, by, bw, 76, 38, alpha(C_ORANGE, f * a * 0.8f), 14, 6);
        if (classic_look)
            classic_button(bx, by, bw, 76, f, a, C_ORANGE);
        else
            rect(bx, by, bw, 76, alpha(mix(IM_COL32(255, 255, 255, 26), C_ORANGE, f), a), 38);
        if (i == 0)
            icon_play(bx + 66, by + 38, 24, alpha(C_TEXT, a));
        else
            icon_jump(bx + 62, by + 38, 30, alpha(C_TEXT, a), false, "");
        text(f_bold, 26, bx + bw / 2 + 22, by + 22, alpha(C_TEXT, a), labels[i], 0.5f);
    }
    if (hit(PAD_CIRCLE)) {
        modal = NONE;
        return;
    }
    if (hit(PAD_CROSS)) {
        modal = NONE;
        int item = (int)(m - &L()[0]);
        std::vector<int> list = playlist;
        start_playback(item, modal_focus == 0 ? pending_resume : 0, list);
    }
}

/* The Tracks panel: slides in from the right in the player. */
struct PanelEntry {
    int kind; /* 0 header, 1 audio, 2 subtitle, 3 speed, 4 aspect, 5 volume, 6 subtitle file,
               * 7 setting (←/→ changes, ✕ resets or acts), 8 chapter */
    int value; /* kinds 1-4, 8: what it picks; kind 7: which setting */
    std::string label;
    bool active;
    std::string path; /* kind 6 */
    std::string shown; /* kind 7: the value, right-aligned */
};

/* A disc plays: from the library, dvd:// / bluray://, or an .iso / .img
 * from a share or a link. */
bool playing_disc()
{
    int cur_item = current_item();
    std::string bare = playing_path.substr(0, playing_path.find('?'));
    return (cur_item >= 0 && L()[cur_item].disc) || playing_path.compare(0, 6, "dvd://") == 0 ||
           playing_path.compare(0, 9, "bluray://") == 0 ||
           (bare.size() > 4 && (!strcasecmp(bare.c_str() + bare.size() - 4, ".iso") || !strcasecmp(bare.c_str() + bare.size() - 4, ".img")));
}

/* The settings rows (PanelEntry kind 7). */
enum {
    SET_AUDIO_DELAY, SET_EQ, SET_SUB_DELAY, SET_SUB_SIZE, SET_SUB_COLOUR, SET_SUB_BOX, SET_SLEEP,
    SET_LOOP, SET_DEINT, SET_BRIGHT, SET_CONTRAST, SET_SAT, SET_GAMMA, SET_HUE, SET_ROTATE, SET_FLIP,
    SET_SHARPEN, SET_PIC_RESET,
    SET_SOUND_OUT, SET_NIGHT, SET_REPEAT, SET_SHUFFLE, SET_BOOKMARK, SET_BM_CLEAR, SET_OSUB,
    SET_DISC_MENU, SET_POPUP, SET_SUB_PICK,
};

std::string ms_text(int64_t ms)
{
    if (!ms)
        return "0 ms";
    char b[32];
    snprintf(b, sizeof(b), "%+lld ms", (long long)ms);
    return b;
}

std::string signed_text(int v, const char *unit = "")
{
    char b[32];
    if (!v)
        snprintf(b, sizeof(b), "0%s", unit);
    else
        snprintf(b, sizeof(b), "%+d%s", v, unit);
    return b;
}

std::string setting_value(int id)
{
    SubStyle st = player_sub_style();
    PictureAdjust pa = player_picture();
    static const char *const deint[] = { "Auto", "On", "Off" };
    switch (id) {
    case SET_AUDIO_DELAY: return ms_text(player_audio_delay());
    case SET_SUB_DELAY: return ms_text(player_subtitle_delay());
    case SET_EQ: return player_eq_name(player_eq());
    case SET_SUB_SIZE: return std::to_string(st.size) + "%";
    case SET_SUB_COLOUR: return sub_colour_names[st.colour];
    case SET_SUB_BOX: return st.box ? "On" : "Off";
    case SET_SLEEP:
        if (sleep_mode != SLEEP_OFF && sleep_mode != SLEEP_END) {
            int left = (int)((sleep_at - now) / 60 + 0.99);
            return trf("%d min left", left < 1 ? 1 : left);
        }
        return sleep_names[sleep_mode];
    case SET_LOOP:
        if (loop_a < 0)
            return "Off";
        if (loop_b < 0)
            return trf("A at %s", format_time(loop_a).c_str());
        return format_time(loop_a) + " - " + format_time(loop_b);
    case SET_DEINT: return deint[player_deinterlace()];
    case SET_BRIGHT: return signed_text(pa.brightness);
    case SET_CONTRAST: return signed_text(pa.contrast);
    case SET_SAT: return signed_text(pa.saturation);
    case SET_GAMMA: return signed_text(pa.gamma);
    case SET_HUE: return signed_text(pa.hue, "°");
    case SET_ROTATE: {
        static const char *const turns[] = { "Off", "90° right", "180°", "90° left" };
        return turns[pa.rotate & 3];
    }
    case SET_FLIP:
        return pa.mirror && pa.upside_down ? "Both ways" : pa.mirror ? "Mirror" : pa.upside_down ? "Upside down" : "Off";
    case SET_SHARPEN: return pa.sharpen ? std::to_string(pa.sharpen) + "%" : "Off";
    case SET_SOUND_OUT: return player_audio_output() ? "Surround" : "Stereo";
    case SET_NIGHT: return player_night_mode() ? "On" : "Off";
    case SET_REPEAT: return loop_on() ? "On" : "Off";
    case SET_SHUFFLE: return pref_int("shuffle", 0) ? "On" : "Off";
    case SET_BOOKMARK: {
        size_t n = library_bookmarks(playing_path).size();
        return n ? trf("%d saved", (int)n) : std::string(tr("None yet"));
    }
    case SET_BM_CLEAR: return "";
    case SET_OSUB: return osub_has_key() ? "OpenSubtitles" : "Set up";
    default: return "";
    }
}

/* ←/→ (step -1/+1) or ✕ (step 0) on a settings row. */
void setting_change(int id, int step)
{
    SubStyle st = player_sub_style();
    PictureAdjust pa = player_picture();
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    switch (id) {
    case SET_OSUB:
        if (step == 0)
            open_osub_results();
        return;
    case SET_SUB_PICK:
        if (step == 0)
            start_sub_pick();
        return;
    case SET_DISC_MENU:
    case SET_POPUP:
        /* the menu shows over the film: the panel gets out of its way */
        if (step == 0) {
            player_disc_menu(id == SET_POPUP);
            modal = NONE;
        }
        return;
    case SET_AUDIO_DELAY:
        player_set_audio_delay(step ? player_audio_delay() + step * 50 : 0);
        break;
    case SET_SUB_DELAY:
        player_set_subtitle_delay(step ? player_subtitle_delay() + step * 50 : 0);
        break;
    case SET_EQ: {
        int n = player_eq_presets(), e = player_eq();
        player_set_eq(step ? (e + 1 + step + n + 1) % (n + 1) - 1 : -1);
        break;
    }
    case SET_SUB_SIZE: {
        static const int sizes[] = { 50, 75, 100, 125, 150, 175, 200 };
        int i = 2;
        for (int k = 0; k < 7; k++)
            if (sizes[k] == st.size)
                i = k;
        st.size = step ? sizes[clamp(i + step, 0, 6)] : 100;
        player_set_sub_style(st);
        break;
    }
    case SET_SUB_COLOUR:
        st.colour = step ? (st.colour + step + SUB_COLOURS) % SUB_COLOURS : SUB_WHITE;
        player_set_sub_style(st);
        break;
    case SET_SUB_BOX:
        if (step > 0 && st.box)
            break;
        st.box = !st.box;
        player_set_sub_style(st);
        break;
    case SET_SLEEP:
        sleep_mode = step ? (sleep_mode + step + SLEEP_MODES) % SLEEP_MODES : SLEEP_OFF;
        sleep_at = now + sleep_minutes[sleep_mode] * 60.0;
        break;
    case SET_LOOP:
        if (step)
            break;
        if (loop_a < 0) {
            loop_a = player_time();
        } else if (loop_b < 0) {
            int64_t t = player_time();
            if (t > loop_a + 500) {
                loop_b = t;
                toast(trf("Repeating %s - %s", format_time(loop_a).c_str(), format_time(loop_b).c_str()), 2);
            }
        } else {
            loop_a = loop_b = -1;
            toast("A-B repeat off", 1.5);
        }
        break;
    case SET_DEINT:
        player_set_deinterlace(step ? (player_deinterlace() + step + 3) % 3 : 0);
        break;
    case SET_BRIGHT: pa.brightness = step ? clamp(pa.brightness + step * 5, -100, 100) : 0; break;
    case SET_CONTRAST: pa.contrast = step ? clamp(pa.contrast + step * 5, -100, 100) : 0; break;
    case SET_SAT: pa.saturation = step ? clamp(pa.saturation + step * 5, -100, 100) : 0; break;
    case SET_GAMMA: pa.gamma = step ? clamp(pa.gamma + step * 5, -100, 100) : 0; break;
    case SET_HUE: pa.hue = step ? clamp(pa.hue + step * 5, -180, 180) : 0; break;
    case SET_ROTATE: pa.rotate = step ? (pa.rotate + step + 4) & 3 : 0; break;
    case SET_FLIP: {
        /* Off, Mirror, Upside down, Both ways */
        int f = (pa.mirror ? 1 : 0) | (pa.upside_down ? 2 : 0);
        f = step ? (f + step + 4) & 3 : 0;
        pa.mirror = f & 1;
        pa.upside_down = f & 2;
        break;
    }
    case SET_SHARPEN: pa.sharpen = step ? clamp(pa.sharpen + step * 10, 0, 100) : 0; break;
    case SET_SOUND_OUT:
        player_set_audio_output(!player_audio_output());
        toast("From the next file you open", 2);
        break;
    case SET_NIGHT: player_set_night_mode(!player_night_mode()); break;
    case SET_REPEAT: pref_set("loop", !loop_on()); break;
    case SET_SHUFFLE: pref_set("shuffle", !pref_int("shuffle", 0)); break;
    case SET_BOOKMARK:
        if (!step) {
            library_add_bookmark(playing_path, player_time());
            toast(trf("Bookmark at %s", format_time(player_time()).c_str()), 1.5);
            buzz(0.4f, 40);
        }
        break;
    case SET_BM_CLEAR:
        if (!step) {
            library_clear_bookmarks(playing_path);
            toast("Bookmarks cleared", 1.5);
        }
        break;
    case SET_PIC_RESET:
        if (!step) {
            pa = PictureAdjust{};
            toast("Picture reset", 1.5);
        }
        break;
    }
    if (id >= SET_BRIGHT && id <= SET_PIC_RESET)
        player_set_picture(pa);
}

/* Subtitle files in the playing video's folder, to load on demand. */
std::vector<std::string> subtitle_files()
{
    static const char *const exts[] = { "srt", "ass", "ssa", "vtt", "sub", "smi", "txt", nullptr };
    std::vector<std::string> out;
    std::string dir = playing_path.substr(0, playing_path.rfind('/'));
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || e->d_name[0] == '.')
            continue;
        for (const char *const *x = exts; *x; x++)
            if (!strcasecmp(dot + 1, *x) && strcasecmp(dot + 1, "txt") != 0)
                out.push_back(dir + "/" + e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

void modal_tracks()
{
    bool all = panel_section == SEC_ALL;
    std::vector<PanelEntry> e;
    if (all || panel_section == SEC_AUDIO) {
        e.push_back({ 0, 0, "Audio", false, "" });
        char vol[32];
        snprintf(vol, sizeof(vol), tr("Volume  %d%%"), player_volume());
        e.push_back({ 5, 0, vol, false, "" });
        e.push_back({ 7, SET_AUDIO_DELAY, "Audio delay", false, "" });
        e.push_back({ 7, SET_EQ, "Equalizer", false, "" });
        e.push_back({ 7, SET_NIGHT, "Night mode", false, "" });
        e.push_back({ 7, SET_SOUND_OUT, "Sound output", false, "" });
        int at = player_audio_track();
        for (const Track &t : player_audio_tracks())
            if (t.id >= 0)
                e.push_back({ 1, t.id, t.name, t.id == at, "" });
    }
    if (all || panel_section == SEC_SUBS) {
        e.push_back({ 0, 0, "Subtitles", false, "" });
        e.push_back({ 7, SET_SUB_DELAY, "Subtitle delay", false, "" });
        e.push_back({ 7, SET_SUB_SIZE, "Size", false, "" });
        e.push_back({ 7, SET_SUB_COLOUR, "Colour", false, "" });
        e.push_back({ 7, SET_SUB_BOX, "Background box", false, "" });
        int st = player_subtitle_track();
        std::vector<Track> subs = player_subtitle_tracks();
        if (subs.empty())
            e.push_back({ 2, -1, "Off", true, "" });
        for (const Track &t : subs)
            e.push_back({ 2, t.id, t.id < 0 ? "Off" : t.name, t.id == st, "" });
        static std::vector<std::string> files;
        static std::string files_for;
        if (files_for != playing_path) {
            files = subtitle_files();
            files_for = playing_path;
        }
        for (const std::string &f : files)
            e.push_back({ 6, 0, trf("Load  %s", f.substr(f.rfind('/') + 1).c_str()), false, f });
        e.push_back({ 7, SET_SUB_PICK, "Add a subtitle file", false, "" });
        e.push_back({ 7, SET_OSUB, "Download subtitles", false, "" });
    }
    if (all || panel_section == SEC_SPEED) {
        e.push_back({ 0, 0, "Playback", false, "" });
        e.push_back({ 7, SET_REPEAT, "Loop this video", false, "" });
        e.push_back({ 7, SET_SHUFFLE, "Shuffle", false, "" });
        if (playing_disc()) {
            e.push_back({ 7, SET_DISC_MENU, "Disc menu", false, "" });
            if (player_disc_has_popup())
                e.push_back({ 7, SET_POPUP, "Pop-up menu", false, "" });
        }
        e.push_back({ 7, SET_SLEEP, "Sleep timer", false, "" });
        e.push_back({ 7, SET_LOOP, "A-B repeat", false, "" });
        e.push_back({ 7, SET_BOOKMARK, "Add a bookmark here", false, "" });
        std::vector<int64_t> bms = library_bookmarks(playing_path);
        for (int i = 0; i < (int)bms.size(); i++)
            e.push_back({ 9, i, trf("Bookmark  %s", format_time(bms[i]).c_str()), false, "" });
        if (!bms.empty())
            e.push_back({ 7, SET_BM_CLEAR, "Clear bookmarks", false, "" });
        float rate = player_rate();
        static const char *const speed_names[6] = { "Speed 0.5x", "Speed 0.75x", "Speed normal",
                                                    "Speed 1.25x", "Speed 1.5x", "Speed 2x" };
        for (int i = 0; i < 6; i++)
            e.push_back({ 3, i, speed_names[i], fabsf(rate - speeds[i]) < 0.01f, "" });
        std::vector<Chapter> ch = player_chapters();
        if (!ch.empty()) {
            e.push_back({ 0, 0, "Chapters", false, "" });
            int cur = player_chapter();
            for (int i = 0; i < (int)ch.size(); i++)
                e.push_back({ 8, i, format_time(ch[i].start_ms) + "   " + ch[i].name, i == cur, "" });
        }
    }
    if (all || panel_section == SEC_PICTURE) {
        e.push_back({ 0, 0, "Picture", false, "" });
        for (int i = 0; i < PIC_MODES; i++)
            e.push_back({ 4, i, aspect_names[i], aspect_mode == i, "" });
        e.push_back({ 7, SET_DEINT, "Deinterlace", false, "" });
        e.push_back({ 7, SET_BRIGHT, "Brightness", false, "" });
        e.push_back({ 7, SET_CONTRAST, "Contrast", false, "" });
        e.push_back({ 7, SET_SAT, "Saturation", false, "" });
        e.push_back({ 7, SET_GAMMA, "Gamma", false, "" });
        e.push_back({ 7, SET_HUE, "Hue", false, "" });
        e.push_back({ 7, SET_SHARPEN, "Sharpen", false, "" });
        e.push_back({ 7, SET_ROTATE, "Rotate", false, "" });
        e.push_back({ 7, SET_FLIP, "Flip", false, "" });
        e.push_back({ 7, SET_PIC_RESET, "Reset picture", false, "" });
    }

    std::vector<int> focusable;
    for (int i = 0; i < (int)e.size(); i++)
        if (e[i].kind)
            focusable.push_back(i);
    if (modal_focus >= (int)focusable.size())
        modal_focus = (int)focusable.size() - 1;
    /* round the list: up from the first is the last, down from the last the first */
    int nf = (int)focusable.size();
    if (hit(PAD_UP) && nf > 0)
        modal_focus = (modal_focus + nf - 1) % nf;
    if (hit(PAD_DOWN) && nf > 0)
        modal_focus = (modal_focus + 1) % nf;
    if (hit(PAD_CIRCLE) || hit(PAD_TRIANGLE)) {
        modal = NONE;
        return;
    }
    for (PanelEntry &p : e)
        if (p.kind == 7)
            p.shown = setting_value(p.value);
    bool on_setting = !focusable.empty() && e[focusable[modal_focus]].kind == 7;
    if (on_setting && (hit(PAD_LEFT) || hit(PAD_RIGHT)))
        setting_change(e[focusable[modal_focus]].value, hit(PAD_RIGHT) ? 1 : -1);
    bool on_volume = !focusable.empty() && e[focusable[modal_focus]].kind == 5;
    if (on_volume && (hit(PAD_LEFT) || hit(PAD_RIGHT)))
        player_set_volume(player_volume() + (hit(PAD_RIGHT) ? 5 : -5));
    if (hit(PAD_CROSS) && !focusable.empty()) {
        const PanelEntry &p = e[focusable[modal_focus]];
        if (p.kind == 1)
            player_set_audio_track(p.value);
        else if (p.kind == 2)
            player_set_subtitle_track(p.value);
        else if (p.kind == 3)
            player_set_rate(speeds[p.value]);
        else if (p.kind == 4)
            aspect_mode = p.value;
        else if (p.kind == 5)
            player_set_volume(player_volume() >= 100 ? 0 : 100); /* ✕: mute / back to 100 */
        else if (p.kind == 6 && player_add_subtitle(p.path))
            toast("Subtitles loaded", 1.5);
        else if (p.kind == 7)
            setting_change(p.value, 0);
        else if (p.kind == 8) {
            player_set_chapter(p.value);
            last_seek_at = now;
        } else if (p.kind == 9) {
            std::vector<int64_t> bms = library_bookmarks(playing_path);
            if (p.value < (int)bms.size()) {
                player_seek(bms[p.value]);
                last_seek_at = now;
            }
        }
    }

    float a = ease(modal_anim);
    float w = 600, x = 1920 - w * a;
    dl->AddRectFilledMultiColor(P(x - 200, 0), P(x, 1080), IM_COL32(0, 0, 0, 0), alpha(IM_COL32(0, 0, 0, 255), 0.6f * a),
                                alpha(IM_COL32(0, 0, 0, 255), 0.6f * a), IM_COL32(0, 0, 0, 0));
    static const char *const titles[] = { "Audio", "Subtitles", "Playback", "Picture" };
    if (classic_look) {
        /* a docked pane: graphite body, its own title band */
        vgrad(x, 0, w, 1080, IM_COL32(40, 43, 49, 255), IM_COL32(24, 26, 31, 255));
        rect(x, 0, 1.5f, 1080, IM_COL32(8, 9, 11, 255));
        rect(x + 1.5f, 0, 1, 1080, IM_COL32(255, 255, 255, 20));
        bevel(x + 2.5f, 0, w, 108, IM_COL32(58, 63, 72, 255), IM_COL32(36, 39, 45, 255));
        text(f_semi, 30, x + 48, 34, C_TEXT, all ? "Tracks" : titles[panel_section]);
    } else {
        rect(x, 0, w, 1080, IM_COL32(18, 18, 24, 255));
        rect(x, 0, 2, 1080, IM_COL32(255, 255, 255, 18));
        text(f_bold, 34, x + 48, 60, C_TEXT, all ? "Tracks" : titles[panel_section]);
    }
    /* Scroll so the focused line stays in view. */
    static float scroll;
    float fy = 0, y = 0;
    for (int i = 0; i < (int)e.size(); i++) {
        if (!focusable.empty() && i == focusable[modal_focus])
            fy = y;
        y += e[i].kind ? 64 : (i ? 86 : 56);
    }
    scroll = approach(scroll, std::max(0.0f, fy - 520), 12);
    dl->PushClipRect(P(x, 120), P(1920, 1000), true);
    y = 130 - scroll;
    for (int i = 0; i < (int)e.size(); i++) {
        const PanelEntry &p = e[i];
        if (!p.kind) {
            if (i)
                y += 30;
            float iy = y + 16;
            if (p.label == "Audio")
                icon_speaker(x + 62, iy, 26, C_ORANGE);
            else if (p.label == "Subtitles")
                icon_cc(x + 62, iy, 28, C_ORANGE);
            else if (p.label == "Playback" || p.label == "Chapters")
                icon_speed(x + 62, iy, 28, C_ORANGE);
            else
                icon_picture(x + 62, iy, 28, C_ORANGE);
            if (classic_look) {
                text(f_semi, 23, x + 96, y + 3, IM_COL32(150, 186, 226, 255), p.label.c_str());
                float lx = x + 96 + text_size(f_semi, 23, p.label.c_str()).x + 14;
                rect(lx, y + 18, x + w - 40 - lx, 1, IM_COL32(90, 110, 135, 255));
            } else {
                text(f_bold, 24, x + 96, y + 2, C_ORANGE, p.label.c_str());
            }
            y += 56;
            continue;
        }
        bool foc = !focusable.empty() && i == focusable[modal_focus];
        float f = focus_anim(hash_id(p.kind == 5 ? "volume" : p.label, 600 + p.kind * 100 + p.value), foc);
        if (p.kind == 7) {
            list_row(x + 28, y, w - 56, 56, f, false);
            text(f_semi, 23, x + 72, y + 14, mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), p.label.c_str());
            float vx = x + w - 72;
            if (f > 0.05f) {
                /* ‹ value ›: ←/→ change it */
                tri(vx + 4, y + 20, vx + 4, y + 36, vx + 14, y + 28, alpha(C_ORANGE, f));
                vx -= 22;
            }
            text(f_semi, 23, vx, y + 14, p.shown == "Off" || p.shown == "0 ms" || p.shown == "0" ? C_DIM : C_TEXT,
                 p.shown.c_str(), 1, w - 300);
            y += 64;
            continue;
        }
        list_row(x + 28, y, w - 56, 56, f, false);
        if (p.kind == 5) {
            /* Volume: a slider, ←/→ to move it (VLC goes to 200%). */
            float v = player_volume() / 200.0f, bx = x + 270, bw = w - 270 - 72;
            text(f_semi, 23, x + 72, y + 14, mix(C_DIM, C_TEXT, 0.6f + 0.4f * f), p.label.c_str());
            rect(bx, y + 25, bw, 6, IM_COL32(255, 255, 255, 50), 3);
            rect(bx, y + 25, bw * v, 6, C_ORANGE, 3);
            rect(bx + bw * 0.5f - 1, y + 19, 2, 18, IM_COL32(255, 255, 255, 70)); /* 100% */
            circle(bx + bw * v, y + 28, 8 + 4 * f, C_TEXT);
            y += 64;
            continue;
        }
        text(f_semi, 23, x + 72, y + 14, p.active ? C_TEXT : mix(C_DIM, C_TEXT, f), p.label.c_str(), 0, w - 190);
        if (p.active)
            icon_check(x + w - 76, y + 28, 24, C_ORANGE);
        y += 64;
    }
    dl->PopClipRect();
    bool setting_focused = !focusable.empty() && e[focusable[modal_focus]].kind == 7;
    if (setting_focused) {
        int id = e[focusable[modal_focus]].value;
        const char *act = id == SET_LOOP ? (loop_a < 0 ? "Mark A" : loop_b < 0 ? "Mark B" : "Clear")
                        : id == SET_SUB_BOX || id == SET_NIGHT || id == SET_SOUND_OUT || id == SET_SHUFFLE ? "Switch"
                        : id == SET_REPEAT ? "Switch" : id == SET_BOOKMARK ? "Add"
                        : id == SET_BM_CLEAR ? "Clear" : id == SET_DISC_MENU || id == SET_POPUP || id == SET_SUB_PICK ? "Open" : "Reset";
        hints(1920 - 48, 1040, { { PAD_CROSS, act }, { PAD_CIRCLE, "Close" } }, a);
    }
    else
        hints(1920 - 48, 1040, { { PAD_CROSS, "Select" }, { PAD_CIRCLE, "Close" } }, a);
}

/* ---- the player ------------------------------------------------------------------------ */

void compute_video_rect()
{
    const VideoInfo &v = player_video_info();
    float sw = (float)gfx.width, sh = (float)gfx.height;
    if (v.spherical) {
        /* a 360° video: the view fills the screen */
        vid_rect[0] = 0;
        vid_rect[1] = 0;
        vid_rect[2] = sw;
        vid_rect[3] = sh;
        return;
    }
    float aspect = v.aspect > 0 ? v.aspect : 16.0f / 9;
    if (aspect_mode == PIC_16_9)
        aspect = 16.0f / 9;
    else if (aspect_mode == PIC_4_3)
        aspect = 4.0f / 3;
    else if (aspect_mode == PIC_21_9)
        aspect = 2.39f;
    if (player_rotation() & 1)
        aspect = 1 / aspect; /* turned on its side */
    float w = sw, h = sh; /* Stretch: the whole screen */
    bool wider = aspect > sw / sh;
    if (aspect_mode != PIC_STRETCH) {
        /* Fit (and the forced shapes, and zoom): bars on the short side.
         * Fill: crop the long side. */
        bool by_width = (aspect_mode != PIC_FILL) == wider;
        if (by_width) {
            w = sw;
            h = sw / aspect;
        } else {
            h = sh;
            w = sh * aspect;
        }
    }
    if (aspect_mode == PIC_ZOOM) {
        w *= 1.25f;
        h *= 1.25f;
    }
    vid_rect[0] = (sw - w) / 2;
    vid_rect[1] = (sh - h) / 2;
    vid_rect[2] = w;
    vid_rect[3] = h;
}

void spinner(float cx, float cy, float r)
{
    float a = (float)fmod(now * 5.2, 2 * PI_F);
    dl->PathArcTo(P(cx, cy), r * S, a, a + PI_F * 1.4f, 40);
    dl->PathStroke(C_ORANGE, 0, r * 0.14f * S);
}

/* The file has ended: its last picture stays (dimmed) with Watch again in
 * the centre. ✕ plays it again, ○ goes back, L1/R1 the previous / next file. */
void end_screen()
{
    float k = ease((float)std::min(1.0, (now - ended_at) * 4));
    rect(0, 0, 1920, 1080, alpha(IM_COL32(0, 0, 0, 255), 0.6f * k));
    if (modal == NONE) {
        if (hit(PAD_CROSS)) {
            int item = current_item();
            if (item >= 0) {
                restart_playing(item);
                return;
            }
            if (is_link(playing_path)) {
                std::string url = playing_path;
                std::vector<std::string> opts = stream_options;
                std::string name = stream_name;
                player_stop();
                start_stream(url, opts, stream_remember, name, net_take_media(url));
                return;
            }
        }
        if (hit(PAD_CIRCLE)) {
            leave_player(true);
            return;
        }
        if (hit(PAD_L1) || hit(PAD_R1)) {
            play_neighbour(hit(PAD_R1) ? 1 : -1);
            return;
        }
    }
    if (classic_look) {
        /* A small dialog: what finished, and Watch again (focused). */
        float w = 760, h = 340, x = 960 - w / 2, y = 540 - h / 2 + (1 - k) * 30;
        panel_band = 72;
        panel(x, y, w, h, k);
        cone(x + 26, y + 20, 32, k);
        text(f_semi, 26, x + 72, y + 20, alpha(C_TEXT, k), "Finished", 0);
        text(f_reg, 24, x + 40, y + 98, alpha(C_TEXT, k), playing_name.c_str(), 0, w - 80);
        text(f_reg, 20, x + 40, y + 136, alpha(C_DIM, k), "The end of the file was reached.", 0, w - 80);
        /* the button fits its icon and words, both centred in it */
        const float icon_w = 30, gap = 14, pad = 36;
        float tw = text_size(f_semi, 25, "Watch again").x;
        float bw = pad * 2 + icon_w + gap + tw, bh = 60, bx = 960 - bw / 2, by2 = y + h - bh - 36;
        vgrad(bx, by2, bw, bh, alpha(IM_COL32(72, 77, 87, 255), k), alpha(IM_COL32(44, 48, 55, 255), k));
        stroke(bx, by2, bw, bh, alpha(IM_COL32(10, 11, 13, 255), k), 4, 1);
        sel_box(bx, by2, bw, bh, k);
        icon_jump(bx + pad + icon_w / 2, by2 + bh / 2, icon_w, alpha(C_TEXT, k), false, "");
        text(f_semi, 25, bx + pad + icon_w + gap, by2 + bh / 2 - 15, alpha(C_TEXT, k), "Watch again");
        if (modal == NONE)
            hints(1824, 1030, { { PAD_CROSS, "Watch again" }, { PAD_CIRCLE, "Back" } }, k);
        return;
    }
    dl->AddRectFilledMultiColor(P(0, 0), P(1920, 240), alpha(IM_COL32(0, 0, 0, 255), 0.7f * k),
                                alpha(IM_COL32(0, 0, 0, 255), 0.7f * k), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    text(f_bold, 40, 96, 60, alpha(C_TEXT, k), playing_name.c_str(), 0, 1500);
    text(f_reg, 23, 96, 116, alpha(C_DIM, k), "Finished", 0);
    /* Watch again: always the focused button, white disc and dark arrow. */
    float bx = 960, by = 520;
    glow(bx - 90, by - 90, 180, 180, 90, alpha(C_ORANGE, 0.6f * k), 22, 8);
    circle(bx, by, 90, alpha(IM_COL32(255, 255, 255, 255), k));
    icon_jump(bx, by, 96, alpha(C_BG, k), false, "");
    text(f_bold, 28, bx, by + 120, alpha(C_TEXT, k), "Watch again", 0.5f);
    if (modal == NONE)
        hints(1824, 1030, { { PAD_CROSS, "Watch again" }, { PAD_CIRCLE, "Back" } }, k);
}

/* □: one picture on per press; held, it keeps going (about 12 a second,
 * after 0.4 s), like slow motion one frame at a time. */
bool frame_step_due()
{
    static double down_at = -1, next_at;
    if (!(held & PAD_SQUARE)) {
        down_at = -1;
        return false;
    }
    if (hit(PAD_SQUARE)) {
        down_at = now;
        next_at = now + 0.4;
        return true;
    }
    if (down_at >= 0 && now >= next_at) {
        next_at = now + 1.0 / 12;
        return true;
    }
    return false;
}

/* Spectrum bars under the cover. Classic: LED segments, green to orange, with
 * falling peaks, as the desktop players had; Modern: soft orange bars. */
void visualizer(float cx, float top, float w, float h)
{
    const int N = 32;
    static float lvl[N], peak[N];
    float bands[N];
    player_spectrum(bands, N);
    bool paused = player_paused();
    float bw = w / N, x0 = cx - w / 2;
    for (int i = 0; i < N; i++) {
        float v = paused ? 0 : bands[i];
        lvl[i] = v > lvl[i] ? v : std::max(v, lvl[i] - dt * 2.2f);
        peak[i] = lvl[i] > peak[i] ? lvl[i] : std::max(0.0f, peak[i] - dt * 0.6f);
        float x = x0 + i * bw + 2, bwi = bw - 4;
        if (classic_look) {
            const int segs = 12;
            float sh = h / segs;
            int lit = (int)(lvl[i] * segs + 0.5f);
            for (int k = 0; k < segs; k++) {
                float y = top + h - (k + 1) * sh;
                ImU32 c = k < 7 ? IM_COL32(70, 200, 90, 255) : k < 10 ? IM_COL32(230, 200, 60, 255) : C_ORANGE;
                rect(x, y + 1, bwi, sh - 2, k < lit ? c : IM_COL32(255, 255, 255, 12));
            }
            int pk = (int)(peak[i] * segs);
            if (pk > 0 && pk < segs)
                rect(x, top + h - (pk + 1) * sh + 1, bwi, sh - 2, IM_COL32(255, 255, 255, 200));
        } else {
            float bh = std::max(3.0f, lvl[i] * h);
            dl->AddRectFilledMultiColor(P(x, top + h - bh), P(x + bwi, top + h), alpha(C_ORANGE, 0.95f), alpha(C_ORANGE, 0.95f),
                                        alpha(C_ORANGE2, 0.5f), alpha(C_ORANGE2, 0.5f));
            rect(x, top + h - peak[i] * h - 3, bwi, 2, IM_COL32(255, 255, 255, 150));
        }
    }
}

/* Classic: a desktop media player's controls. A title strip at the top; at the
 * bottom a graphite transport panel with a sunken seek groove, metal buttons
 * and an orange play orb in the middle. */
void player_overlay_classic(float a, int64_t t, int64_t len, bool paused, const std::string &meta)
{
    /* top: title strip */
    float ty = -(1 - a) * 30;
    vgrad(0, ty, 1920, 112, alpha(IM_COL32(44, 48, 55, 255), 0.92f * a), alpha(IM_COL32(24, 26, 31, 255), 0.92f * a));
    rect(0, ty + 111, 1920, 1, alpha(C_SHADE, a));
    cone(96, ty + 30, 44, a);
    text(f_semi, 30, 158, ty + 22, alpha(C_TEXT, a), playing_name.c_str(), 0, 1380);
    text(f_reg, 20, 158, ty + 64, alpha(C_DIM, a), meta.c_str(), 0, 1380);
    time_t tt = time(nullptr);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text(f_reg, 28, 1824, ty + 22, alpha(C_TEXT, a), clock, 1);
    if (len > 0 && !paused) {
        time_t ends = tt + (time_t)((len - t) / 1000 / std::max(0.25f, player_rate()));
        localtime_r(&ends, &tmv);
        char e[32];
        snprintf(e, sizeof(e), tr("Ends at %02d:%02d"), tmv.tm_hour, tmv.tm_min);
        text(f_reg, 19, 1824, ty + 64, alpha(C_DIM, a), e, 1);
    }

    /* bottom: the transport panel */
    float py = 868 + (1 - a) * 40;
    vgrad(0, py, 1920, 1080 - py, alpha(IM_COL32(50, 54, 62, 255), 0.95f * a), alpha(IM_COL32(22, 24, 28, 255), 0.95f * a));
    rect(0, py, 1920, 1, alpha(IM_COL32(90, 96, 106, 255), a));
    /* the seek groove */
    float gx = 96, gw = 1728, gy = py + 34, gh = 12;
    float fs = focus_anim(700, osd_row == 0 && modal == NONE);
    rect(gx, gy, gw, gh, alpha(IM_COL32(12, 13, 16, 255), a), 3);
    rect(gx + 1, gy, gw - 2, 1, alpha(IM_COL32(0, 0, 0, 255), a));
    float frac = len > 0 ? (float)t / len : 0, sfrac = len > 0 ? (float)scrub_ms / len : 0;
    float shown = scrubbing ? sfrac : frac;
    if (frac > 0)
        vgrad(gx + 1, gy + 2, (gw - 2) * frac, gh - 4, alpha(IM_COL32(255, 176, 70, 255), a), alpha(C_ORANGE2, a));
    if (scrubbing)
        rect(gx + gw * std::min(frac, sfrac), gy + 2, gw * fabsf(sfrac - frac), gh - 4, alpha(IM_COL32(255, 255, 255, 90), a));
    stroke(gx, gy, gw, gh, alpha(fs > 0.5f ? C_ORANGE : C_LINE, a), 3, 1);
    if (len > 0) {
        for (const Chapter &c : player_chapters())
            if (c.start_ms > 0)
                rect(gx + gw * c.start_ms / len - 1, gy - 3, 2, gh + 6, alpha(IM_COL32(220, 224, 230, 255), a));
        for (int64_t bm : library_bookmarks(playing_path)) {
            float bx = gx + gw * bm / len;
            tri(bx - 7, gy - 12, bx + 7, gy - 12, bx, gy - 2, alpha(C_TEXT, a));
        }
        if (loop_a >= 0)
            rect(gx + gw * loop_a / len - 2, gy - 8, 4, gh + 16, alpha(C_ORANGE, a));
        if (loop_b >= 0)
            rect(gx + gw * loop_b / len - 2, gy - 8, 4, gh + 16, alpha(C_ORANGE, a));
    }
    /* the slider's handle: a little metal block */
    float kx = gx + gw * shown, kw = 16 + 4 * fs, kh = 30 + 4 * fs;
    vgrad(kx - kw / 2, gy + gh / 2 - kh / 2, kw, kh, alpha(IM_COL32(236, 238, 242, 255), a), alpha(IM_COL32(150, 156, 166, 255), a));
    stroke(kx - kw / 2, gy + gh / 2 - kh / 2, kw, kh, alpha(fs > 0.5f ? C_ORANGE : IM_COL32(40, 43, 49, 255), a), 3, 1.5f);
    if (scrubbing) {
        std::string st = format_time(scrub_ms);
        float tw = text_size(f_semi, 24, st.c_str()).x + 26;
        vgrad(kx - tw / 2, gy - 60, tw, 40, alpha(IM_COL32(70, 75, 84, 255), a), alpha(IM_COL32(44, 48, 55, 255), a));
        stroke(kx - tw / 2, gy - 60, tw, 40, alpha(C_ORANGE, a), 4, 1.5f);
        text(f_semi, 24, kx, gy - 54, alpha(C_TEXT, a), st.c_str(), 0.5f);
    }
    text(f_reg, 22, gx, gy + 22, alpha(C_TEXT, a), format_time(scrubbing ? scrub_ms : t).c_str());
    if (len > 0)
        text(f_reg, 22, gx + gw, gy + 22, alpha(C_DIM, a), ("-" + format_time(len - (scrubbing ? scrub_ms : t))).c_str(), 1);

    /* the buttons (the same order as Modern's) */
    float cy = py + 146;
    const float xs[10] = { 960 - 270, 960 - 150, 960, 960 + 150, 960 + 270, 960 + 380,
                           1824 - 330, 1824 - 230, 1824 - 130, 1824 - 30 };
    int repeat_mode = loop_on() ? 2 : 0;
    for (int i = 0; i < 10; i++) {
        bool foc = osd_row == 1 && osd_col == i && modal == NONE;
        float f = focus_anim(800 + i, foc, 16);
        float x = xs[i];
        ImU32 ic = alpha(C_TEXT, a);
        if (i == 2) {
            /* the play orb */
            circle(x, cy, 44, alpha(IM_COL32(16, 17, 20, 255), a));
            circle(x, cy, 40, alpha(mix(IM_COL32(214, 96, 0, 255), IM_COL32(255, 128, 10, 255), f), a));
            radial(x, cy - 14, 30, alpha(IM_COL32(255, 220, 160, 255), 0.55f * a), 8, 48);
            ring(x, cy, 40, alpha(IM_COL32(255, 200, 120, 255), (0.35f + 0.65f * f) * a), 1.5f + f);
            if (paused)
                icon_play(x + 4, cy, 30, alpha(IM_COL32(255, 255, 255, 255), a));
            else
                icon_pause(x, cy, 26, alpha(IM_COL32(255, 255, 255, 255), a));
            continue;
        }
        float bw = i < 6 ? 76 : 70, bh = 56;
        vgrad(x - bw / 2, cy - bh / 2, bw, bh, alpha(IM_COL32(72, 77, 87, 255), a), alpha(IM_COL32(40, 43, 49, 255), a));
        stroke(x - bw / 2, cy - bh / 2, bw, bh, alpha(IM_COL32(10, 11, 13, 255), a), 4, 1);
        rect(x - bw / 2 + 2, cy - bh / 2 + 1, bw - 4, 1, alpha(IM_COL32(255, 255, 255, 34), a));
        sel_box(x - bw / 2, cy - bh / 2, bw, bh, f * a);
        float sz = 30;
        switch (i) {
        case 0: icon_skip(x, cy, sz * 0.8f, ic, false); break;
        case 1: icon_jump(x, cy, sz * 1.05f, ic, false, "10"); break;
        case 3: icon_jump(x, cy, sz * 1.05f, ic, true, "10"); break;
        case 4: icon_skip(x, cy, sz * 0.8f, ic, true); break;
        case 5: icon_repeat(x, cy, sz, repeat_mode ? alpha(C_ORANGE, a) : ic, repeat_mode); break;
        case 6: icon_speaker(x, cy, sz, ic); break;
        case 7: icon_cc(x, cy, sz, ic); break;
        case 8: icon_speed(x, cy, sz, ic); break;
        case 9: icon_picture(x, cy, sz, ic); break;
        }
    }
    static const char *const tips[10] = { "Previous", "Back 10 s", "Play / Pause", "Forward 10 s",
                                          "Next", "Loop", "Audio & volume", "Subtitles", "Playback", "Picture" };
    if (osd_row == 1 && modal == NONE && osd_col != 2) /* the big play button needs no name */
        text(f_reg, 19, xs[osd_col], cy + 32, alpha(C_DIM, a), tips[osd_col], 0.5f); /* under it: clear of the times */
    if (paused)
        hints(560, cy, { { PAD_SQUARE, "Next frame (hold: keep going)" }, { PAD_CIRCLE, "Back" } }, a * 0.9f);
    else
        hints(560, cy, { { PAD_TRIANGLE, "Tracks" }, { PAD_CIRCLE, "Back" } }, a * 0.9f);
}

/* ---- the photo viewer --------------------------------------------------------------
 * The photos of a list (a folder, the Pictures row) one at a time, fitted to
 * the screen, crossfading. ←/→ (or L1/R1) the previous / next one, ✕ starts or
 * stops a slideshow, ○ back. The name and "3 of 12" come up with any press. */
std::vector<int> viewer_list;
int viewer_pos;
ImTextureID viewer_tex, viewer_prev;
float viewer_aspect = 1, viewer_prev_aspect = 1, viewer_fade = 1;
std::string viewer_shown;   /* the path of viewer_tex */
bool viewer_failed, slideshow;
double viewer_next_at, viewer_osd_until;
const double SLIDE_SECONDS = 5;

/* ---- the text viewer: .txt, .nfo and friends -------------------------------------
 * An .nfo is DOS text (code page 437: its box drawing and shades make the
 * art); other files are UTF-8, or Windows-1252 when they aren't. Long lines
 * of plain text wrap; .nfo art never does. ↑/↓ scroll (held: keep going),
 * L2/R2 a page, ○ close. */
std::string text_title;
std::vector<std::string> text_lines;
bool text_art;     /* an .nfo: lines touch, so the art joins up */
int text_top;      /* first line shown */
float text_scroll;

/* UTF-8 of the 128 upper characters of code page 437 */
const char *const cp437_high[128] = {
    "\u00C7", "\u00FC", "\u00E9", "\u00E2", "\u00E4", "\u00E0", "\u00E5", "\u00E7", "\u00EA", "\u00EB", "\u00E8", "\u00EF", "\u00EE", "\u00EC", "\u00C4", "\u00C5",
    "\u00C9", "\u00E6", "\u00C6", "\u00F4", "\u00F6", "\u00F2", "\u00FB", "\u00F9", "\u00FF", "\u00D6", "\u00DC", "\u00A2", "\u00A3", "\u00A5", "\u20A7", "\u0192",
    "\u00E1", "\u00ED", "\u00F3", "\u00FA", "\u00F1", "\u00D1", "\u00AA", "\u00BA", "\u00BF", "\u2310", "\u00AC", "\u00BD", "\u00BC", "\u00A1", "\u00AB", "\u00BB",
    "\u2591", "\u2592", "\u2593", "\u2502", "\u2524", "\u2561", "\u2562", "\u2556", "\u2555", "\u2563", "\u2551", "\u2557", "\u255D", "\u255C", "\u255B", "\u2510",
    "\u2514", "\u2534", "\u252C", "\u251C", "\u2500", "\u253C", "\u255E", "\u255F", "\u255A", "\u2554", "\u2569", "\u2566", "\u2560", "\u2550", "\u256C", "\u2567",
    "\u2568", "\u2564", "\u2565", "\u2559", "\u2558", "\u2552", "\u2553", "\u256B", "\u256A", "\u2518", "\u250C", "\u2588", "\u2584", "\u258C", "\u2590", "\u2580",
    "\u03B1", "\u00DF", "\u0393", "\u03C0", "\u03A3", "\u03C3", "\u00B5", "\u03C4", "\u03A6", "\u0398", "\u03A9", "\u03B4", "\u221E", "\u03C6", "\u03B5", "\u2229",
    "\u2261", "\u00B1", "\u2265", "\u2264", "\u2320", "\u2321", "\u00F7", "\u2248", "\u00B0", "\u2219", "\u00B7", "\u221A", "\u207F", "\u00B2", "\u25A0", "\u00A0",
};

bool valid_utf8(const std::string &t)
{
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= t.size() + (n ? 0 : 1))
            return false;
        for (int k = 1; k <= n; k++)
            if (((unsigned char)t[i + k] & 0xC0) != 0x80)
                return false;
        i += n + 1;
    }
    return true;
}

void put_utf8_cp(std::string &out, unsigned c)
{
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | c >> 6);
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xE0 | c >> 12);
        out += (char)(0x80 | (c >> 6 & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

void open_text(const MediaItem &m)
{
    std::string raw;
    if (FILE *f = fopen(m.path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && raw.size() < (2u << 20))
            raw.append(buf, n);
        fclose(f);
    } else {
        toast(trf("Couldn't open %s", m.name.c_str()), 3);
        return;
    }
    bool nfo = m.ext == "NFO" || m.ext == "DIZ";
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        raw.erase(0, 3);
    std::string text;
    if (!valid_utf8(raw)) {
        for (unsigned char c : raw) {
            if (c < 0x80)
                text += (char)c;
            else if (nfo)
                text += cp437_high[c - 0x80];
            else
                put_utf8_cp(text, c); /* Latin-1 / Windows-1252 letters */
        }
    } else {
        text = raw;
    }
    /* lines; tabs as 4 spaces; plain text wraps at about 110 columns */
    text_lines.clear();
    size_t at = 0;
    const size_t wrap = 110;
    while (at <= text.size()) {
        size_t eol = text.find('\n', at);
        std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string clean;
        for (char c : line)
            clean += c == '\t' ? std::string("    ") : std::string(1, c);
        if (!nfo) {
            while (clean.size() > wrap) {
                size_t cut = clean.rfind(' ', wrap);
                if (cut == std::string::npos || cut < wrap / 2)
                    cut = wrap;
                while (cut > 0 && ((unsigned char)clean[cut] & 0xC0) == 0x80)
                    cut--; /* whole characters */
                text_lines.push_back(clean.substr(0, cut));
                clean = clean.substr(cut + (clean[cut] == ' ' ? 1 : 0));
            }
        }
        text_lines.push_back(clean);
        if (eol == std::string::npos)
            break;
        at = eol + 1;
    }
    while (!text_lines.empty() && text_lines.back().empty())
        text_lines.pop_back();
    std::string ext = m.ext;
    for (char &c : ext)
        c = (char)tolower((unsigned char)c);
    text_title = m.name + "." + ext;
    text_art = nfo;
    text_top = 0;
    text_scroll = 0;
    modal = TEXT_VIEW;
    modal_anim = 0;
}

void modal_text_view()
{
    const float x = 160, w = 1600, top = 150, bottom = 990, lh = text_art ? 26 : 30;
    int visible = (int)((bottom - top - 40) / lh);
    int n = (int)text_lines.size(), last = std::max(0, n - visible);
    if (hit(PAD_DOWN))
        text_top = std::min(last, text_top + 1);
    if (hit(PAD_UP))
        text_top = std::max(0, text_top - 1);
    if (hit(PAD_R2) || hit(PAD_RIGHT))
        text_top = std::min(last, text_top + visible - 2);
    if (hit(PAD_L2) || hit(PAD_LEFT))
        text_top = std::max(0, text_top - (visible - 2));
    if (hit(PAD_CIRCLE) || hit(PAD_CROSS)) {
        modal = NONE;
        text_lines.clear();
        return;
    }
    text_scroll = approach(text_scroll, (float)text_top, 18);
    float a = ease(modal_anim);
    screen_backdrop(a);
    text(f_bold, 36, x, classic_look ? 32 : 56, alpha(C_TEXT, a), text_title.c_str(), 0, w - 300);
    char where[48];
    snprintf(where, sizeof(where), tr("%d lines"), n);
    text(f_reg, 22, x + w, classic_look ? 44 : 68, alpha(C_DIM, a), where, 1);
    /* the page: a sunken sheet in Classic, a dark card in Modern */
    if (classic_look) {
        inset(x - 20, top, w + 40, bottom - top);
    } else {
        rect(x - 20, top, w + 40, bottom - top, alpha(IM_COL32(20, 20, 26, 255), a), 16);
    }
    dl->PushClipRect(P(x - 20, top + 4), P(x + w + 20, bottom - 4), true);
    int first = std::max(0, (int)text_scroll - 1);
    for (int i = first; i < n && i < first + visible + 3; i++) {
        float y = top + 20 + (i - text_scroll) * lh;
        text(f_mono, 22, x, y, alpha(C_TEXT, a), text_lines[i].c_str());
    }
    dl->PopClipRect();
    /* where in the file: a thin scroll bar */
    if (n > visible) {
        float bh = (bottom - top - 20) * visible / n, by = top + 10 + (bottom - top - 20 - bh) * text_scroll / std::max(1, last);
        rect(x + w + 8, by, 6, bh, alpha(C_ORANGE, 0.8f * a), 3);
    }
    hints(1824, classic_look ? 1042 : 1040, { { PAD_UP, "Scroll" }, { PAD_R2, "Next page" }, { PAD_CIRCLE, "Close" } }, a);
}

void open_viewer(int item, const std::vector<int> &list)
{
    viewer_list.clear();
    for (int i : list)
        if (L()[i].image)
            viewer_list.push_back(i);
    if (viewer_list.empty())
        viewer_list.push_back(item);
    viewer_pos = (int)(std::find(viewer_list.begin(), viewer_list.end(), item) - viewer_list.begin());
    if (viewer_pos >= (int)viewer_list.size())
        viewer_pos = 0;
    slideshow = false;
    viewer_failed = false;
    viewer_osd_until = now + 3;
    screen = VIEWER;
    screen_fade = 0;
}

void leave_viewer()
{
    library_free_image(viewer_prev);
    library_free_image(viewer_tex);
    viewer_prev = viewer_tex = 0;
    viewer_shown.clear();
    slideshow = false;
    screen = LIBRARY;
    screen_fade = 0;
}

void viewer_step(int dir)
{
    int n = (int)viewer_list.size();
    viewer_pos = (viewer_pos + dir + n) % n;
    viewer_failed = false;
    viewer_next_at = now + SLIDE_SECONDS;
}

void viewer_draw(ImTextureID tex, float aspect, float alpha_k)
{
    if (!tex || alpha_k < 0.01f)
        return;
    float w = 1920, h = 1920 / aspect;
    if (h > 1080) {
        h = 1080;
        w = 1080 * aspect;
    }
    dl->AddImage(tex, P(960 - w / 2, 540 - h / 2), P(960 + w / 2, 540 + h / 2), ImVec2(0, 0), ImVec2(1, 1),
                 alpha(IM_COL32_WHITE, alpha_k));
}

void viewer_screen()
{
    rect(0, 0, 1920, 1080, IM_COL32(0, 0, 0, 255));
    if (viewer_list.empty() || viewer_pos >= (int)viewer_list.size()) {
        leave_viewer();
        return;
    }
    const MediaItem &m = L()[viewer_list[viewer_pos]];
    /* input */
    if (modal == NONE) {
        if (pressed)
            viewer_osd_until = now + 3;
        if (hit(PAD_CIRCLE)) {
            leave_viewer();
            return;
        }
        if (hit(PAD_RIGHT) || hit(PAD_R1))
            viewer_step(1);
        if (hit(PAD_LEFT) || hit(PAD_L1))
            viewer_step(-1);
        if (hit(PAD_CROSS)) {
            slideshow = !slideshow;
            viewer_next_at = now + SLIDE_SECONDS;
            toast(slideshow ? "Slideshow: a new photo every 5 seconds" : "Slideshow stopped", 2);
        }
    }
    if (slideshow && now >= viewer_next_at && viewer_shown == m.path)
        viewer_step(1);
    /* the photo wanted: decoded in the background, then it fades in */
    const MediaItem &want = L()[viewer_list[viewer_pos]];
    if (viewer_shown != want.path && !viewer_failed) {
        float asp = 1;
        bool failed = false;
        ImTextureID t = library_photo(want.path, &asp, &failed);
        if (failed) {
            viewer_failed = true;
            toast(trf("Couldn't open %s", want.name.c_str()), 3);
        } else if (t) {
            library_free_image(viewer_prev);
            viewer_prev = viewer_tex;
            viewer_prev_aspect = viewer_aspect;
            viewer_tex = t;
            viewer_aspect = asp > 0 ? asp : 1;
            viewer_shown = want.path;
            viewer_fade = 0;
            viewer_next_at = now + SLIDE_SECONDS;
        }
    }
    viewer_fade = approach(viewer_fade, 1, 7);
    viewer_draw(viewer_prev, viewer_prev_aspect, 1 - viewer_fade);
    viewer_draw(viewer_tex, viewer_aspect, viewer_fade);
    if (viewer_shown != want.path && !viewer_failed)
        spinner(960, 540, 30);
    if (viewer_failed)
        text(f_reg, 24, 960, 530, C_DIM, "This picture can't be opened", 0.5f);

    /* the strip: name, position, size */
    static float oa;
    /* up while the controller is used, then out of the photo's way (in a
     * slideshow too) */
    oa = approach(oa, now < viewer_osd_until ? 1.0f : 0.0f, now < viewer_osd_until ? 12 : 5);
    if (oa < 0.01f)
        return;
    char pos[48];
    snprintf(pos, sizeof(pos), tr("%d of %d"), viewer_pos + 1, (int)viewer_list.size());
    std::string info = std::string(pos) + "  \xC2\xB7  " + want.ext;
    if (want.size > 0)
        info += "  \xC2\xB7  " + size_text(want.size);
    if (slideshow)
        info += std::string("  \xC2\xB7  ") + tr("Slideshow");
    if (classic_look) {
        float ty = -(1 - oa) * 30;
        vgrad(0, ty, 1920, 112, alpha(IM_COL32(44, 48, 55, 255), 0.92f * oa), alpha(IM_COL32(24, 26, 31, 255), 0.92f * oa));
        rect(0, ty + 111, 1920, 1, alpha(C_SHADE, oa));
        cone(96, ty + 30, 44, oa);
        text(f_semi, 30, 158, ty + 22, alpha(C_TEXT, oa), want.name.c_str(), 0, 1400);
        text(f_reg, 20, 158, ty + 64, alpha(C_DIM, oa), info.c_str(), 0, 1400);
        bevel(0, 1004 + (1 - oa) * 30, 1920, 76, alpha(IM_COL32(44, 48, 55, 255), 0.92f * oa), alpha(IM_COL32(26, 28, 33, 255), 0.92f * oa));
    } else {
        dl->AddRectFilledMultiColor(P(0, 0), P(1920, 220), alpha(IM_COL32(0, 0, 0, 255), 0.8f * oa),
                                    alpha(IM_COL32(0, 0, 0, 255), 0.8f * oa), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        text(f_bold, 36, 96, 52, alpha(C_TEXT, oa), want.name.c_str(), 0, 1400);
        text(f_reg, 21, 96, 104, alpha(C_DIM, oa), info.c_str());
        dl->AddRectFilledMultiColor(P(0, 900), P(1920, 1080), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                    alpha(IM_COL32(0, 0, 0, 255), 0.8f * oa), alpha(IM_COL32(0, 0, 0, 255), 0.8f * oa));
    }
    hints(1824, classic_look ? 1042 : 1030,
          { { PAD_LEFT, "Previous" }, { PAD_RIGHT, "Next" }, { PAD_CROSS, slideshow ? "Stop slideshow" : "Slideshow" },
            { PAD_CIRCLE, "Back" } }, oa);
}

void player_screen()
{
    compute_video_rect();
    show_video = true;
    bool active = player_active();
    if (active)
        was_active = true;
    if (player_ended() || (was_active && !active && !player_buffering())) {
        bool failed = player_failed();
        /* Ended without ever showing a picture or knowing its length: it
         * never played (a link VLC can't read), not "Finished". */
        if (!failed && !ended && player_length() <= 0 && !video_has_picture() && is_link(playing_path))
            failed = true;
        if (failed)
            toast(!is_link(playing_path) ? trf("Couldn't play %s", playing_name.c_str())
                  : !stream_name.empty() ? trf("%s isn't available right now (offline or blocked here)", stream_name.c_str())
                                          : std::string(tr("Couldn't play this link")), 4);
        if (!failed && sleep_mode == SLEEP_END) {
            sleep_mode = SLEEP_OFF;
            toast("Sleep timer: stopped at the end of the file", 4);
            leave_player(true);
            return;
        }
        if (failed) {
            leave_player(false);
            return;
        }
        if (!ended) {
            library_set_resume(playing_path, 0, player_length());
            int cur = current_item();
            if (loop_on() && cur >= 0) {
                /* Loop this video. */
                restart_playing(cur);
                return;
            }
            /* Music and playlists go on to the next file by themselves. */
            int next = neighbour_pos(1);
            if (next >= 0 && (from_playlist || (cur >= 0 && L()[cur].audio))) {
                int item = playlist[next];
                player_stop();
                start_playback(item, 0, playlist);
                return;
            }
            ended = true;
            ended_at = now;
        }
        end_screen();
        return;
    }
    switch (player_tick()) {
    case PLAYER_EVENT_WENT_FAST:
        toast("Too heavy to decode in full: switched to fast decoding", 4);
        break;
    case PLAYER_EVENT_TOO_SLOW:
        toast("This video is too heavy for software decoding on the PS5", 5);
        break;
    default:
        break;
    }
    /* Save the position now and then: a crash or the PS button loses little. */
    if (now - last_save_time > 15 && !player_paused()) {
        save_position(false);
        last_save_time = now;
    }

    bool paused = player_paused();
    int64_t t = player_time(), len = player_length();
    if (sleep_mode != SLEEP_OFF && sleep_mode != SLEEP_END && now >= sleep_at) {
        sleep_mode = SLEEP_OFF;
        toast("Sleep timer: playback stopped", 4);
        leave_player(false);
        return;
    }
    if (loop_b > loop_a && loop_a >= 0 && t >= loop_b && now - last_seek_at > 0.5) {
        player_seek(loop_a);
        last_seek_at = now;
    }
    /* Touchpad swipes: across seeks (the whole pad = 90 s), up and down is the
     * volume. A swipe is judged when the finger lifts. */
    {
        static bool touching;
        static float t0x, t0y, t1x, t1y;
        if (cur_pad.touches && !touching) {
            touching = true;
            t0x = t1x = cur_pad.tx;
            t0y = t1y = cur_pad.ty;
        } else if (cur_pad.touches) {
            t1x = cur_pad.tx;
            t1y = cur_pad.ty;
        } else if (touching) {
            touching = false;
            float dx = t1x - t0x, dy = t1y - t0y;
            if (modal == NONE && pref_int("swipes", 1)) {
                if (fabsf(dx) > 0.2f && fabsf(dx) > fabsf(dy) * 1.5f) {
                    int64_t by_ms = (int64_t)(dx * 90000);
                    player_seek(t + by_ms);
                    last_seek_at = now;
                    osd_until = now + 3;
                    char msg[32];
                    snprintf(msg, sizeof(msg), "%+d s", (int)(by_ms / 1000));
                    toast(msg, 1);
                    buzz(0.3f, 30);
                } else if (fabsf(dy) > 0.25f && fabsf(dy) > fabsf(dx) * 1.5f) {
                    player_set_volume(player_volume() - (int)(dy * 100));
                    char msg[32];
                    snprintf(msg, sizeof(msg), tr("Volume %d%%"), player_volume());
                    toast(msg, 1);
                }
            }
        }
    }
    bool any_input = held != 0 && modal == NONE;
    /* Whether the overlay was up before this press: every press brings it up,
     * ○ included, so ○ must look at the time before it. */
    bool osd_was_up = now < osd_until;
    /* A 360° video: the right stick looks around (R3 back to the front). */
    if (player_video_info().spherical) {
        static float yaw, pitch, fov = 90;
        static std::string for_path;
        if (for_path != playing_path) {
            for_path = playing_path;
            yaw = pitch = 0;
            fov = 90;
            toast("360\xC2\xB0 video: look around with the right stick", 3);
        }
        yaw += cur_pad.rx * dt * 110;
        pitch = std::max(-85.0f, std::min(85.0f, pitch - cur_pad.ry * dt * 80));
        if (modal == NONE && hit(PAD_R3)) {
            yaw = pitch = 0;
            fov = 90;
        }
        player_view(yaw, pitch, fov);
    }
    /* A disc: the d-pad and ✕ drive its menu while the controls are hidden
     * (the touchpad brings them up). */
    bool disc = playing_disc();
    const uint32_t menu_keys = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_CROSS;
    /* A disc menu with buttons on screen (in the film ✕ pauses as usual): the
     * D-pad and ✕ press its buttons straight away, and the controls stay out
     * of the way unless the touchpad asked for them. */
    static bool osd_touch; /* the controls were brought up with the touchpad */
    if (now >= osd_until)
        osd_touch = false;
    bool in_menu = disc && player_in_menu() && modal == NONE && !paused;
    if (in_menu && !osd_touch)
        osd_until = std::min(osd_until, now);
    bool menu_press = in_menu && !(osd_was_up && osd_touch) && (pressed & menu_keys);
    if (menu_press) {
        player_navigate(hit(PAD_CROSS) ? 0 : hit(PAD_UP) ? 1 : hit(PAD_DOWN) ? 2 : hit(PAD_LEFT) ? 3 : 4);
        pressed &= ~menu_keys;
    }
    if (pressed && modal == NONE && (!in_menu || osd_touch))
        osd_until = now + 4.0;
    bool osd = now < osd_until || paused || scrubbing;

    if (modal == NONE) {
        if (hit(PAD_TRIANGLE)) {
            modal = TRACKS;
            modal_focus = 0;
            panel_section = SEC_ALL;
        } else if (hit(PAD_CIRCLE)) {
            /* ○ always goes back (the controls hide by themselves) */
            {
                int cur = current_item();
                bool music = (cur >= 0 && L()[cur].audio) ||
                             (is_link(playing_path) && !player_video_info().width && !video_has_picture());
                if (music && !paused) {
                    /* the menus come back, the music goes on (L3: back to it) */
                    music_bg = true;
                    screen = LIBRARY;
                    screen_fade = 0;
                    library_set_busy(false);
                    toast("Still playing: L3 brings it back", 2.5);
                } else {
                    leave_player(false);
                }
            }
            return;
        } else if (hit(PAD_TOUCHPAD)) {
            osd_until = osd ? now : now + 4.0;
            osd_touch = !osd;
        } else if (hit(PAD_L1) || hit(PAD_R1)) {
            std::vector<Chapter> ch = player_chapters();
            if (ch.empty()) {
                play_neighbour(hit(PAD_R1) ? 1 : -1);
                return;
            }
            /* Chapters: L1 goes to the start of this one first (as a CD player does). */
            int c = player_chapter(), n = (int)ch.size(), to = c;
            if (hit(PAD_R1))
                to = c + 1 < n ? c + 1 : c;
            else
                to = c >= 0 && c < n && t - ch[c].start_ms > 3000 ? c : std::max(0, c - 1);
            if (hit(PAD_R1) && c + 1 >= n) {
                toast("That's the last chapter", 1.5);
            } else {
                player_set_chapter(to);
                last_seek_at = now;
                char label[64];
                snprintf(label, sizeof(label), tr("Chapter %d of %d"), to + 1, n);
                toast(std::string(label) + "  \xC2\xB7  " + ch[to].name, 2);
                buzz(0.5f, 50);
            }
        } else if (hit(PAD_L3)) {
            std::string dir = library_media_dir() + "/Screenshots";
            mkdir(dir.c_str(), 0777);
            time_t tt = time(nullptr);
            struct tm tmv;
            localtime_r(&tt, &tmv);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tmv);
            std::string path = dir + "/" + playing_name + " " + stamp + ".png";
            toast(player_screenshot(path) ? "Screenshot saved to media/Screenshots" : "No picture to save", 2);
            buzz(0.6f, 70);
        } else if (frame_step_due()) {
            player_next_frame();
            frame_step_at = now;
        } else if (hit(PAD_L2) || hit(PAD_R2)) {
            player_seek(t + (hit(PAD_R2) ? 30000 : -30000));
            last_seek_at = now;
            buzz(0.25f, 30);
        } else if (!osd && (hit(PAD_LEFT) || hit(PAD_RIGHT))) {
            /* Overlay hidden: ←/→ jump 10 s right away. */
            player_seek(t + (hit(PAD_RIGHT) ? 10000 : -10000));
            last_seek_at = now;
            osd_row = 0;
        } else if (!osd && hit(PAD_CROSS) && now - last_seek_at > 1.5) {
            player_toggle_pause();
            pulse_anim = 1;
            pulse_paused = !paused;
        } else if (osd) {
            if (hit(PAD_UP))
                osd_row = 0;
            if (hit(PAD_DOWN))
                osd_row = 1;
            if (osd_row == 0) {
                if (hit(PAD_LEFT) || hit(PAD_RIGHT)) {
                    if (!scrubbing) {
                        scrubbing = true;
                        scrub_ms = t;
                    }
                    int rep = repeat_count[__builtin_ctz(hit(PAD_RIGHT) ? PAD_RIGHT : PAD_LEFT)];
                    int64_t step = rep > 20 ? 60000 : rep > 8 ? 30000 : 10000;
                    scrub_ms += hit(PAD_RIGHT) ? step : -step;
                    if (scrub_ms < 0)
                        scrub_ms = 0;
                    if (len > 0 && scrub_ms > len - 1000)
                        scrub_ms = len - 1000;
                    scrub_commit_at = now + 0.55;
                }
                if (hit(PAD_CROSS)) {
                    if (scrubbing) {
                        player_seek(scrub_ms);
                        scrubbing = false;
                        last_seek_at = now;
                    } else if (now - last_seek_at > 1.5) {
                        /* (✕ just after the jump landed means "OK", not pause.) */
                        player_toggle_pause();
                        pulse_anim = 1;
                        pulse_paused = !paused;
                    }
                }
            } else {
                if (hit(PAD_LEFT) && osd_col > 0)
                    osd_col--;
                if (hit(PAD_RIGHT) && osd_col < 9)
                    osd_col++;
                if (hit(PAD_CROSS)) {
                    switch (osd_col) {
                    case 0: play_neighbour(-1); return;
                    case 1: player_seek(t - 10000); break;
                    case 2:
                        player_toggle_pause();
                        pulse_anim = 1;
                        pulse_paused = !paused;
                        break;
                    case 3: player_seek(t + 10000); break;
                    case 4: play_neighbour(1); return;
                    case 5: {
                        /* VLC's loop button: off, all, this file. */
                        /* VLC's loop button: this video (or song) again and again. */
                        pref_set("loop", !loop_on());
                        int cur = current_item();
                        bool song = cur >= 0 && L()[cur].audio;
                        toast(loop_on() ? (song ? "Loop this song" : "Loop this video")
                                        : (song ? "Loop off: this song plays once" : "Loop off: this video plays once"), 1.5);
                        break;
                    }
                    case 6: case 7: case 8: case 9:
                        /* Audio, Subtitles, Playback, Picture: their own panel. */
                        modal = TRACKS;
                        modal_focus = 0;
                        panel_section = osd_col - 6;
                        break;
                    }
                }
            }
        }
    }
    if (scrubbing && now >= scrub_commit_at) {
        player_seek(scrub_ms);
        last_seek_at = now;
        scrubbing = false;
    }
    (void)any_input;

    osd_anim = approach(osd_anim, osd ? 1.0f : 0.0f, osd ? 14 : 6);
    float a = ease(osd_anim);

    if (player_buffering() || !video_has_picture()) {
        if (!video_has_picture() && !player_video_info().width) {
            /* Music, or the first picture is coming: something nice to look at. */
            MediaItem *m = library_find(playing_path);
            /* A stream with sound and no picture after a while: radio. */
            if (!m && is_link(playing_path) && !player_buffering() && player_time() > 1500) {
                background();
                float pulse = 1 + 0.03f * sinf((float)now * 2.0f);
                float s = 420 * pulse;
                glow(960 - s / 2, 400 - s / 2, s, s, 40, C_ORANGE, 60, 10);
                dl->AddRectFilledMultiColor(P(960 - s / 2, 400 - s / 2), P(960 + s / 2, 400 + s / 2),
                                            tint_for(playing_name, 0.6f), tint_for(playing_name + "#", 0.5f),
                                            tint_for(playing_name, 0.25f), tint_for(playing_name + "#", 0.3f));
                if (classic_look)
                    stroke(960 - s / 2, 400 - s / 2, s, s, IM_COL32(255, 255, 255, 46), 0, 1);
                icon_radio(960, 400, 200, IM_COL32(255, 255, 255, 230));
                text(f_bold, 40, 960, 660, C_TEXT, playing_name.c_str(), 0.5f, 1400);
                text(f_reg, 24, 960, 716, C_DIM, player_length() > 0 ? "Stream" : "Live", 0.5f);
                visualizer(960, 760, 860, 80);
                show_video = false;
            }
            if (m && m->audio) {
                background();
                float pulse = 1 + 0.03f * sinf((float)now * 2.0f);
                float s = 420 * pulse;
                glow(960 - s / 2, 400 - s / 2, s, s, 40, C_ORANGE, 60, 10);
                if (m->thumb) {
                    float ta = m->thumb_aspect > 0 ? m->thumb_aspect : 1, cw = s, ch = s;
                    if (ta > 1)
                        ch = s / ta;
                    else
                        cw = s * ta;
                    rect(960 - s / 2, 400 - s / 2, s, s, IM_COL32(0, 0, 0, 255));
                    dl->AddImage(m->thumb, P(960 - cw / 2, 400 - ch / 2), P(960 + cw / 2, 400 + ch / 2));
                } else {
                    dl->AddRectFilledMultiColor(P(960 - s / 2, 400 - s / 2), P(960 + s / 2, 400 + s / 2),
                                                tint_for(m->name, 0.6f), tint_for(m->name + "#", 0.5f),
                                                tint_for(m->name, 0.25f), tint_for(m->name + "#", 0.3f));
                    icon_note(960, 400, 180, IM_COL32(255, 255, 255, 230));
                }
                if (classic_look)
                    stroke(960 - s / 2, 400 - s / 2, s, s, IM_COL32(255, 255, 255, 46), 0, 1);
                text(f_bold, 40, 960, 640, C_TEXT, m->name.c_str(), 0.5f, 1400);
                std::string by = !m->artist.empty() ? m->artist + (m->album.empty() ? "" : "  \xC2\xB7  " + m->album)
                                                    : m->folder.substr(m->folder.rfind('/') + 1);
                text(f_reg, 24, 960, 694, C_DIM, by.c_str(), 0.5f, 1400);
                visualizer(960, 742, 860, 92);
                show_video = false;
            }
        }
        if (player_buffering())
            spinner(960, 540, 40);
    }

    /* The big play/pause flash. */
    if (pulse_anim > 0.01f) {
        pulse_anim = approach(pulse_anim, 0, 5);
        float k = pulse_anim, r = 70 + (1 - k) * 30;
        circle(960, 540, r, alpha(IM_COL32(0, 0, 0, 255), 0.55f * k));
        if (pulse_paused)
            icon_pause(960, 540, 48, alpha(C_TEXT, k));
        else
            icon_play(965, 540, 52, alpha(C_TEXT, k));
    }
    /* Paused: say so for as long as it lasts, so a still picture never looks
     * like a stuck one. */
    static float pause_badge;
    pause_badge = approach(pause_badge, paused && pulse_anim < 0.3f && now - frame_step_at > 2 ? 1.0f : 0.0f, 10);
    if (pause_badge > 0.01f) {
        float k = pause_badge;
        circle(960, 540, 66, alpha(IM_COL32(0, 0, 0, 255), 0.6f * k));
        ring(960, 540, 66, alpha(C_ORANGE, 0.9f * k), 3);
        icon_pause(960, 540, 44, alpha(C_TEXT, k));
    }

    if (a < 0.01f)
        return;
    const VideoInfo &v = player_video_info();
    std::string meta;
    if (v.width) {
        char res[64];
        snprintf(res, sizeof(res), "%ux%u", v.width, v.height);
        meta = res;
        if (!v.codec.empty())
            meta += "  \xC2\xB7  " + v.codec;
        if (v.ten_bit)
            meta += "  \xC2\xB7  10-bit";
        if (v.hdr)
            meta += v.hdr == 2 ? "  \xC2\xB7  HLG" : "  \xC2\xB7  HDR10";
        if (v.hardware)
            meta += "  \xC2\xB7  HW";
    }
    if (!v.audio_codec.empty()) {
        meta += (meta.empty() ? "" : "  \xC2\xB7  ") + v.audio_codec;
        if (v.audio_channels)
            meta += v.audio_channels == 6 ? " 5.1" : v.audio_channels == 8 ? " 7.1" : v.audio_channels == 2 ? " " + std::string(tr("stereo")) : "";
    }
    {
        std::vector<Chapter> ch = player_chapters();
        int c = player_chapter();
        if (!ch.empty() && c >= 0 && c < (int)ch.size()) {
            char cl[48];
            snprintf(cl, sizeof(cl), tr("Chapter %d/%d"), c + 1, (int)ch.size());
            meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::string(cl);
        }
        if (loop_b > loop_a && loop_a >= 0)
            meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::string("A-B repeat");
        if (sleep_mode != SLEEP_OFF)
            meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::string("Sleep timer");
    }
    if (fabsf(player_rate() - 1) > 0.01f) {
        char r[16];
        snprintf(r, sizeof(r), "%.2gx", player_rate());
        meta += std::string("  \xC2\xB7  ") + r;
    }
    if (classic_look) {
        player_overlay_classic(a, t, len, paused, meta);
        return;
    }
    /* Top: title and what's playing. */
    dl->AddRectFilledMultiColor(P(0, 0), P(1920, 260), alpha(IM_COL32(0, 0, 0, 255), 0.8f * a),
                                alpha(IM_COL32(0, 0, 0, 255), 0.8f * a), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    float ty = 52 - (1 - a) * 20;
    text(f_bold, 36, 96, ty, alpha(C_TEXT, a), playing_name.c_str(), 0, 1400);
    text(f_reg, 21, 96, ty + 52, alpha(C_DIM, a), meta.c_str());
    time_t tt = time(nullptr);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d", tmv.tm_hour, tmv.tm_min);
    text(f_semi, 30, 1824, ty, alpha(C_TEXT, a), clock, 1);
    if (len > 0 && !paused) {
        time_t ends = tt + (time_t)((len - t) / 1000 / std::max(0.25f, player_rate()));
        localtime_r(&ends, &tmv);
        char e[32];
        snprintf(e, sizeof(e), tr("Ends at %d:%02d"), tmv.tm_hour, tmv.tm_min);
        text(f_reg, 20, 1824, ty + 44, alpha(C_DIM, a), e, 1);
    }

    /* Bottom: seek bar and buttons. */
    dl->AddRectFilledMultiColor(P(0, 700), P(1920, 1080), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                alpha(IM_COL32(0, 0, 0, 255), 0.88f * a), alpha(IM_COL32(0, 0, 0, 255), 0.88f * a));
    float by = 1080 - 218 + (1 - a) * 30;
    float bar_x = 96, bar_w = 1728;
    float fs = focus_anim(700, osd_row == 0 && modal == NONE);
    float bh = 6 + 4 * fs;
    float frac = len > 0 ? (float)t / len : 0;
    float sfrac = len > 0 ? (float)scrub_ms / len : 0;
    rect(bar_x, by - bh / 2, bar_w, bh, alpha(IM_COL32(255, 255, 255, 55), a), bh / 2);
    float shown_frac = scrubbing ? sfrac : frac;
    dl->AddRectFilledMultiColor(P(bar_x, by - bh / 2), P(bar_x + bar_w * frac, by + bh / 2),
                                alpha(C_ORANGE2, a), alpha(C_ORANGE, a), alpha(C_ORANGE, a), alpha(C_ORANGE2, a));
    if (scrubbing)
        rect(bar_x + bar_w * std::min(frac, sfrac), by - bh / 2, bar_w * fabsf(sfrac - frac), bh,
             alpha(IM_COL32(255, 255, 255, 120), a), bh / 2);
    if (len > 0) {
        for (const Chapter &c : player_chapters())
            if (c.start_ms > 0)
                rect(bar_x + bar_w * c.start_ms / len - 1.5f, by - bh / 2 - 3, 3, bh + 6,
                     alpha(IM_COL32(255, 255, 255, 200), a));
        for (int64_t bm : library_bookmarks(playing_path)) {
            float bx = bar_x + bar_w * bm / len;
            tri(bx - 7, by - 16, bx + 7, by - 16, bx, by - 6, alpha(C_TEXT, a));
        }
        if (loop_a >= 0)
            rect(bar_x + bar_w * loop_a / len - 2, by - 14, 4, 28, alpha(C_ORANGE, a), 2);
        if (loop_b >= 0)
            rect(bar_x + bar_w * loop_b / len - 2, by - 14, 4, 28, alpha(C_ORANGE, a), 2);
    }
    float kx = bar_x + bar_w * shown_frac;
    float kr = 8 + 7 * fs;
    if (fs > 0.01f)
        circle(kx, by, kr + 10 * fs, alpha(C_ORANGE, 0.25f * fs * a));
    circle(kx, by, kr, alpha(C_TEXT, a));
    if (scrubbing) {
        std::string s = format_time(scrub_ms);
        float tw = text_size(f_bold, 26, s.c_str()).x + 32;
        rect(kx - tw / 2, by - 76, tw, 46, alpha(IM_COL32(255, 255, 255, 235), a), 12);
        tri(kx - 9, by - 31, kx + 9, by - 31, kx, by - 20, alpha(IM_COL32(255, 255, 255, 235), a));
        text(f_bold, 26, kx, by - 68, alpha(C_BG, a), s.c_str(), 0.5f);
    }
    text(f_semi, 24, bar_x, by + 22, alpha(C_TEXT, a), format_time(scrubbing ? scrub_ms : t).c_str());
    if (len > 0)
        text(f_semi, 24, bar_x + bar_w, by + 22, alpha(C_DIM, a),
             ("-" + format_time(len - (scrubbing ? scrub_ms : t))).c_str(), 1);

    /* Buttons: prev, -10, play/pause, +10, next | audio, subtitles, speed, picture. */
    float cy = 1080 - 88 + (1 - a) * 30;
    struct Btn {
        float x, r;
    } btn[10] = { { 960 - 280, 32 }, { 960 - 145, 36 }, { 960, 46 }, { 960 + 145, 36 },
                  { 960 + 280, 32 }, { 960 + 395, 28 }, { 1824 - 330, 30 }, { 1824 - 230, 30 },
                  { 1824 - 130, 30 }, { 1824 - 30, 30 } };
    int repeat_mode = loop_on() ? 2 : 0;
    for (int i = 0; i < 10; i++) {
        bool foc = osd_row == 1 && osd_col == i && modal == NONE;
        float f = focus_anim(800 + i, foc, 16);
        float r = btn[i].r * (1 + 0.12f * f);
        ImU32 fill = i == 2 ? mix(IM_COL32(255, 255, 255, 40), C_TEXT, f) : alpha(C_TEXT, 0.13f + 0.87f * f);
        if (f > 0.01f)
            glow(btn[i].x - r, cy - r, 2 * r, 2 * r, r, alpha(C_ORANGE, 0.5f * f * a), 14, 6);
        circle(btn[i].x, cy, r, alpha(fill, a));
        ImU32 ic = alpha(mix(C_TEXT, C_BG, f), a);
        float s = r * 0.9f;
        switch (i) {
        case 0: icon_skip(btn[i].x, cy, s * 0.8f, ic, false); break;
        case 1: icon_jump(btn[i].x, cy, s * 1.05f, ic, false, "10"); break;
        case 2:
            if (paused)
                icon_play(btn[i].x + 3, cy, s * 0.8f, ic);
            else
                icon_pause(btn[i].x, cy, s * 0.7f, ic);
            break;
        case 3: icon_jump(btn[i].x, cy, s * 1.05f, ic, true, "10"); break;
        case 4: icon_skip(btn[i].x, cy, s * 0.8f, ic, true); break;
        case 5:
            icon_repeat(btn[i].x, cy, s * 1.0f,
                        repeat_mode ? alpha(f > 0.5f ? C_BG : C_ORANGE, a) : ic, repeat_mode);
            break;
        case 6: icon_speaker(btn[i].x, cy, s * 1.0f, ic); break;
        case 7: icon_cc(btn[i].x, cy, s * 1.0f, ic); break;
        case 8: icon_speed(btn[i].x, cy, s * 1.0f, ic); break;
        case 9: icon_picture(btn[i].x, cy, s * 1.0f, ic); break;
        }
    }
    /* What the focused button does. */
    static const char *const tips[10] = { "Previous", "Back 10 s", "Play / Pause", "Forward 10 s",
                                          "Next", "Loop", "Audio & volume", "Subtitles", "Playback", "Picture" };
    if (osd_row == 1 && modal == NONE && osd_col != 2) /* the big play button needs no name */
        text(f_semi, 20, btn[osd_col].x, cy + btn[osd_col].r + 8, alpha(C_DIM, a), tips[osd_col], 0.5f); /* under it */
    /* Left of the transport buttons, clear of them. */
    if (paused)
        hints(600, 1080 - 88 + (1 - a) * 30, { { PAD_SQUARE, "Next frame (hold: keep going)" }, { PAD_CIRCLE, "Back" } }, a * 0.9f);
    else
        hints(600, 1080 - 88 + (1 - a) * 30,
              { { PAD_TRIANGLE, "Tracks" }, { PAD_CIRCLE, "Back" } }, a * 0.9f);
}

/* A theme from a drive or /data/vlc: vlc-theme.txt with "name=", "accent=#RRGGBB"
 * and optionally "accent2=#RRGGBB". It joins the list after the built-in ones. */
void load_usb_theme()
{
    themes.resize(BUILTIN_THEMES);
    std::vector<std::string> dirs = library_roots();
    dirs.push_back("/data/vlc");
    for (const std::string &d : dirs) {
        FILE *f = fopen((d + "/vlc-theme.txt").c_str(), "r");
        if (!f)
            continue;
        Theme t = { trf("From %s", d.substr(d.rfind('/') + 1).c_str()), C_ORANGE, 0 };
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
                l.pop_back();
            size_t eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "name" && !v.empty())
                t.name = v;
            else if (k == "accent")
                t.accent = parse_colour(v, t.accent);
            else if (k == "accent2")
                t.accent2 = parse_colour(v, 0);
        }
        fclose(f);
        if (!t.accent2)
            t.accent2 = mix(t.accent, IM_COL32(0, 0, 0, 255), 0.18f);
        themes.push_back(t);
        break;
    }
}

void toast_draw()
{
    float a = (float)std::min(1.0, std::max(0.0, (toast_until - now) * 3));
    static float ta;
    ta = approach(ta, a, 12);
    if (ta < 0.01f || toast_text.empty())
        return;
    float tw = text_size(f_semi, 24, toast_text.c_str()).x + 64;
    float y = 120 - (1 - ta) * 20;
    if (classic_look) {
        /* A tooltip balloon under the title band. */
        y += 6;
        rect(960 - tw / 2 + 3, y + 4, tw, 56, alpha(IM_COL32(0, 0, 0, 120), ta), 4);
        vgrad(960 - tw / 2, y, tw, 56, alpha(IM_COL32(64, 69, 78, 255), ta), alpha(IM_COL32(40, 43, 49, 255), ta));
        stroke(960 - tw / 2, y, tw, 56, alpha(IM_COL32(10, 11, 13, 255), ta), 4, 1.5f);
        tri(960 - 9, y, 960 + 9, y, 960, y - 9, alpha(IM_COL32(64, 69, 78, 255), ta));
        circle(960 - tw / 2 + 24, y + 28, 9, alpha(C_ORANGE, ta));
        text(f_bold, 15, 960 - tw / 2 + 24, y + 18, alpha(IM_COL32(255, 255, 255, 255), ta), "i", 0.5f);
        text(f_semi, 24, 960 + 10, y + 14, alpha(C_TEXT, ta), toast_text.c_str(), 0.5f);
        return;
    }
    glow(960 - tw / 2, y, tw, 56, 28, alpha(IM_COL32(0, 0, 0, 255), ta), 20);
    rect(960 - tw / 2, y, tw, 56, alpha(IM_COL32(32, 32, 40, 245), ta), 28);
    rect(960 - tw / 2 + 18, y + 22, 12, 12, alpha(C_ORANGE, ta), 6);
    text(f_semi, 24, 960 + 10, y + 14, alpha(C_TEXT, ta), toast_text.c_str(), 0.5f);
}

} // namespace

bool ui_init()
{
    lang_init();
    ImGuiIO &io = ImGui::GetIO();
    std::string fonts = std::string(plat_data_dir()) + "/fonts/";
    /* Each interface font, with the subtitle fallbacks merged in (Arabic,
     * Hebrew, Thai, Devanagari, CJK...): file names in any script. Glyphs are
     * made when first drawn, and each fallback file is read once and shared. */
    /* Arabic and Hebrew letters are drawn small in their fonts next to Inter: a bit larger. */
    struct Fallback {
        std::vector<uint8_t> data;
        float scale;
    };
    static std::vector<Fallback> fallbacks;
    if (fallbacks.empty()) {
        std::string dir = fonts + "fallback";
        std::vector<std::string> names;
        if (DIR *d = opendir(dir.c_str())) {
            while (struct dirent *e = readdir(d)) {
                const char *dot = strrchr(e->d_name, '.');
                if (dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") || !strcasecmp(dot, ".ttc")))
                    names.push_back(e->d_name);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) {
            FILE *fp = fopen((dir + "/" + n).c_str(), "rb");
            if (!fp)
                continue;
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            std::vector<uint8_t> data(size > 0 ? (size_t)size : 0);
            if (size > 0 && fread(data.data(), 1, data.size(), fp) == data.size())
                fallbacks.push_back({ std::move(data), n.find("Arabic") != std::string::npos ||
                                                       n.find("Hebrew") != std::string::npos ? 1.4f : 1.0f });
            fclose(fp);
        }
        fprintf(stderr, "ui: %zu fallback fonts\n", fallbacks.size());
    }
    /* also: a second font for what the first lacks (Selawik has no Cyrillic:
     * Inter of the same weight gives it) */
    auto add = [&](const char *file, const char *also) {
        ImFont *font = io.Fonts->AddFontFromFileTTF((fonts + file).c_str(), 24);
        if (!font)
            return font;
        if (also) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF((fonts + also).c_str(), 24, &cfg);
        }
        for (auto &fb : fallbacks) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.FontDataOwnedByAtlas = false;
            cfg.ExtraSizeScale = fb.scale;
            io.Fonts->AddFontFromMemoryTTF(fb.data.data(), (int)fb.data.size(), 24, &cfg);
        }
        return font;
    };
    apply_look();
    /* Classic: Selawik (Microsoft's open font in the style of the Windows 7
     * one); Modern: Inter. */
    f_reg = add(classic_look ? "Selawik-Regular.ttf" : "Inter-Regular.ttf", classic_look ? "Inter-Regular.ttf" : nullptr);
    f_semi = add(classic_look ? "Selawik-SemiBold.ttf" : "Inter-SemiBold.ttf", classic_look ? "Inter-SemiBold.ttf" : nullptr);
    f_bold = add(classic_look ? "Selawik-Bold.ttf" : "Inter-Bold.ttf", classic_look ? "Inter-Bold.ttf" : nullptr);
    f_mono = add("DejaVuSansMono.ttf", nullptr);
    if (!f_reg || !f_semi || !f_bold) {
        fprintf(stderr, "ui: fonts missing in %s\n", fonts.c_str());
        ImFont *d = io.Fonts->AddFontDefault();
        f_reg = f_reg ? f_reg : d;
        f_semi = f_semi ? f_semi : d;
        f_bold = f_bold ? f_bold : d;
    }
    if (!f_mono) {
        f_mono = f_reg;
    }
    logo_tex = library_load_image(std::string(plat_data_dir()) + "/assets/vlc-logo.png", &logo_aspect);
    load_usb_theme();
    apply_theme();
    srand((unsigned)time(nullptr));
    web_places_update();
    if (pref_int("web", 1))
        web_start();
    net_init();
    rebuild_lists();
    rebuild_browse();
    /* the first run: the logo and the setup; an install from before just
     * goes on as it was */
    if (!pref_int("setup_done", 0)) {
        if (prefs_existed()) {
            pref_set("look", classic_look ? 1 : 0);
            pref_set("setup_done", 1);
        } else {
            setup_begin(true);
        }
    }
    return true;
}

namespace {

/* ---- the first run: VLC's logo on black, then a few choices ------------------
 * Shown once, on a new install (no prefs.txt yet); again from Settings >
 * System > Run setup again (without the logo). Drawn in the Modern style:
 * pages that slide, dots for where you are. ✕ goes on, ○ back, OPTIONS skips. */
bool setup_on, setup_boot;
int setup_page;
float setup_slide;
double setup_since;
int setup_lang_focus, setup_look, setup_row;
bool setup_was_classic;
ImTextureID setup_img[2];
float setup_img_aspect[2] = { 16.0f / 9, 16.0f / 9 };
const int SETUP_PAGES = 4;
double setup_leaving = -1;  /* the last fade to black began */
double setup_ended = -10;   /* the home screen comes up out of black from here */

void setup_begin(bool with_logo)
{
    setup_on = true;
    setup_boot = with_logo;
    setup_page = 0;
    setup_slide = 0;
    setup_since = -1; /* from its first frame (at start-up the clock isn't running yet) */
    setup_lang_focus = lang_current();
    setup_was_classic = classic_look;
    setup_look = classic_look ? 1 : 0;
    setup_row = 0;
    setup_leaving = -1;
    modal = NONE;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        std::string dir = std::string(plat_data_dir()) + "/assets/setup/";
        setup_img[0] = library_load_image(dir + "modern.png", &setup_img_aspect[0]);
        setup_img[1] = library_load_image(dir + "classic.png", &setup_img_aspect[1]);
    }
}

void setup_finish()
{
    pref_set("setup_done", 1);
    setup_on = false;
    setup_ended = now;
    screen_fade = 0;
    if ((setup_look == 1) != setup_was_classic) {
        /* the other look: VLC starts again in it */
        pref_set("look", setup_look);
        plat_restart();
        toast("Couldn't restart: close VLC and open it again to change the look", 4);
    }
}

/* Text not translated (a language's own name). */
void text_as_is(ImFont *f, float size, float x, float y, ImU32 col, const char *s, float align)
{
    s = lang_shown_as_is(s);
    float w = align ? f->CalcTextSizeA(size * S, FLT_MAX, 0, s).x / S : 0;
    dl->AddText(f, size * S, P(x - w * align, y), col, s);
}

/* Calm entrances: on a page that just came, each part fades in a little
 * after the one before, drifting up into place. */
double setup_entered;      /* when the page in view came */
double setup_lang_at = -10; /* when the language last changed (the words fade) */

float setup_k(float since, int order)
{
    float t = (since - 0.11f * order) / 0.75f;
    return ease(t < 0 ? 0 : t > 1 ? 1 : t);
}

void setup_page_draw(int p, float ox, float a, bool active, float since)
{
    /* words fade back in after a language change */
    float lw = ease((float)std::min(1.0, (now - setup_lang_at) / 0.4));
    auto k = [&](int order) { return setup_k(since, order); };
    auto dy = [&](int order) { return (1 - k(order)) * 22; };
    ImU32 card = IM_COL32(28, 28, 35, 255);
    auto txt = [&](float m) { return alpha(IM_COL32(245, 245, 248, 255), a * m); };
    auto dim = [&](float m) { return alpha(IM_COL32(150, 150, 162, 255), a * m); };
    float cx = 960 + ox;
    if (p == 0) {
        /* Welcome, and the language (it changes as you move) */
        logo(cx, 210 + dy(0), 120, a * k(0));
        text(f_bold, 58, cx, 300 + dy(1), txt(k(1) * lw), "Welcome to VLC", 0.5f);
        text(f_reg, 26, cx, 385 + dy(2), dim(k(2) * lw), "Choose your language", 0.5f);
        int n = lang_count();
        const float bw = 350, bh = 74, gap = 18;
        float x0 = cx - (4 * bw + 3 * gap) / 2, y0 = 460;
        if (active) {
            int f = setup_lang_focus, before = f;
            if (hit(PAD_LEFT) && f % 4 > 0) f--;
            if (hit(PAD_RIGHT) && f % 4 < 3 && f + 1 < n) f++;
            if (hit(PAD_UP) && f >= 4) f -= 4;
            if (hit(PAD_DOWN) && f + 4 < n) f += 4;
            setup_lang_focus = f;
            if (f != before) {
                lang_set(f);
                setup_lang_at = now;
            }
        }
        for (int i = 0; i < n; i++) {
            int order = 3 + i / 4; /* row by row */
            float m = k(order);
            float x = x0 + (i % 4) * (bw + gap), y = y0 + (i / 4) * (bh + gap) + dy(order);
            float f = focus_anim(9000 + i, i == setup_lang_focus, 10);
            if (f > 0.01f)
                glow(x, y, bw, bh, 18, alpha(C_ORANGE, 0.4f * f * a * m), 14, 6);
            rect(x, y, bw, bh, alpha(mix(card, C_ORANGE, f), a * m), 18);
            text_as_is(f_semi, 26, x + bw / 2, y + 22,
                       alpha(mix(IM_COL32(220, 220, 228, 255), IM_COL32(255, 255, 255, 255), f), a * m), lang_native_name(i), 0.5f);
        }
    } else if (p == 1) {
        /* the look: two pictures of the home screen */
        text(f_bold, 52, cx, 150 + dy(0), txt(k(0)), "Choose your look", 0.5f);
        text(f_reg, 24, cx, 228 + dy(1), dim(k(1)), "You can change it later in Settings", 0.5f);
        if (active) {
            if (hit(PAD_LEFT))
                setup_look = 0;
            if (hit(PAD_RIGHT))
                setup_look = 1;
        }
        static const char *const names[2] = { "Modern", "Classic" };
        static const char *const descs[2] = { "Black and clean, in the style of VLC 4",
                                              "Panels and folders, like a desktop player" };
        const float cw = 790, ch = 590, gap = 56;
        float x0 = cx - (2 * cw + gap) / 2;
        for (int i = 0; i < 2; i++) {
            float m = k(2 + i), x = x0 + i * (cw + gap);
            float f = focus_anim(9100 + i, i == setup_look, 8);
            float y = 300 - 10 * f + dy(2 + i);
            float am = a * m;
            if (f > 0.01f)
                glow(x, y, cw, ch, 26, alpha(C_ORANGE, 0.45f * f * am), 22, 8);
            rect(x, y, cw, ch, alpha(card, am), 26);
            stroke(x, y, cw, ch, alpha(mix(IM_COL32(255, 255, 255, 30), C_ORANGE, f), am), 26, 2 + 2 * f);
            float iw = cw - 60, ih = iw * 9 / 16;
            if (setup_img[i])
                dl->AddImageRounded(setup_img[i], P(x + 30, y + 30), P(x + 30 + iw, y + 30 + ih), ImVec2(0, 0), ImVec2(1, 1),
                                    alpha(IM_COL32_WHITE, am * (0.6f + 0.4f * f)), 14 * S);
            else
                rect(x + 30, y + 30, iw, ih, alpha(IM_COL32(18, 18, 24, 255), am), 14);
            text(f_bold, 34, x + 40, y + 30 + ih + 30, txt(m), names[i]);
            text(f_reg, 22, x + 40, y + 30 + ih + 80, dim(m), descs[i], 0, cw - 80);
            /* the tick of the chosen one grows in */
            circle(x + cw - 56, y + 30 + ih + 50, 20 * (0.6f + 0.4f * f), alpha(C_ORANGE, am * f));
            icon_check(x + cw - 56, y + 30 + ih + 50, 22 * (0.6f + 0.4f * f), alpha(IM_COL32(255, 255, 255, 255), am * f));
        }
    } else if (p == 2) {
        /* what VLC picks for you */
        text(f_bold, 52, cx, 150 + dy(0), txt(k(0)), "Playback", 0.5f);
        text(f_reg, 24, cx, 228 + dy(1), dim(k(1)), "Pick what VLC chooses for you", 0.5f);
        static double changed_at[3] = { -10, -10, -10 };
        if (active) {
            if (hit(PAD_UP) && setup_row > 0)
                setup_row--;
            if (hit(PAD_DOWN) && setup_row < 2)
                setup_row++;
            if (hit(PAD_LEFT) || hit(PAD_RIGHT)) {
                int dir = hit(PAD_RIGHT) ? 1 : -1;
                if (setup_row == 0)
                    cycle_language(false, dir);
                else if (setup_row == 1)
                    cycle_language(true, dir);
                else
                    player_set_audio_output(!player_audio_output());
                changed_at[setup_row] = now;
            }
        }
        static const char *const labels[3] = { "Audio language", "Subtitle language", "Sound output" };
        std::string values[3] = { lang_name(player_audio_language(), false), lang_name(player_sub_language(), true),
                                  player_audio_output() ? "Surround 5.1 / 7.1" : "Stereo" };
        const float rw = 1100, rh = 100;
        float x = cx - rw / 2;
        for (int r = 0; r < 3; r++) {
            float m = k(2 + r), am = a * m;
            float y = 330 + r * 124 + dy(2 + r);
            float f = focus_anim(9200 + r, r == setup_row, 10);
            if (f > 0.01f)
                glow(x, y, rw, rh, 20, alpha(C_ORANGE, 0.3f * f * am), 14, 6);
            rect(x, y, rw, rh, alpha(mix(card, IM_COL32(44, 44, 56, 255), f), am), 20);
            rect(x, y + 22, 5, rh - 44, alpha(C_ORANGE, f * am), 3);
            text(f_semi, 28, x + 44, y + 32, txt(m), labels[r]);
            /* a new value fades in from the side */
            float c = ease((float)std::min(1.0, (now - changed_at[r]) / 0.3));
            float vx = x + rw - 44;
            float tw = text_size(f_semi, 26, values[r].c_str()).x;
            tri(vx - 2, y + 40, vx - 2, y + 60, vx + 10, y + 50, alpha(C_ORANGE, f * am));
            tri(vx - tw - 40, y + 40, vx - tw - 40, y + 60, vx - tw - 52, y + 50, alpha(C_ORANGE, f * am));
            text(f_semi, 26, vx - 22 * f + (1 - c) * 12, y + 35,
                 alpha(mix(IM_COL32(150, 150, 162, 255), IM_COL32(255, 255, 255, 255), 0.4f + 0.6f * f), am * (0.3f + 0.7f * c)),
                 values[r].c_str(), 1);
        }
    } else {
        /* ready: three things worth knowing, then go */
        logo(cx, 200 + dy(0), 110, a * k(0));
        text(f_bold, 56, cx, 285 + dy(1), txt(k(1)), "You're all set", 0.5f);
        static const struct {
            uint32_t button;
            const char *what;
        } tips[3] = { { PAD_CIRCLE, "Goes back, from anywhere" },
                      { PAD_TRIANGLE, "Tracks, subtitles and picture while watching" },
                      { PAD_OPTIONS, "Options: search, settings and more" } };
        const float tw = 900;
        float tx = cx - tw / 2;
        for (int i = 0; i < 3; i++) {
            float m = k(2 + i);
            float y = 420 + i * 92 + dy(2 + i);
            rect(tx, y, tw, 76, alpha(card, a * m), 18);
            glyph(tx + 52, y + 38, 24, tips[i].button, a * m);
            text(f_reg, 25, tx + 96, y + 23, txt(m), tips[i].what, 0, tw - 120);
        }
        float m = k(5);
        float bw = 460, bh = 86, bx = cx - bw / 2, by = 740 + dy(5);
        /* a slow breath on the button */
        float pulse = 0.5f + 0.5f * sinf((float)now * 1.6f);
        glow(bx, by, bw, bh, 43, alpha(C_ORANGE, (0.3f + 0.25f * pulse) * a * m), 22, 8);
        rect(bx, by, bw, bh, alpha(C_ORANGE, a * m), 43);
        text(f_bold, 30, cx, by + 26, alpha(IM_COL32(255, 255, 255, 255), a * m), "Start watching", 0.5f);
    }
}

void setup_screen()
{
    /* the setup is drawn Modern whatever the look */
    bool classic = classic_look;
    classic_look = false;
    rect(0, 0, 1920, 1080, IM_COL32(0, 0, 0, 255));
    if (setup_since < 0)
        setup_since = now;
    if (setup_boot) {
        /* the logo on black, as VLC opens: a slow fade in, a few seconds,
         * a slow fade out */
        const float in_s = 1.4f, hold = 4.3f, out_s = 1.4f;
        float t = (float)(now - setup_since);
        float a = t < in_s ? ease(t / in_s) : t < in_s + hold ? 1.0f : 1.0f - ease((t - in_s - hold) / out_s);
        logo(960, 520, 240, a < 0 ? 0 : a);
        if (t > in_s + hold + out_s + 0.3f) {
            setup_boot = false;
            setup_since = now;
            setup_entered = now;
        }
        pressed = 0;
        classic_look = classic;
        return;
    }
    if (setup_entered < setup_since)
        setup_entered = setup_since;
    float in = ease((float)std::min(1.0, (now - setup_since) / 1.2));
    /* leaving: everything fades to black, then the home screen */
    float out = setup_leaving > 0 ? 1.0f - ease((float)std::min(1.0, (now - setup_leaving) / 0.7)) : 1.0f;
    float g = in * out;
    /* a warm light from the top that breathes slowly, the rest black */
    float breath = 0.82f + 0.18f * sinf((float)now * 0.5f);
    dl->AddRectFilledMultiColor(P(0, 0), P(1920, 560), alpha(IM_COL32(40, 22, 6, 255), g * breath),
                                alpha(IM_COL32(40, 22, 6, 255), g * breath), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    /* where you are */
    float dots_w = SETUP_PAGES * 16 + 30;
    float dx = 960 - dots_w / 2;
    for (int i = 0; i < SETUP_PAGES; i++) {
        float f = focus_anim(9300 + i, i == setup_page, 7);
        float w = 10 + 28 * f;
        rect(dx, 60, w, 10, alpha(mix(IM_COL32(255, 255, 255, 50), C_ORANGE, f), g), 5);
        dx += w + 12;
    }
    /* pages: a short glide and a crossfade */
    setup_slide = approach(setup_slide, (float)setup_page, 6);
    for (int p = 0; p < SETUP_PAGES; p++) {
        float d = p - setup_slide;
        if (fabsf(d) >= 1)
            continue;
        float vis = ease(1 - fabsf(d));
        float since = p == setup_page ? (float)(now - setup_entered) : 10.0f;
        setup_page_draw(p, d * 260, g * vis, p == setup_page && fabsf(d) < 0.3f && setup_leaving < 0, since);
    }
    std::vector<std::pair<uint32_t, const char *>> h;
    h.push_back({ PAD_CROSS, setup_page + 1 < SETUP_PAGES ? "Continue" : "Start" });
    if (setup_page > 0)
        h.push_back({ PAD_CIRCLE, "Back" });
    h.push_back({ PAD_OPTIONS, "Skip" });
    hints(1824, 1020, h, g);
    if (setup_leaving > 0) {
        if (now - setup_leaving > 0.75) {
            setup_leaving = -1;
            setup_finish();
        }
    } else if (hit(PAD_CROSS)) {
        if (setup_page + 1 < SETUP_PAGES) {
            setup_page++;
            setup_entered = now;
        } else {
            setup_leaving = now;
        }
    } else if (hit(PAD_CIRCLE) && setup_page > 0) {
        setup_page--;
        setup_entered = now;
    } else if (hit(PAD_OPTIONS)) {
        setup_leaving = now;
    }
    pressed = 0;
    classic_look = classic;
}

}  // namespace

void ui_frame(const PadState &pad, float frame_dt)
{
    dt = frame_dt > 0.1f ? 0.1f : frame_dt;
    now = plat_time();
    lang_frame();
    S = gfx.height / 1080.0f;
    dl = ImGui::GetBackgroundDrawList();
    read_input(pad);
    cur_pad = pad;
    show_video = false;
    if (setup_on) {
        setup_screen();
        toast_draw();
        return;
    }
    if (now - setup_ended < 0.9) {
        /* after the setup: the home screen comes up out of black, calmly */
        float k = 1.0f - ease((float)((now - setup_ended) / 0.9));
        ImGui::GetForegroundDrawList()->AddRectFilled(P(0, 0), P(1920, 1080), IM_COL32(0, 0, 0, (int)(255 * k)));
    }
    if (modal == NONE && hit(PAD_OPTIONS)) {
        modal = OPTIONS;
        modal_focus = 0;
        modal_anim = 0;
        pressed &= ~PAD_OPTIONS;
    }
    modal_anim = approach(modal_anim, modal == NONE ? 0.0f : 1.0f, 14);

    /* A rescan (a drive plugged in, files copied) rebuilt the library: rebuild
     * the lists made from it. A drive that went away takes its folder with it. */
    static int last_generation = -1;
    static size_t last_roots;
    if (library_generation() != last_generation) {
        bool first = last_generation < 0;
        last_generation = library_generation();
        size_t roots_now = library_roots().size();
        if (!first && roots_now < last_roots)
            toast(plat_full_access() ? "Drive removed" : "Drive removed. Plugged back in? Options > Reload drives", 6);
        last_roots = roots_now;
        load_usb_theme();
        apply_theme();
        web_places_update();
        rebuild_lists();
        if (!browse_dir.empty()) {
            bool still_there = false;
            for (const std::string &r : library_roots())
                if (browse_dir.compare(0, r.size(), r) == 0)
                    still_there = true;
            if (!still_there) {
                browse_dir.clear();
                browse_focus = 0;
            }
        }
        rebuild_browse();
    }

    /* VLC asks something (a login, a certificate): its window comes up over
     * whatever is open, which comes back after. Its errors become toasts. */
    net_update();
    {
        NetAsk ask = net_ask();
        if (ask.serial && modal != LOGIN && modal != QUESTION) {
            ask_back = modal == OPTIONS ? NONE : modal;
            modal = ask.login ? LOGIN : QUESTION;
            modal_focus = 0;
            modal_anim = 0;
            pressed = 0;
        }
        std::string et, ex;
        if (net_pop_error(&et, &ex) && et != "Your input can't be opened") {
            std::string msg = vlc_message(ex.empty() ? et : ex);
            if (msg.size() > 140)
                msg = msg.substr(0, 137) + "...";
            if (!(tab == BROWSE && !net_path.empty() && screen != PLAYER))
                toast(msg, 5);
        }
    }

    if (screen == LIBRARY)
        music_tick();
    if (screen == LIBRARY && music_bg && modal == NONE && hit(PAD_L3)) {
        screen = PLAYER;
        screen_fade = 0;
        osd_until = now + 3.5;
        library_set_busy(true);
        pressed &= ~PAD_L3;
    }

    screen_fade = approach(screen_fade, 1, 8);
    Modal before = modal;
    if (screen == PLAYER) {
        uint32_t keep = pressed;
        if (modal != NONE && modal != TRACKS)
            pressed = 0; /* the menu has the buttons */
        player_screen();
        pressed = keep;
    } else if (screen == VIEWER) {
        uint32_t keep = pressed;
        if (modal != NONE)
            pressed = 0;
        viewer_screen();
        pressed = keep;
    } else {
        background();
        tab_bar();
        if (sub_pick)
            tab = BROWSE; /* picking: Browse only */
        if (modal == NONE && !sub_pick) {
            if (hit(PAD_L1))
                tab = (Tab)((tab + TABS - 1) % TABS);
            if (hit(PAD_R1))
                tab = (Tab)((tab + 1) % TABS);
        }
        uint32_t keep = pressed;
        if (modal != NONE)
            pressed = 0;
        if (modal == NONE && !sub_pick && hit(PAD_R3)) {
            open_search();
            modal_anim = 1;
            pressed = 0;
        }
        if (modal == NONE && !sub_pick)
            library_actions();
        switch (tab) {
        case HOME: tab_home(); break;
        case VIDEOS: tab_videos(); break;
        case MUSIC: tab_music(); break;
        case PLAYLISTS: tab_playlists(); break;
        case BROWSE: tab_browse(); break;
        default: break;
        }
        pressed = keep;
        mini_player();
        if (modal == NONE) {
            bool open_kind = tab == BROWSE && browse_focus < (int)browse_entries.size() &&
                             (browse_entries[browse_focus].dir || browse_entries[browse_focus].net == NET_ADD);
            std::vector<std::pair<uint32_t, const char *>> h = { { PAD_CROSS, open_kind ? "Open" : "Play" } };
            int fi = focused_item();
            if (fi >= 0 && L()[fi].other) {
                h = { { PAD_SQUARE, "Details" } };
            } else if (fi >= 0) {
                h.push_back({ PAD_TRIANGLE, L()[fi].favourite ? "Unfavourite" : "Favourite" });
                h.push_back({ PAD_SQUARE, "Details" });
            } else if (tab == BROWSE && browse_focus < (int)browse_entries.size() &&
                       browse_entries[browse_focus].net == NET_SERVER && net_is_saved_server(browse_entries[browse_focus].path)) {
                h.push_back({ PAD_SQUARE, "Forget" });
            } else if (tab == BROWSE && browse_focus < (int)browse_entries.size() &&
                       browse_entries[browse_focus].net == NET_NONE &&
                       (browse_entries[browse_focus].playlist || !browse_dir.empty())) {
                h.push_back({ PAD_SQUARE, "Delete" });
            }
            if (tab == BROWSE && (!browse_dir.empty() || !net_path.empty()))
                h.push_back({ PAD_CIRCLE, "Back" });
            if (tab == PLAYLISTS)
                h = playlist_hints();
            if (sub_pick) {
                bool file = browse_focus < (int)browse_entries.size() && !browse_entries[browse_focus].dir &&
                            (browse_entries[browse_focus].net == NET_FILE || browse_entries[browse_focus].net == NET_NONE);
                h = { { PAD_CROSS, file ? "Add" : "Open" },
                      { PAD_CIRCLE, browse_dir.empty() && net_path.empty() ? "Cancel" : "Back" } };
            } else {
                h.push_back({ PAD_R3, "Search" });
                h.push_back({ PAD_OPTIONS, "Options" });
            }
            if (classic_look) {
                if (tab == PLAYLISTS) {
                    if (pl_open.empty())
                        status_text = trf("%d playlists", (int)library_playlists().size());
                } else if (tab != BROWSE) {
                    char st[64];
                    if (tab == VIDEOS)
                        snprintf(st, sizeof(st), tr("%zu videos"), videos_list.size());
                    else if (tab == MUSIC)
                        snprintf(st, sizeof(st), tr("%zu tracks"), music_list.size());
                    else
                        snprintf(st, sizeof(st), tr("%zu videos, %zu tracks"), videos_list.size(), music_list.size());
                    status_text = st;
                }
                status_bar();
                hints(1824, 1042, h);
            } else {
                hints(1824, 1030, h);
            }
        } else if (classic_look) {
            status_bar(); /* part of the window: it stays under a menu */
        }
        /* Fade in after leaving the player. */
        if (screen_fade < 0.99f)
            rect(0, 0, 1920, 1080, alpha(IM_COL32(0, 0, 0, 255), 1 - screen_fade));
    }
    /* The open menu. When it hands over to another one (Options > Settings,
     * Speaker test > Settings), that one is drawn in this same frame, with the
     * press already used: no blank frame between them. Each pass draws on its
     * own layer, and the one that handed over is dropped: drawn too, its dim
     * and panel showed under the new one for that frame (a dark blink). */
    dl->ChannelsSplit(3);
    dl->ChannelsSetCurrent(1);
    bool handed_over = false;
    for (int pass = 0; pass < 2; pass++) {
        if (before != modal)
            break;
        switch (modal) {
        case OPTIONS: modal_options(); break;
        case RESUME: modal_resume(); break;
        case INFO: modal_info(); break;
        case DETAILS: modal_details(); break;
        case PL_PICK: modal_pl_pick(); break;
        case PL_NAME: modal_pl_name(); break;
        case SETTINGS: modal_settings(); break;
        case SPEAKERS: modal_speakers(); break;
        case PHONE: modal_phone(); break;
        case SEARCH: case LINK: modal_text_screen(); break;
        case CONFIRM_DELETE: modal_confirm_delete(); break;
        case TEXT_VIEW: modal_text_view(); break;
        case ADD_SHARE: modal_add_share(); break;
        case LOGIN: modal_login(); break;
        case QUESTION: modal_question(); break;
        case OSUB_SETUP: modal_osub_setup(); break;
        case OSUB_RESULTS: if (screen == PLAYER) modal_osub_results(); else modal = NONE; break;
        case TRACKS: if (screen == PLAYER) modal_tracks(); else modal = NONE; break;
        default: break;
        }
        if (modal == before || modal == NONE)
            break;
        before = modal;
        pressed = 0;
        modal_anim = 1;
        dl->ChannelsSetCurrent(2);
        handed_over = true;
    }
    if (handed_over) {
        dl->_Splitter._Channels[1]._CmdBuffer.resize(0);
        dl->_Splitter._Channels[1]._IdxBuffer.resize(0);
    }
    dl->ChannelsMerge();
    web_frame();
    upload_pill();
    toast_draw();
    /* Forget animations of things no longer drawn. */
    anims.erase(std::remove_if(anims.begin(), anims.end(), [](const Anim &x) { return now - x.seen > 2; }),
                anims.end());
}

bool ui_video_rect(float *x, float *y, float *w, float *h)
{
    if (!show_video)
        return false;
    *x = vid_rect[0];
    *y = vid_rect[1];
    *w = vid_rect[2];
    *h = vid_rect[3];
    return true;
}

bool ui_quit_requested()
{
    return quit;
}
