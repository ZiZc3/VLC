/*
 * VLC-PS5: "Redesign by Juxtopposed" (PLAN-REDESIGN.txt): the design's icon
 * style, our own drawings: thin outlines (2.2 units), rounded, one colour.
 * The shared icon functions (icon_folder, icon_film...) draw these in this
 * look, so Browse, the cards, menus and the player all match. Included into
 * ui.cc right after the drawing helpers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The design's colours (measured from its frames), dark or light (Settings >
 * Interface > Theme): jux_set_theme fills them in. */
ImU32 J_BG, J_BAR, J_PANEL, J_RAISED, J_CTRL, J_LINE, J_TEXT, J_DIM, J_FAINT;
ImU32 J_POP;     /* menus, dialogs, popups */
ImU32 J_TABS;    /* the side panel's tab strip */
ImU32 J_EDGE;    /* keycaps' edges, slider ticks */
ImU32 J_STRIPE;  /* every other row */
ImU32 J_INK;     /* drawn on the accent (the play button's icon) */
float J_SHADE;   /* how dark shadows are: soft on the light theme */
bool jux_light;

void jux_set_theme(bool light)
{
    jux_light = light;
    if (!light) {
        J_BG = IM_COL32(14, 14, 14, 255);
        J_BAR = IM_COL32(9, 9, 9, 255);
        J_PANEL = IM_COL32(24, 24, 24, 255);
        J_RAISED = IM_COL32(34, 34, 34, 255);
        J_CTRL = IM_COL32(25, 25, 25, 255);
        J_LINE = IM_COL32(32, 32, 32, 255);
        J_TEXT = IM_COL32(224, 224, 224, 255);
        J_DIM = IM_COL32(143, 143, 143, 255);
        J_FAINT = IM_COL32(82, 82, 82, 255);
        J_POP = IM_COL32(27, 27, 27, 255);
        J_TABS = IM_COL32(16, 16, 16, 255);
        J_EDGE = IM_COL32(56, 56, 56, 255);
        J_STRIPE = IM_COL32(255, 255, 255, 5);
    } else {
        /* the light frame: soft greys, dark text */
        J_BG = IM_COL32(233, 233, 232, 255);
        J_BAR = IM_COL32(246, 246, 245, 255);
        J_PANEL = IM_COL32(218, 218, 217, 255);
        J_RAISED = IM_COL32(203, 203, 202, 255);
        J_CTRL = IM_COL32(224, 224, 223, 255);
        J_LINE = IM_COL32(196, 196, 195, 255);
        J_TEXT = IM_COL32(28, 28, 28, 255);
        J_DIM = IM_COL32(92, 92, 92, 255);
        J_FAINT = IM_COL32(140, 140, 140, 255);
        J_POP = IM_COL32(242, 242, 241, 255);
        J_TABS = IM_COL32(208, 208, 207, 255);
        J_EDGE = IM_COL32(176, 176, 175, 255);
        J_STRIPE = IM_COL32(0, 0, 0, 7);
    }
    J_INK = IM_COL32(18, 18, 18, 255);
    J_SHADE = light ? 0.28f : 1.0f;
}

/* the shared colours (C_BG, C_TEXT...) the other looks' pieces use, from the
 * theme's set */
void jux_look_colours()
{
    jux_set_theme(pref_int("jux_light", 0) != 0);
    C_BG = J_BG;
    C_CARD = J_PANEL;
    C_CARD_HI = J_RAISED;
    C_TEXT = J_TEXT;
    C_DIM = J_DIM;
    C_FAINT = J_FAINT;
}

enum JuxIcon {
    JI_FOLDER, JI_VIDEO, JI_NOTE, JI_DOC, JI_PICTURE, JI_NETWORK, JI_TV, JI_RADIO, JI_PLUS, JI_GEAR,
    JI_REFRESH, JI_PLAY, JI_STOP, JI_PLAYLIST_ADD, JI_PENCIL, JI_TRASH, JI_PHONE, JI_SEARCH, JI_LINK
};

const float JT = 2.2f; /* the design's stroke */

