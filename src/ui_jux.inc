/*
 * VLC-PS5: the "Redesign by Juxtopposed" look (PLAN-REDESIGN.txt).
 *
 * Her "VLC Redesign" Figma design (CC BY 4.0), adapted for a TV and a
 * controller: our own drawing code, the design's layout, sizes and colours.
 * Included into ui.cc (inside its namespace), so it uses the same helpers
 * and state as the other two looks.
 *
 * The library: a sidebar on the left (the tabs), a top bar (back,
 * breadcrumbs, the count, search), and the tab's content drawn into the
 * rest of the screen (P() moved and scaled: jux_content_begin/end). The
 * motion is the setup's: parts fade in one after another, drifting up into
 * place; the selection glides; the accent breathes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */


/* the sidebar and the content's place (layout units at full scale) */
const float JX_SIDE_X = 24, JX_SIDE_W = 276, JX_SIDE_Y = 24;
const float JX_CONTENT_X = 324, JX_TOP_H = 104;
/* the tab's own layout (1920 wide) shown from x 60 to 1860 in 324..1896 */
const float JX_K = (1896.0f - JX_CONTENT_X) / 1800.0f;

void icon_search(float cx, float cy, float s, ImU32 col);

/* a thin chevron, left or right (the design's back / forward arrows) */
void icon_chevron(float cx, float cy, float s, ImU32 col, bool left)
{
    float d = left ? -1 : 1, h = s * 0.42f;
    line(cx - d * h * 0.5f, cy - h, cx + d * h * 0.5f, cy, col, 2.4f);
    line(cx + d * h * 0.5f, cy, cx - d * h * 0.5f, cy + h, col, 2.4f);
}

/* a controller button's name on a small key (L1, R1, R3) */
void jux_key(float cx, float cy, const char *label)
{
    rect(cx - 20, cy - 13, 40, 26, J_RAISED, 6);
    stroke(cx - 20, cy - 13, 40, 26, J_EDGE, 6, 1.2f);
    text(f_bold, 15, cx, cy - 9, J_DIM, label, 0.5f);
}

/* The design's sidebar icons (her library frame), drawn here: a house with
 * a door, a play button in a rounded square, a folder with a note, lines
 * with a play mark, a folder. The selected one is filled in the accent. */
void jux_poly(std::initializer_list<ImVec2> pts, float cx, float cy, float s, ImU32 col, bool fill, float t = 2.2f)
{
    for (const ImVec2 &q : pts)
        dl->PathLineTo(P(cx + q.x * s, cy + q.y * s));
    if (fill)
        dl->PathFillConvex(col);
    else
        dl->PathStroke(col, ImDrawFlags_Closed, t * S);
}

void jux_tab_icon(int i, float cx, float cy, float s, ImU32 col, bool sel)
{
    ImU32 hole = J_PANEL;
    switch (i) {
    case HOME:
        jux_poly({ { -0.42f, -0.06f }, { 0, -0.44f }, { 0.42f, -0.06f }, { 0.42f, 0.42f }, { -0.42f, 0.42f } }, cx, cy, s,
                 col, sel);
        if (sel)
            jux_poly({ { -0.1f, 0.42f }, { -0.1f, 0.12f }, { 0.1f, 0.12f }, { 0.1f, 0.42f } }, cx, cy, s, hole, true);
        else
            jux_poly({ { -0.1f, 0.42f }, { -0.1f, 0.14f }, { 0.1f, 0.14f }, { 0.1f, 0.42f } }, cx, cy, s, col, false);
        break;
    case VIDEOS:
        if (sel)
            rect(cx - 0.44f * s, cy - 0.44f * s, 0.88f * s, 0.88f * s, col, 0.16f * s);
        else
            stroke(cx - 0.42f * s, cy - 0.42f * s, 0.84f * s, 0.84f * s, col, 0.16f * s, 2.2f);
        tri(cx - 0.12f * s, cy - 0.2f * s, cx + 0.22f * s, cy, cx - 0.12f * s, cy + 0.2f * s, sel ? hole : col);
        break;
    case MUSIC:
    case BROWSE:
        /* a folder: its tab top left */
        /* centred on the line like the others (it sat low), a touch narrower */
        jux_poly({ { -0.4f, -0.4f }, { -0.1f, -0.4f }, { 0, -0.27f }, { 0.4f, -0.27f }, { 0.4f, 0.34f },
                   { -0.4f, 0.34f } },
                 cx, cy, s, col, false);
        if (sel)
            rect(cx - 0.4f * s, cy - 0.27f * s, 0.8f * s, 0.61f * s, col, 0.06f * s);
        if (i == MUSIC) {
            ImU32 n = sel ? hole : col;
            circle(cx - 0.06f * s, cy + 0.13f * s, 0.1f * s, n);
            line(cx + 0.03f * s, cy + 0.13f * s, cx + 0.03f * s, cy - 0.15f * s, n, 2.2f);
            line(cx + 0.03f * s, cy - 0.15f * s, cx + 0.18f * s, cy - 0.07f * s, n, 2.2f);
        }
        break;
    case PLAYLISTS:
        line(cx - 0.42f * s, cy - 0.28f * s, cx + 0.42f * s, cy - 0.28f * s, col, 2.2f);
        line(cx - 0.42f * s, cy, cx + 0.18f * s, cy, col, 2.2f);
        line(cx - 0.42f * s, cy + 0.28f * s, cx + 0.02f * s, cy + 0.28f * s, col, 2.2f);
        tri(cx + 0.14f * s, cy + 0.08f * s, cx + 0.42f * s, cy + 0.26f * s, cx + 0.14f * s, cy + 0.44f * s, col);
        break;
    default:
        break;
    }
}

