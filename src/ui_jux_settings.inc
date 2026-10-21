/*
 * VLC-PS5: "Redesign by Juxtopposed" (PLAN-REDESIGN.txt): Settings, as her
 * "Settings - global" and "Settings - audio" frames. A large rounded window:
 * the categories on the left in groups, the chosen one's page on the right
 * (its title in the top bar, then each setting with its control on the
 * right: ON/OFF switches, value boxes, the accent colours). Included into
 * ui.cc just before modal_settings (it uses the settings' tables).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

double jux_set_at = -10;  /* the page came (a category change): its rows enter */
int jux_set_seen = -1;
float jux_cat_y = -1;

/* the design's group names over the categories: Media (Playback, Video,
 * Sound), App (Interface, Playlists), System */
const char *jux_cat_group(int i)
{
    return i == 0 ? "Media" : i == 3 ? "App" : i == 5 ? "System" : nullptr;
}

void jux_settings_draw(const SettingsCat &cat, float a)
{
    if (jux_set_seen != settings_cat) {
        jux_set_seen = settings_cat;
        jux_set_at = now;
    }
    int rows = (int)cat.rows.size();
    dim_screen(a);
    const float w = 1640, h = 960, rail = 400;
    float x = 960 - w / 2, y = 540 - h / 2 + (1 - a) * 40;
    glow(x, y + 16, w, h, 18, alpha(IM_COL32(0, 0, 0, 255), J_SHADE * a), 30, 6);
    rect(x, y, w, h, alpha(J_BG, a), 18);

    /* the rail: the categories in their groups, flush with the window's
     * left edge (no gap); the chosen one's box glides */
    float rx = x, ry0 = y, rw = rail;
    dl->AddRectFilled(P(x, y), P(x + rail, y + h), alpha(J_PANEL, a), 18 * S, ImDrawFlags_RoundCornersLeft);
    rect(x + rail - 1, y, 1, h, alpha(J_LINE, a));
    text(f_semi, 28, rx + 28, ry0 + 26, alpha(J_TEXT, a), "Settings");
    float ry = ry0 + 92;
    float cat_y[SETTINGS_CATS];
    for (int i = 0; i < SETTINGS_CATS; i++) {
        if (const char *g = jux_cat_group(i)) {
            text(f_reg, 20, rx + 28, ry + 10, alpha(J_FAINT, a), g);
            ry += 44;
        }
        cat_y[i] = ry;
        ry += 66;
    }
    if (jux_cat_y < 0)
        jux_cat_y = cat_y[settings_cat];
    jux_cat_y = approach(jux_cat_y, cat_y[settings_cat], 14);
    stroke(x, y, w, h, alpha(J_LINE, a), 18, 1.5f); /* the window's edge, over both */
    float fo = focus_anim(940, !settings_inside, 12);
    rect(rx + 12, jux_cat_y, rw - 24, 56, alpha(J_RAISED, a), 10);
    if (fo > 0.01f) {
        glow(rx + 12, jux_cat_y + 14, 4, 28, 2, alpha(C_ORANGE, 0.6f * fo * a), 8, 4);
        rect(rx + 12, jux_cat_y + 14, 4, 28, alpha(C_ORANGE, fo * a), 2);
    }
    for (int i = 0; i < SETTINGS_CATS; i++) {
        bool sel = i == settings_cat;
        float f = ease(focus_anim(hash_id(settings_cats[i].name, 950), sel, 12));
        ImU32 col = alpha(mix(J_DIM, J_TEXT, f), a);
        category_icon(settings_cats[i].icon, rx + 52, cat_y[i] + 28, 28, alpha(mix(J_DIM, C_ORANGE, f), a));
        text(sel ? f_semi : f_reg, 24, rx + 88, cat_y[i] + 14, col, settings_cats[i].name);
    }

    /* the page: its title in the top bar, ○ closes (the design's ✕) */
    float px = x + rail + 32, pw = w - rail - 80;
    /* the top bar meets the rail and the window's top-right corner exactly */
    dl->AddRectFilled(P(x + rail, y), P(x + w, y + 96), alpha(J_BAR, a), 18 * S, ImDrawFlags_RoundCornersTopRight);
    rect(x + rail, y + 96, w - rail, 1, alpha(J_LINE, a));
    float tk = ease((float)(now - jux_set_at) / 0.35f);
    text(f_reg, 30, px + (1 - tk) * 24, y + 30, alpha(J_TEXT, a * tk), cat.name);
    glyph(x + w - 54, y + 48, 20, PAD_CIRCLE, a * 0.8f);

    /* the rows: each enters a little after the one above */
    const float row_h = 84, list_top = y + 124, list_bottom = y + h - 80;
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
    dl->PushClipRect(P(px - 24, list_top - 6), P(px + pw + 30, list_bottom), true);
    for (int r = 0; r < rows; r++) {
        int id = cat.rows[r];
        float k = ease(((float)(now - jux_set_at) - 0.045f * r) / 0.4f) * a;
        float yy = list_top + r * row_h - scroll + (1 - k) * 18;
        bool foc = settings_inside && r == modal_focus;
        float f = ease(focus_anim(hash_id(st_labels[id], 960), foc, 14));
        if (f > 0.01f) {
            rect(px - 16, yy, pw + 32, row_h - 10, alpha(J_RAISED, f * k), 10);
            glow(px - 16, yy + 18, 4, row_h - 46, 2, alpha(C_ORANGE, 0.5f * f * k), 8, 4);
            rect(px - 16, yy + 18, 4, row_h - 46, alpha(C_ORANGE, f * k), 2);
        }
        float ly = foc ? yy + 10 : yy + 22;
        const char *label = id == ST_THEME ? "Accent colour" : st_labels[id]; /* (the design's name) */
        text(f_reg, 24, px + 8, ly, alpha(mix(J_DIM, J_TEXT, 0.45f + 0.55f * f), k), label);
        /* the design's (i): our help line, shown under the name while focused */
        float lw = text_size(f_reg, 24, label).x;
        ring(px + 8 + lw + 22, ly + 14, 10, alpha(J_FAINT, k), 1.5f);
        text(f_semi, 14, px + 8 + lw + 22, ly + 4, alpha(J_FAINT, k), "i", 0.5f);
        if (foc)
            text(f_reg, 18, px + 8, yy + 44, alpha(J_DIM, k * f),
                 id == ST_THEME ? "The colour of the buttons, sliders and selection" : st_help[id], 0, pw - 420);

        std::string v = st_value(id);
        float right = px + pw;
        bool fixed = id == ST_STORAGE || id == ST_VERSION;
        bool action = id == ST_SPEAKERS || id == ST_RELOAD || id == ST_ABOUT || id == ST_SETUP;
        if (action) {
            const char *act = id == ST_RELOAD ? "Restart" : "Open";
            float bw = text_size(f_reg, 22, act).x + 60;
            rect(right - bw, yy + 16, bw, 44, alpha(mix(J_CTRL, C_ORANGE2, f), k), 8);
            text(f_reg, 22, right - bw + 20, yy + 26, alpha(mix(J_DIM, J_TEXT, f), k), act);
            icon_chevron(right - 20, yy + 38, 14, alpha(mix(J_DIM, C_ORANGE, f), k), false);
        } else if (fixed || id == ST_OSUB) {
            text(f_reg, 22, right, yy + 26, alpha(id == ST_OSUB && v != "Not set" ? J_TEXT : J_DIM, k), v.c_str(), 1);
        } else if (id == ST_JUXTHEME) {
            /* the design's segments: Dark | Light, the chosen one in the accent */
            bool light = pref_int("jux_light", 0) != 0;
            const float sw = 150;
            float sx = right - 2 * sw - 6;
            rect(sx - 4, yy + 13, 2 * sw + 14, 50, alpha(J_BAR, k), 8);
            for (int c = 0; c < 2; c++) {
                bool on = (c == 1) == light;
                float cf = ease(focus_anim(980 + c, on, 12));
                float bx = sx + c * (sw + 6);
                rect(bx, yy + 17, sw, 42, alpha(mix(J_CTRL, C_ORANGE2, cf), k), 6);
                text(f_reg, 21, bx + sw / 2, yy + 26, alpha(mix(J_DIM, C_ORANGE, cf), k), c ? "Light" : "Dark", 0.5f);
            }
        } else if (id == ST_THEME) {
            /* the design's nine accent colours; the chosen one in a frame */
            int cur = pref_int("jux_accent", 0);
            float sx = right - 9 * 46;
            rect(sx - 6, yy + 13, 9 * 46 + 6, 50, alpha(J_BAR, k), 8);
            for (int c = 0; c < 9; c++) {
                float cf = ease(focus_anim(970 + c, c == cur, 12));
                float cx = sx + c * 46 + 20;
                if (cf > 0.01f)
                    rect(cx - 20, yy + 17, 40, 42, alpha(mix(J_BAR, C_ORANGE2, 1.0f), cf * k), 6);
                rect(cx - 13 - 2 * cf, yy + 25 - 2 * cf, 26 + 4 * cf, 26 + 4 * cf, alpha(jux_accents[c], k), 4);
            }
        } else if ((v == "On" || v == "Off") && id != ST_UPSCALE) { /* (upscaling is a list) */
            jux_switch(right - 92, yy + 19, v == "On", k, hash_id(st_labels[id], 961));
        } else {
            jux_value_box(right, yy + 16, 380, v, f, k);
        }
    }
    dl->PopClipRect();
    if (content > view) {
        float bh = view * view / content, by2 = list_top + (view - bh) * (scroll / (content - view));
        rect(px + pw + 22, by2, 4, bh, alpha(J_EDGE, a), 2);
    }
    int fid = settings_inside && modal_focus < rows ? cat.rows[modal_focus] : -1;
    if (fid == ST_STORAGE || fid == ST_VERSION)
        hints(x + w - 40, y + h - 40, { { PAD_CIRCLE, "Back" } }, a);
    else if (settings_inside)
        hints(x + w - 40, y + h - 40, { { PAD_CROSS, fid == ST_SPEAKERS || fid == ST_ABOUT ? "Open" : fid == ST_RELOAD ? "Restart" : "Change" }, { PAD_CIRCLE, "Back" } }, a);
    else
        hints(x + w - 40, y + h - 40, { { PAD_CROSS, "Open" }, { PAD_CIRCLE, "Close" } }, a);
}