void jux_path(std::initializer_list<ImVec2> pts, float cx, float cy, float s, ImU32 col, bool closed)
{
    for (const ImVec2 &q : pts)
        dl->PathLineTo(P(cx + q.x * s, cy + q.y * s));
    dl->PathStroke(col, closed ? ImDrawFlags_Closed : 0, JT * S);
}

void jux_arc(float cx, float cy, float r, float a0, float a1, ImU32 col)
{
    dl->PathArcTo(P(cx, cy), r * S, a0, a1, 24);
    dl->PathStroke(col, 0, JT * S);
}

void jux_icon(int kind, float cx, float cy, float s, ImU32 col)
{
    switch (kind) {
    case JI_FOLDER:
        jux_path({ { -0.42f, -0.34f }, { -0.1f, -0.34f }, { 0, -0.22f }, { 0.42f, -0.22f }, { 0.42f, 0.36f }, { -0.42f, 0.36f } },
                 cx, cy, s, col, true);
        break;
    case JI_VIDEO:
        stroke(cx - 0.42f * s, cy - 0.38f * s, 0.84f * s, 0.76f * s, col, 0.14f * s, JT);
        jux_path({ { -0.1f, -0.17f }, { 0.18f, 0 }, { -0.1f, 0.17f } }, cx, cy, s, col, true);
        break;
    case JI_NOTE:
        ring(cx - 0.16f * s, cy + 0.24f * s, 0.13f * s, col, JT);
        jux_path({ { -0.03f, 0.24f }, { -0.03f, -0.36f }, { 0.3f, -0.24f }, { 0.3f, -0.08f }, { -0.03f, -0.2f } }, cx, cy, s,
                 col, false);
        break;
    case JI_DOC:
        jux_path({ { -0.32f, -0.42f }, { 0.12f, -0.42f }, { 0.32f, -0.22f }, { 0.32f, 0.42f }, { -0.32f, 0.42f } }, cx, cy, s,
                 col, true);
        jux_path({ { 0.12f, -0.42f }, { 0.12f, -0.22f }, { 0.32f, -0.22f } }, cx, cy, s, col, false);
        line(cx - 0.16f * s, cy + 0.02f * s, cx + 0.16f * s, cy + 0.02f * s, col, JT);
        line(cx - 0.16f * s, cy + 0.2f * s, cx + 0.1f * s, cy + 0.2f * s, col, JT);
        break;
    case JI_PICTURE:
        stroke(cx - 0.42f * s, cy - 0.34f * s, 0.84f * s, 0.68f * s, col, 0.1f * s, JT);
        jux_path({ { -0.3f, 0.22f }, { -0.08f, -0.02f }, { 0.06f, 0.12f }, { 0.16f, 0.02f }, { 0.3f, 0.22f } }, cx, cy, s, col,
                 false);
        circle(cx + 0.16f * s, cy - 0.14f * s, 0.06f * s, col);
        break;
    case JI_NETWORK: /* the design's "Network": a dot with arcs both sides */
        circle(cx, cy, 0.08f * s, col);
        jux_arc(cx, cy, 0.24f * s, 2.4f, 3.9f, col);
        jux_arc(cx, cy, 0.24f * s, -0.75f, 0.75f, col);
        jux_arc(cx, cy, 0.42f * s, 2.45f, 3.85f, col);
        jux_arc(cx, cy, 0.42f * s, -0.7f, 0.7f, col);
        break;
    case JI_TV:
        stroke(cx - 0.42f * s, cy - 0.32f * s, 0.84f * s, 0.56f * s, col, 0.08f * s, JT);
        line(cx - 0.18f * s, cy + 0.38f * s, cx + 0.18f * s, cy + 0.38f * s, col, JT);
        break;
    case JI_RADIO:
        stroke(cx - 0.42f * s, cy - 0.16f * s, 0.84f * s, 0.54f * s, col, 0.08f * s, JT);
        ring(cx + 0.16f * s, cy + 0.11f * s, 0.12f * s, col, JT);
        line(cx - 0.26f * s, cy + 0.02f * s, cx - 0.04f * s, cy + 0.02f * s, col, JT);
        line(cx - 0.26f * s, cy + 0.2f * s, cx - 0.04f * s, cy + 0.2f * s, col, JT);
        line(cx - 0.24f * s, cy - 0.16f * s, cx + 0.18f * s, cy - 0.42f * s, col, JT);
        break;
    case JI_PLUS:
        line(cx - 0.36f * s, cy, cx + 0.36f * s, cy, col, JT);
        line(cx, cy - 0.36f * s, cx, cy + 0.36f * s, col, JT);
        break;
    case JI_GEAR:
        ring(cx, cy, 0.13f * s, col, JT);
        ring(cx, cy, 0.3f * s, col, JT);
        for (int i = 0; i < 8; i++) {
            float an = i * 3.14159265f / 4;
            line(cx + cosf(an) * 0.3f * s, cy + sinf(an) * 0.3f * s, cx + cosf(an) * 0.43f * s, cy + sinf(an) * 0.43f * s,
                 col, JT * 1.6f);
        }
        break;
    case JI_REFRESH: /* the design's "Recent": a circle that comes back on itself */
        jux_arc(cx, cy, 0.36f * s, -2.4f, 2.6f, col);
        tri(cx - 0.4f * s, cy - 0.34f * s, cx - 0.12f * s, cy - 0.3f * s, cx - 0.32f * s, cy - 0.08f * s, col);
        break;
    case JI_PLAY:
        jux_path({ { -0.22f, -0.36f }, { 0.36f, 0 }, { -0.22f, 0.36f } }, cx, cy, s, col, true);
        break;
    case JI_STOP:
        stroke(cx - 0.3f * s, cy - 0.3f * s, 0.6f * s, 0.6f * s, col, 0.06f * s, JT);
        break;
    case JI_PLAYLIST_ADD:
        line(cx - 0.42f * s, cy - 0.28f * s, cx + 0.3f * s, cy - 0.28f * s, col, JT);
        line(cx - 0.42f * s, cy, cx + 0.06f * s, cy, col, JT);
        line(cx - 0.42f * s, cy + 0.28f * s, cx - 0.06f * s, cy + 0.28f * s, col, JT);
        line(cx + 0.14f * s, cy + 0.2f * s, cx + 0.42f * s, cy + 0.2f * s, col, JT);
        line(cx + 0.28f * s, cy + 0.06f * s, cx + 0.28f * s, cy + 0.34f * s, col, JT);
        break;
    case JI_PENCIL:
        jux_path({ { -0.34f, 0.34f }, { -0.3f, 0.14f }, { 0.18f, -0.34f }, { 0.34f, -0.18f }, { -0.14f, 0.3f } }, cx, cy, s, col,
                 true);
        break;
    case JI_TRASH:
        line(cx - 0.38f * s, cy - 0.26f * s, cx + 0.38f * s, cy - 0.26f * s, col, JT);
        jux_path({ { -0.12f, -0.26f }, { -0.12f, -0.38f }, { 0.12f, -0.38f }, { 0.12f, -0.26f } }, cx, cy, s, col, false);
        jux_path({ { -0.28f, -0.26f }, { -0.22f, 0.4f }, { 0.22f, 0.4f }, { 0.28f, -0.26f } }, cx, cy, s, col, false);
        break;
    case JI_PHONE:
        stroke(cx - 0.22f * s, cy - 0.42f * s, 0.44f * s, 0.84f * s, col, 0.08f * s, JT);
        line(cx - 0.06f * s, cy + 0.3f * s, cx + 0.06f * s, cy + 0.3f * s, col, JT);
        break;
    case JI_SEARCH:
        ring(cx - 0.08f * s, cy - 0.08f * s, 0.26f * s, col, JT);
        line(cx + 0.12f * s, cy + 0.12f * s, cx + 0.38f * s, cy + 0.38f * s, col, JT);
        break;
    case JI_LINK:
        jux_arc(cx - 0.14f * s, cy + 0.14f * s, 0.16f * s, 0.8f, 3.9f, col);
        jux_arc(cx + 0.14f * s, cy - 0.14f * s, 0.16f * s, -2.35f, 0.75f, col);
        line(cx - 0.1f * s, cy + 0.1f * s, cx + 0.1f * s, cy - 0.1f * s, col, JT);
        break;
    }
}