/* The design's cone (her onboarding frame): flat, one colour, its bands
 * cut out (the panel shows through) instead of the white stripes of the
 * other looks' cone. (cx, top) and the height. */
void jux_cone(float cx, float top, float h, ImU32 col)
{
    /* the cone's half width down its body (0 at the tip, 1 at the base plate) */
    auto half = [&](float t) { return h * (0.07f + 0.33f * t); };
    auto band = [&](float t0, float t1) {
        float y0 = top + t0 * h * 0.78f, y1 = top + t1 * h * 0.78f;
        dl->AddQuadFilled(P(cx - half(t0), y0), P(cx + half(t0), y0), P(cx + half(t1), y1), P(cx - half(t1), y1), col);
    };
    circle(cx, top + h * 0.05f, h * 0.075f, col); /* the round tip */
    band(0.05f, 0.25f);
    band(0.38f, 0.58f);
    band(0.71f, 1.0f);
    /* the base plate, wider, rounded */
    rect(cx - h * 0.5f, top + h * 0.78f, h, h * 0.22f, col, h * 0.06f);
}

double jux_enter_at = -10;   /* the content's entrance (a tab or folder change) */
std::string jux_where;       /* what the content shows: a new one enters */
float jux_sel_y = -1, jux_sel_top = -1, jux_sel_bot = -1;

/* the entrance of a part at (x, y) of the content: 0 to 1, a little later
 * down and to the right, like the setup's rows */
float jux_in(float x, float y)
{
    float delay = 0.05f * std::max(0.0f, (y - 120) / 330) + 0.03f * std::max(0.0f, (x - 60) / 400);
    float t = ((float)(now - jux_enter_at) - std::min(delay, 0.45f)) / 0.5f;
    return ease(t);
}

void jux_background()
{
    rect(0, 0, 1920, 1080, J_BG);
    /* a slow, faint light of the accent at the top right: barely there */
    float breath = 0.75f + 0.25f * sinf((float)now * 0.45f);
    radial(1700, -120, 900, alpha(C_ORANGE, 0.07f * breath));
}

/* ---- the tab's content in its place -------------------------------------------------- */

float jx_s0;
std::vector<std::string> jux_crumbs();
double jux_crumb_at = -10;

void jux_content_begin()
{
    /* a tab or folder changed this frame (L1 / R1 are read after the top bar
     * is drawn): its entrance starts now, so the new content never shows at
     * full for a frame first (the user saw it flick) */
    std::string where;
    for (auto &c : jux_crumbs())
        where += c + "\n";
    if (where != jux_where) {
        jux_where = where;
        jux_enter_at = now;
        jux_crumb_at = now;
    }
    jx_s0 = S;
    dl->PushClipRect(P(JX_CONTENT_X - 12, JX_TOP_H + 6), P(1920, 1080), true);
    S = jx_s0 * JX_K;
    OX = (JX_CONTENT_X - 60 * JX_K) * jx_s0;
    OY = (JX_TOP_H + 16 - 112 * JX_K) * jx_s0;
}

