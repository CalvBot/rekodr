// What the Timeline and the editor share on their tracks: the view bar over the whole, the in and out on the ruler,
// the playhead, the time pill, and Shift's scaling of the in and out around the playhead. Both draw and handle them
// alike; times are each caller's own units, passed as f64.

// The sizes both use
static f32 wa_ruler_h() { return kit_px(28); }
static f32 wa_viewbar_h() { return kit_px(10); }
static f32 wa_grab() { return kit_px(7); } // how near an end of the in and out the mouse takes hold of it

// A text in a pill centred on x, kept between lo and hi
static void wa_pill(ImDrawList *dl, f32 x, f32 y, f32 lo, f32 hi, const char *text, ImU32 bg, ImU32 fg)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    f32 px = kit_px(7), py = kit_px(2);
    f32 w = ts.x + 2 * px, h = ts.y + 2 * py;
    f32 a = x - w / 2;
    if (a + w > hi) a = hi - w;
    if (a < lo) a = lo;
    dl->AddRectFilled(ImVec2(a, y), ImVec2(a + w, y + h), bg, h / 2);
    dl->AddText(ImVec2(floorf(a + px), floorf(y + py)), fg, text);
}

// The view bar: the whole, with the window on the part shown (grips at its ends), the in and out faintly (sa >= sz
// for none) and the playhead.
static void wa_draw_viewbar(ImDrawList *dl, f32 x0, f32 x1, f32 y, f32 wa, f32 wb, bool hot, f32 sa, f32 sz, f32 px)
{
    f32 s = kit_px(1), h = wa_viewbar_h(), r = h / 2, y1 = y + h;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1), col32(COL_PANEL2), r);
    if (sz > sa) {
        sa = ImClamp(sa, x0, x1);
        sz = ImClamp(sz, x0, x1);
        dl->AddRectFilled(ImVec2(sa, y), ImVec2(ImMax(sz, sa + 1 * s), y1), col32(COL_ACCENT, 0.35f));
    }
    if (wb - wa < 14 * s) { // wide enough for its two grips
        f32 m = (wa + wb) / 2;
        wa = m - 7 * s;
        wb = m + 7 * s;
    }
    dl->AddRectFilled(ImVec2(wa, y), ImVec2(wb, y1), hot ? col32(COL_TEXT, 0.55f) : col32(COL_DIM), r);
    f32 grips[2] = {wa + 4 * s, wb - 4 * s};
    for (f32 gx : grips)
        dl->AddLine(ImVec2(floorf(gx) + 0.5f, y + 2.5f * s), ImVec2(floorf(gx) + 0.5f, y1 - 2.5f * s), IM_COL32(255, 255, 255, 190), 1.5f * s);
    px = ImClamp(px, x0, x1);
    dl->AddRectFilled(ImVec2(px - 1 * s, y), ImVec2(px + 1 * s, y1), IM_COL32(255, 255, 255, 230));
}

// Where the band of the in and out sits, on a ruler from y_ruler
static f32 wa_band_y(f32 y_ruler) { return y_ruler + wa_ruler_h() - kit_px(6); }

// The in and out: a tint over the tracks between them, a band along the ruler's foot, and a handle on each end, lit
// while held or under the mouse. The tint and the ends' lines run from y_tracks to y_bottom.
static void wa_draw_inout(ImDrawList *dl, f32 xa, f32 xb, f32 y_ruler, f32 y_tracks, f32 y_bottom, bool hot_in, bool hot_out)
{
    f32 s = kit_px(1), y_band = wa_band_y(y_ruler);
    dl->AddRectFilled(ImVec2(xa, y_tracks), ImVec2(xb, y_bottom), col32(COL_ACCENT, 0.12f));
    dl->AddRectFilled(ImVec2(xa, y_band), ImVec2(xb, y_band + 4 * s), col32(COL_ACCENT, 0.85f));
    for (i32 k = 0; k < 2; k++) {
        f32 x = k ? xb : xa;
        ImU32 col = col32((k ? hot_out : hot_in) ? COL_ACCENT_HOVER : COL_ACCENT);
        dl->AddLine(ImVec2(x, y_band), ImVec2(x, y_bottom), col, 2 * s);
        f32 hw = 5 * s, hy = y_band + 2 * s;
        dl->AddRectFilled(ImVec2(x - hw, hy - 11 * s), ImVec2(x + hw, hy + 11 * s), col, 4 * s);
        dl->AddLine(ImVec2(x - 1.5f * s, hy - 5 * s), ImVec2(x - 1.5f * s, hy + 5 * s), col32(0xffffff, 0.8f), 1);
        dl->AddLine(ImVec2(x + 1.5f * s, hy - 5 * s), ImVec2(x + 1.5f * s, hy + 5 * s), col32(0xffffff, 0.8f), 1);
    }
}

// The playhead: a white line down from y_tracks, with a triangle over it
static void wa_draw_playhead(ImDrawList *dl, f32 x, f32 y_tracks, f32 y_bottom)
{
    f32 s = kit_px(1), hy = y_tracks - 4 * s;
    ImU32 col = col32(COL_TEXT);
    dl->AddLine(ImVec2(x, hy), ImVec2(x, y_bottom), col, 1.5f * s);
    dl->AddTriangleFilled(ImVec2(x - 5 * s, hy - 7 * s), ImVec2(x + 5 * s, hy - 7 * s), ImVec2(x, hy), col);
}

// Shift on an end of the in and out: both ends scale around the playhead p, the other end moving in proportion, so
// the playhead keeps its place relative to them (inside or not). in0 and out0 are the ends as the drag began, t
// where the dragged end goes; neither end leaves [lo, hi] and they stay least apart. False when the dragged end is
// on the playhead, where there is nothing to scale by.
static bool wa_scale(f64 p, f64 in0, f64 out0, bool in, f64 t, f64 lo, f64 hi, f64 least, f64 *in_to, f64 *out_to)
{
    f64 a = in0 - p, z = out0 - p; // the ends from the playhead, signed
    if ((in ? a : z) == 0 || z <= a) return false;
    f64 f = (t - p) / (in ? a : z);
    f64 fewest = least / (z - a), most = 1e300;
    f64 ends[2] = {a, z};
    for (f64 e : ends) {
        if (e < 0) most = ImMin(most, (p - lo) / -e);
        if (e > 0) most = ImMin(most, (hi - p) / e);
    }
    f = f > most ? most : f < fewest ? fewest : f;
    *in_to = p + a * f;
    *out_to = p + z * f;
    return true;
}