void jux_content_end()
{
    S = jx_s0;
    OX = OY = 0;
    /* and the whole page fades in over its first moments, under the parts'
     * own staggered entrances: nothing pops */
    float k = ease((float)(now - jux_enter_at) / 0.28f);
    if (k < 1)
        rect(JX_CONTENT_X - 12, JX_TOP_H + 6, 1920 - JX_CONTENT_X + 12, 1080 - JX_TOP_H - 6, alpha(J_BG, 1 - k));
    dl->PopClipRect();
}

/* ---- the sidebar -------------------------------------------------------------------- */

void jux_sidebar()
{
    float x = JX_SIDE_X, y = JX_SIDE_Y, w = JX_SIDE_W, h = 1080 - 2 * JX_SIDE_Y;
    rect(x, y, w, h, J_PANEL, 14);
    /* the brand: the design's flat cone, and VLC beside it */
    jux_cone(x + 48, y + 30, 46, C_ORANGE);
    text(f_semi, 30, x + 84, y + 36, J_TEXT, "VLC");

    const float iy = y + 168, ih = 66;
    float target = iy + tab * ih;
    if (jux_sel_y < 0)
        jux_sel_y = jux_sel_top = jux_sel_bot = target;
    /* the selection glides, its leading edge first (the Modern pill's stretch) */
    bool down = target > jux_sel_y;
    jux_sel_y = approach(jux_sel_y, target, 12);
    jux_sel_top = approach(jux_sel_top, target, down ? 8 : 18);
    jux_sel_bot = approach(jux_sel_bot, target, down ? 18 : 8);
    rect(x + 14, jux_sel_top, w - 28, jux_sel_bot - jux_sel_top + ih - 8, J_RAISED, 10);
    float breathe = 0.8f + 0.2f * sinf((float)now * 2.2f);
    glow(x + 14, jux_sel_top + 14, 5, jux_sel_bot - jux_sel_top + ih - 36, 3, alpha(C_ORANGE, 0.7f * breathe), 10, 5);
    rect(x + 14, jux_sel_top + 14, 5, jux_sel_bot - jux_sel_top + ih - 36, C_ORANGE, 3);

    /* when the library first shows, the names slide in one after another */
    static double side_at = -1;
    if (side_at < 0)
        side_at = now;
    for (int i = 0; i < TABS; i++) {
        float f = focus_anim(30000 + i, i == tab, 10);
        float ry = iy + i * ih;
        float k = ease(((float)(now - side_at) - 0.07f * i) / 0.55f), sx = (1 - k) * -26;
        ImU32 col = alpha(mix(J_DIM, J_TEXT, f), k);
        /* one column: the icons and names line up whichever is chosen */
        jux_tab_icon(i, x + 52 + sx, ry + (ih - 8) / 2, 26, alpha(mix(J_DIM, C_ORANGE, f), k), i == tab);
        text(i == tab ? f_semi : f_reg, 25, x + 86 + sx, ry + (ih - 8) / 2 - 15, col, tab_names[i]);
    }
    /* the list's name, and L1 / R1 that move through it, on one line above it */
    text(f_reg, 19, x + 34, iy - 38, J_FAINT, "Library");
    jux_key(x + w - 82, iy - 26, "L1");
    jux_key(x + w - 36, iy - 26, "R1");

    /* the menu, at the bottom (search is the field at the top) */
    float by = y + h - 78;
    rect(x + 24, by - 14, w - 48, 1, J_LINE);
    icon_gear(x + 50, by + 24, 24, J_DIM);
    text(f_reg, 23, x + 82, by + 9, J_DIM, "Options");
    glyph(x + w - 46, by + 24, 20, PAD_OPTIONS, 0.8f);
}

/* ---- the top bar: back, where you are, the count, search ------------------------------ */

std::vector<std::string> jux_crumbs()
{
    std::vector<std::string> c = { tab_names[tab] };
    if (tab == BROWSE) {
        if (!net_path.empty()) {
            for (auto &p : net_path)
                c.push_back(p.second);
        } else if (!browse_dir.empty()) {
            std::string root;
            for (const std::string &r : library_roots())
                if (browse_dir.compare(0, r.size(), r) == 0 && r.size() > root.size())
                    root = r;
            std::string rest = browse_dir.substr(root.size());
            std::string rname = root;
            while (rname.size() > 1 && rname.back() == '/')
                rname.pop_back();
            rname = rname.substr(rname.rfind('/') + 1);
            c.push_back(rname.empty() ? "/" : rname);
            size_t p = 0;
            while (p < rest.size()) {
                size_t q = rest.find('/', p);
                if (q == std::string::npos)
                    q = rest.size();
                if (q > p)
                    c.push_back(rest.substr(p, q - p));
                p = q + 1;
            }
        }
    } else if (tab == PLAYLISTS && !pl_open.empty()) {
        const PlaylistFile *pl = playlist_by_path(pl_open);
        c.push_back(pl ? pl->name : pl_open.substr(pl_open.rfind('/') + 1));
    }
    return c;
}

void jux_top_bar()
{
    std::vector<std::string> crumbs = jux_crumbs();
    std::string where;
    for (auto &s : crumbs)
        where += s + "\n";
    if (where != jux_where) {
        jux_where = where;
        jux_enter_at = now;
        jux_crumb_at = now;
    }
    const float x0 = JX_CONTENT_X, cy = 64;
    bool can_back = (tab == BROWSE && (!browse_dir.empty() || !net_path.empty())) || (tab == PLAYLISTS && !pl_open.empty());
    /* back and forward, as the design (forward stays faint: a controller goes back with ○) */
    icon_chevron(x0 + 14, cy, 22, can_back ? J_TEXT : J_FAINT, true);
    icon_chevron(x0 + 52, cy, 22, J_EDGE, false);

    /* the crumbs: the last one bold, slid in when it changes; long paths
     * show their last four */
    float x = x0 + 90;
    int first = std::max(0, (int)crumbs.size() - 4);
    if (first > 0) {
        text(f_reg, 25, x, cy - 16, J_DIM, "\xE2\x80\xA6");
        x += 30;
        tri(x + 2, cy - 6, x + 2, cy + 6, x + 10, cy, J_FAINT);
        x += 24;
    }
    float k = ease((float)(now - jux_crumb_at) / 0.35f);
    for (int i = first; i < (int)crumbs.size(); i++) {
        bool last = i + 1 == (int)crumbs.size();
        ImFont *f = last ? f_bold : f_reg;
        float max_w = last ? 560 : 220;
        float tw = std::min(text_size(f, 25, crumbs[i].c_str()).x, max_w);
        float sx = last ? (1 - k) * 26 : 0;
        text(f, 25, x + sx, cy - 16, alpha(last ? J_TEXT : J_DIM, last ? k : 1), crumbs[i].c_str(), 0, max_w);
        x += tw + 16;
        if (!last) {
            tri(x, cy - 6, x, cy + 6, x + 8, cy, J_FAINT);
            x += 24;
        }
    }

    /* right: how many, and the search field */
    size_t count = 0;
    switch (tab) {
    case HOME: count = videos_list.size() + music_list.size(); break;
    case VIDEOS: count = videos_list.size(); break;
    case MUSIC: count = music_list.size(); break;
    case PLAYLISTS: count = pl_open.empty() ? library_playlists().size() : playlist_items_cached(pl_open).size(); break;
    case BROWSE: count = browse_entries.size(); break;
    default: break;
    }
    const float sw = 360, sx = 1896 - sw;
    rect(sx, cy - 25, sw, 50, J_CTRL, 10);
    icon_search(sx + 30, cy, 22, J_DIM);
    text(f_reg, 22, sx + 56, cy - 14, J_FAINT, "Search");
    jux_key(sx + sw - 34, cy, "R3");
    std::string n = count == 1 ? std::string(tr("1 item")) : trf("%d items", (int)count);
    text(f_reg, 22, sx - 28, cy - 14, J_DIM, n.c_str(), 1);
    rect(x0, JX_TOP_H, 1896 - x0, 1, J_LINE);
}

void jux_shell()
{
    jux_sidebar();
    jux_top_bar();
}

/* ---- the design's pieces, in the tab's layout ------------------------------------------ */

/* A video: a rounded thumbnail and its name under it (the library frame).
 * Focused: an accent ring, a little bigger and higher, the name bright. */
void video_card_jux(float x, float y, float w, int item, float f)
{
    MediaItem &m = L()[item];
    float in = jux_in(x, y);
    if (in <= 0)
        return;
    y += (1 - in) * 28;
    float h = w * 9 / 16;
    bool square = m.audio;
    if (square)
        w = h; /* the design's music: a square cover, as tall as a video's thumbnail */
    float e = ease(f);
    float grow = 1 + 0.045f * e;
    float cw = w * grow, ch = h * grow;
    float cx = x - (cw - w) / 2, cy = y - (ch - h) / 2 - 8 * e;
    glow(cx, cy + 12, cw, ch, 12, alpha(IM_COL32(0, 0, 0, 255), J_SHADE * in * (0.6f + 0.4f * e)), 20, 6);
    if (e > 0.01f)
        glow(cx, cy, cw, ch, 12, alpha(C_ORANGE, 0.45f * e * in), 16, 6);
    if (m.thumb) {
        float ta = m.thumb_aspect > 0 ? m.thumb_aspect : 16.0f / 9;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, ca = square ? 1.0f : 16.0f / 9;
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
                            alpha(IM_COL32_WHITE, in), 12 * S);
    } else {
        rect(cx, cy, cw, ch, alpha(J_PANEL, in), 12);
        ImU32 ic = alpha(J_FAINT, in);
        if (m.audio)
            icon_note(cx + cw / 2, cy + ch / 2, ch * 0.3f, ic);
        else if (m.image)
            icon_picture(cx + cw / 2, cy + ch / 2, ch * 0.32f, ic);
        else
            icon_film(cx + cw / 2, cy + ch / 2, ch * 0.32f, ic);
    }
    if (m.favourite)
        icon_star(cx + 28, cy + 28, 26, alpha(C_ORANGE, in));
    if (m.length_ms > 0) {
        std::string len = format_time(m.length_ms);
        float tw = text_size(f_semi, 18, len.c_str()).x;
        rect(cx + cw - tw - 24, cy + ch - 38, tw + 14, 26, alpha(IM_COL32(0, 0, 0, 190), in), 6);
        text(f_semi, 18, cx + cw - tw - 17, cy + ch - 35, alpha(IM_COL32(240, 240, 240, 255), in), len.c_str());
    }
    /* where it was stopped: the design's accent line along the bottom */
    if (m.resume_ms > 0 && m.length_ms > 0) {
        float frac = std::min(1.0f, (float)m.resume_ms / m.length_ms);
        dl->PushClipRect(P(cx, cy + ch - 5), P(cx + cw * frac, cy + ch), true);
        rect(cx, cy, cw, ch, alpha(C_ORANGE, in), 12);
        dl->PopClipRect();
    }
    if (e > 0.01f)
        stroke(cx - 3, cy - 3, cw + 6, ch + 6, alpha(C_ORANGE, e * in), 14, 3);
    text(f_reg, 22, x, y + h + 18 + 2 * e, alpha(mix(J_DIM, J_TEXT, e), in), m.name.c_str(), 0, w);
    std::string meta = square && !m.artist.empty() ? m.artist : m.ext;
    if (m.resume_ms > 0)
        meta += "  \xC2\xB7  " + trf("%s left", format_time(m.length_ms - m.resume_ms).c_str());
    text(f_reg, 18, x, y + h + 48 + 2 * e, alpha(J_FAINT, in), meta.c_str(), 0, w);
}

/* A row (Music, Browse, Playlists): the raised grey of the design's
 * selected row, an accent mark, entering like the cards. */
void list_row_jux(float x, float y, float w, float h, float f, bool even)
{
    float in = jux_in(x, y);
    if (even)
        rect(x, y, w, h, alpha(J_STRIPE, in), 10);
    if (f > 0.01f) {
        float e = ease(f);
        rect(x, y, w, h, alpha(J_RAISED, e * in), 10);
        glow(x, y + 14, 5, h - 28, 3, alpha(C_ORANGE, 0.5f * e * in), 8, 4);
        rect(x, y + 14, 5, h - 28, alpha(C_ORANGE, e * in), 3);
    }
}

/* A heading of the home screen: "Continue watching ›" */
void jux_heading(float x, float y, const char *title, size_t count, float on)
{
    float in = jux_in(x, y);
    float dy = (1 - in) * 20;
    text(f_reg, 30, x, y + dy, alpha(mix(J_DIM, J_TEXT, 0.5f + 0.5f * on), in), title);
    (void)count; /* (the design shows none) */
    float tw = text_size(f_reg, 30, title).x;
    /* its ">" leans out a little while the row is the one in use */
    icon_chevron(x + tw + 24 + 5 * ease(on), y + 19 + dy, 20, alpha(mix(J_DIM, C_ORANGE, on), in), false);
}

/* ---- controls shared by the panel and Settings ---------------------------------------- */

/* the design's ON / OFF switch: a square that slides, its word beside it */
void jux_switch(float x, float y, bool on, float a, uint64_t id)
{
    float p = focus_anim(id, on, 14);
    const float w = 92, h = 38;
    rect(x, y, w, h, alpha(mix(J_BAR, C_ORANGE2, p), a), 7);
    stroke(x, y, w, h, alpha(J_LINE, a), 7, 1.5f);
    float kx = x + 4 + (w - 8 - 32) * p;
    rect(kx, y + 3, 32, h - 6, alpha(mix(J_EDGE, C_ORANGE, p), a), 5);
    text(f_semi, 17, on ? x + 10 : x + w - 10, y + 9, alpha(on ? C_ORANGE : J_DIM, a), on ? "ON" : "OFF", on ? 0 : 1);
}

/* the design's value box: dark, bordered, the value inside; ‹ › while focused */
void jux_value_box(float right, float y, float w, const std::string &v, float f, float a)
{
    float x = right - w;
    rect(x, y, w, 40, alpha(J_BAR, a), 8);
    stroke(x, y, w, 40, alpha(mix(J_LINE, C_ORANGE, 0.7f * f), a), 8, 1.5f);
    text(f_reg, 21, x + 16, y + 9, alpha(v == "Off" || v == "0 ms" ? J_DIM : J_TEXT, a), v.c_str(), 0, w - 52);
    if (f > 0.05f) {
        icon_chevron(right - 18, y + 20, 14, alpha(C_ORANGE, f * a), false);
        icon_chevron(x - 14, y + 20, 14, alpha(C_ORANGE, f * a), true);
    }
}

/* ---- the mini player (the design's library frame: the video keeps playing in
 * a card at the bottom right) ------------------------------------------------------ */

void jux_mini_player()
{
    MediaItem *m = library_find(playing_path);
    const float w = 440, h = 112;
    float x = 1896 - w, y = 1080 - 24 - 64 - h;
    static double shown_at = -1;
    if (shown_at < 0)
        shown_at = now;
    float k = ease((float)(now - shown_at) / 0.45f);
    y += (1 - k) * 30;
    glow(x, y + 10, w, h, 14, alpha(IM_COL32(0, 0, 0, 255), J_SHADE * k), 22, 6);
    rect(x, y, w, h, alpha(J_PANEL, k), 14);
    stroke(x, y, w, h, alpha(J_LINE, k), 14, 1.2f);
    /* the cover, then the name and who, the progress along the bottom */
    float cs = h - 24;
    if (m && m->thumb)
        dl->AddImageRounded(m->thumb, P(x + 12, y + 12), P(x + 12 + cs, y + 12 + cs), ImVec2(0, 0), ImVec2(1, 1),
                            alpha(IM_COL32_WHITE, k), 8 * S);
    else {
        rect(x + 12, y + 12, cs, cs, alpha(J_RAISED, k), 8);
        jux_icon(JI_NOTE, x + 12 + cs / 2, y + 12 + cs / 2, cs * 0.5f, alpha(C_ORANGE, k));
    }
    std::string title = m ? m->name : playing_name;
    std::string sub = m ? (m->artist.empty() ? m->folder.substr(m->folder.rfind('/') + 1) : m->artist) : "Live";
    float tx = x + 24 + cs;
    text(f_reg, 22, tx, y + 18, alpha(J_TEXT, k), title.c_str(), 0, w - cs - 120);
    text(f_reg, 18, tx, y + 48, alpha(J_DIM, k), sub.c_str(), 0, w - cs - 120);
    int64_t len = player_length(), t = player_time();
    float bw = w - cs - 120;
    rect(tx, y + h - 26, bw, 6, alpha(J_BAR, k), 3);
    if (len > 0)
        rect(tx, y + h - 26, bw * std::min(1.0f, (float)t / len), 6, alpha(C_ORANGE, k), 3);
    /* playing or paused, and L3 to open it */
    float bx = x + w - 52;
    if (player_paused())
        icon_pause(bx, y + 36, 18, alpha(J_DIM, k));
    else
        icon_play(bx + 2, y + 36, 18, alpha(C_ORANGE, k));
    jux_key(bx, y + h - 26, "L3");
}
