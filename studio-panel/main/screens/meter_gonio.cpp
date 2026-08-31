// meter_gonio.cpp — Lissajous Goniometer (Phosphor-style)
// Plots gonio_l (X) vs gonio_r (Y) — ring buffer of 512 samples at 30 Hz (~17s).
// Older samples fade to black; newest samples bright accent.
// Reference lines: center cross (dim), +45° (in-phase/mono), -45° (out-of-phase).
// Canvas 800×480 PSRAM ARGB8888. Updates at 20 Hz.

#include <cmath>
#include <cstring>
#include <algorithm>
#include "meter_gonio.h"
#include "metering_hub.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "screens/settings_overlay.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG __attribute__((unused)) = "meter_gonio";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int CW = 800;
static constexpr int CH = 480;

// Square plot area, centered on screen
static constexpr int PLOT_SZ = 340;    // side length px
static constexpr int PLOT_CX = CW / 2; // 400
static constexpr int PLOT_CY = CH / 2; // 240
static constexpr int PLOT_R  = PLOT_SZ / 2;  // 170 — sample maps ±1 → ±PLOT_R

// Ring buffer capacity — 512 samples at 30 Hz ≈ 17 s persistence
static constexpr int BUF_CAP = 512;

// Dot radius (soft blob per sample)
static constexpr int DOT_R = 2;

// ── State ─────────────────────────────────────────────────────────────────────

struct GonioState {
    lv_timer_t   *timer   = nullptr;
    lv_obj_t     *canvas  = nullptr;
    lv_color32_t *buf     = nullptr;

    // Ring buffer of L/R sample pairs
    float         rb_l[BUF_CAP] = {};
    float         rb_r[BUF_CAP] = {};
    int           rb_head = 0;
    int           rb_count = 0;

    uint32_t      last_seq = 0xFFFFFFFF;
    int decay_sel    = 1;  // 0=Fast, 1=Medium, 2=Slow
    int display_mode = 0;  // 0=XY, 1=M/S
    int prev_mode    = -1; // for detecting mode change to update axis labels

    lv_obj_t *lbl_h_axis = nullptr;  // horizontal axis label (L or S)
    lv_obj_t *lbl_v_axis = nullptr;  // vertical axis label (R or M)
};

// ── Pixel helper ──────────────────────────────────────────────────────────────

static inline void put_px_max(lv_color32_t *buf, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    if (r > p->red)   p->red   = r;
    if (g > p->green) p->green = g;
    if (b > p->blue)  p->blue  = b;
}

static inline void put_px(lv_color32_t *buf, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
}

// ── Reference lines ───────────────────────────────────────────────────────────

static void draw_references(lv_color32_t *buf, int mode)
{
    // Bounding square
    for (int d = -PLOT_R; d <= PLOT_R; d++) {
        put_px(buf, PLOT_CX + d, PLOT_CY - PLOT_R, 0x28, 0x28, 0x28);
        put_px(buf, PLOT_CX + d, PLOT_CY + PLOT_R, 0x28, 0x28, 0x28);
        put_px(buf, PLOT_CX - PLOT_R, PLOT_CY + d, 0x28, 0x28, 0x28);
        put_px(buf, PLOT_CX + PLOT_R, PLOT_CY + d, 0x28, 0x28, 0x28);
    }

    // Center cross (dim)
    for (int d = -PLOT_R; d <= PLOT_R; d++) {
        put_px(buf, PLOT_CX + d, PLOT_CY, 0x22, 0x22, 0x22);
        put_px(buf, PLOT_CX, PLOT_CY + d, 0x22, 0x22, 0x22);
    }

    if (mode == 0) {
        // XY: +45 deg guide = in-phase (L=R) green; -45 deg = cancelled (L=-R) red
        for (int d = -PLOT_R; d <= PLOT_R; d++)
            put_px(buf, PLOT_CX + d, PLOT_CY - d, 0x00, 0x28, 0x0A);
        for (int d = -PLOT_R; d <= PLOT_R; d++)
            put_px(buf, PLOT_CX + d, PLOT_CY + d, 0x28, 0x00, 0x00);
    } else {
        // M/S: vertical center = mono direction (green); horizontal = cancelled (red)
        for (int d = -PLOT_R; d <= PLOT_R; d++)
            put_px(buf, PLOT_CX, PLOT_CY + d, 0x00, 0x28, 0x0A);
        for (int d = -PLOT_R; d <= PLOT_R; d++)
            put_px(buf, PLOT_CX + d, PLOT_CY, 0x28, 0x00, 0x00);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────

static constexpr int DECAY_CAPS[] = { 96, 256, 512 };

static void render_gonio(GonioState *st)
{
    lv_color32_t *buf  = st->buf;
    int           mode = st->display_mode;

    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    draw_references(buf, mode);

    int count = st->rb_count;
    if (count == 0) { lv_obj_invalidate(st->canvas); return; }

    for (int age = 0; age < count; age++) {
        float t      = (float)age / (float)(count > 1 ? count - 1 : 1);
        float bright = t * t * t;
        uint8_t r    = 0;
        uint8_t g    = (uint8_t)(0xCC * bright);
        uint8_t b    = (uint8_t)(0xAA * bright);

        int   idx = (st->rb_head - count + age + BUF_CAP) % BUF_CAP;
        float lv  = st->rb_l[idx];
        float rv  = st->rb_r[idx];

        float mx, my;
        if (mode == 0) {
            mx = lv;          // XY: L→X, R→Y
            my = rv;
        } else {
            mx = (lv - rv) * 0.5f;   // M/S: Side→X
            my = (lv + rv) * 0.5f;   // M/S: Mid→Y
        }

        int px = PLOT_CX + (int)(mx * PLOT_R);
        int py = PLOT_CY - (int)(my * PLOT_R);

        put_px_max(buf, px,     py,     r, g, b);
        put_px_max(buf, px - 1, py,     (uint8_t)(r>>1), (uint8_t)(g>>1), (uint8_t)(b>>1));
        put_px_max(buf, px + 1, py,     (uint8_t)(r>>1), (uint8_t)(g>>1), (uint8_t)(b>>1));
        put_px_max(buf, px,     py - 1, (uint8_t)(r>>1), (uint8_t)(g>>1), (uint8_t)(b>>1));
        put_px_max(buf, px,     py + 1, (uint8_t)(r>>1), (uint8_t)(g>>1), (uint8_t)(b>>1));
    }

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void gonio_timer_cb(lv_timer_t *t)
{
    GonioState *st = static_cast<GonioState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.seq != st->last_seq) {
        st->last_seq = pkt.seq;

        st->rb_l[st->rb_head] = pkt.gonio_l;
        st->rb_r[st->rb_head] = pkt.gonio_r;
        st->rb_head = (st->rb_head + 1) % BUF_CAP;
        if (st->rb_count < BUF_CAP) st->rb_count++;
        if (st->rb_count > DECAY_CAPS[st->decay_sel])
            st->rb_count = DECAY_CAPS[st->decay_sel];
    }

    // Update axis labels when display mode changes
    if (st->display_mode != st->prev_mode) {
        st->prev_mode = st->display_mode;
        if (st->lbl_h_axis) lv_label_set_text(st->lbl_h_axis, st->display_mode == 0 ? "L" : "S");
        if (st->lbl_v_axis) lv_label_set_text(st->lbl_v_axis, st->display_mode == 0 ? "R" : "M");
    }

    render_gonio(st);
}

// ── Info overlay ──────────────────────────────────────────────────────────────

static void gonio_info_open(lv_obj_t *scr)
{
    constexpr int IW       = 560;
    constexpr int IH       = 290;
    constexpr int IPAD     = 16;
    constexpr int SECT_IND = 11;   // 3px accent bar + 8px gap
    constexpr int BODY_W   = IW - 2 * IPAD - SECT_IND;

    int ix = (800 - IW) / 2;
    int iy = 32 + (392 - IH) / 2;

    // Dimmer — tap anywhere outside card to close
    lv_obj_t *dimmer = lv_obj_create(scr);
    lv_obj_remove_style_all(dimmer);
    lv_obj_set_size(dimmer, 800, 480);
    lv_obj_set_pos(dimmer, 0, 0);
    lv_obj_set_style_bg_color(dimmer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimmer, LV_OPA_70, 0);
    lv_obj_clear_flag(dimmer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dimmer, [](lv_event_t *e) {
        lv_obj_t *dim = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
        lv_async_call([](void *a){ lv_obj_delete(static_cast<lv_obj_t *>(a)); }, dim);
    }, LV_EVENT_CLICKED, dimmer);

    // Card
    lv_obj_t *card = lv_obj_create(dimmer);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, IW, IH);
    lv_obj_set_pos(card, ix, iy);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS * 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(card, 28, 0);
    lv_obj_set_style_shadow_spread(card, 4, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_70, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Accent top stripe
    lv_obj_t *stripe = lv_obj_create(card);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_size(stripe, IW, 4);
    lv_obj_set_pos(stripe, 0, 0);
    lv_obj_set_style_bg_color(stripe, THEME_ACCENT, 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_obj_remove_style_all(title);
    lv_label_set_text(title, "GONIOMETER");
    lv_obj_set_style_text_color(title, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, 0);
    lv_obj_set_pos(title, IPAD, 10);

    // Close button
    lv_obj_t *xbtn = lv_btn_create(card);
    lv_obj_remove_style_all(xbtn);
    lv_obj_set_size(xbtn, 32, 32);
    lv_obj_set_pos(xbtn, IW - IPAD - 32, 8);
    lv_obj_set_style_bg_color(xbtn, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_color(xbtn, lv_color_hex(0x404040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(xbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(xbtn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(xbtn, [](lv_event_t *e) {
        lv_obj_t *dim = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
        lv_async_call([](void *a){ lv_obj_delete(static_cast<lv_obj_t *>(a)); }, dim);
    }, LV_EVENT_CLICKED, dimmer);
    lv_obj_t *xlbl = lv_label_create(xbtn);
    lv_obj_remove_style_all(xlbl);
    lv_label_set_text(xlbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xlbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(xlbl, THEME_FONT_LABEL, 0);
    lv_obj_center(xlbl);

    // Header separator
    lv_obj_t *sep0 = lv_obj_create(card);
    lv_obj_remove_style_all(sep0);
    lv_obj_set_size(sep0, IW - 2 * IPAD, 1);
    lv_obj_set_pos(sep0, IPAD, 46);
    lv_obj_set_style_bg_color(sep0, THEME_SEPARATOR, 0);
    lv_obj_set_style_bg_opa(sep0, LV_OPA_COVER, 0);

    // Helper — places one section (accent bar + heading + body)
    int cur_y = 54;
    auto section = [&](const char *heading, const char *body, int body_h) {
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 3, 18);
        lv_obj_set_pos(bar, IPAD, cur_y + 1);
        lv_obj_set_style_bg_color(bar, THEME_ACCENT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 1, 0);

        lv_obj_t *hdr = lv_label_create(card);
        lv_obj_remove_style_all(hdr);
        lv_label_set_text(hdr, heading);
        lv_obj_set_style_text_color(hdr, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(hdr, THEME_FONT_LABEL, 0);
        lv_obj_set_pos(hdr, IPAD + SECT_IND, cur_y);
        cur_y += 24;

        lv_obj_t *bdy = lv_label_create(card);
        lv_obj_remove_style_all(bdy);
        lv_label_set_long_mode(bdy, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(bdy, BODY_W, body_h);
        lv_obj_set_pos(bdy, IPAD + SECT_IND, cur_y);
        lv_label_set_text(bdy, body);
        lv_obj_set_style_text_color(bdy, THEME_TEXT_SECONDARY, 0);
        lv_obj_set_style_text_font(bdy, THEME_FONT_HINT, 0);
        cur_y += body_h;
    };

    section(
        "IN PRACTICE",
        "Mono source (voice, kick): narrow diagonal (XY) or vertical line (M/S).\n"
        "Wide stereo: spread ellipse or broad horizontal trace.\n"
        "Phase problem: trace drifts toward the red guide line.\n"
        "Signals on the red axis cancel silently in any mono system.",
        86
    );

    // Mid separator
    lv_obj_t *sep1 = lv_obj_create(card);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, IW - 2 * IPAD, 1);
    lv_obj_set_pos(sep1, IPAD, cur_y + 4);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0x282828), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    cur_y += 12;

    section(
        "HOW IT WORKS",
        "Each sample pair (L, R) plots one point — older points fade, giving the "
        "trace a sense of time.\n"
        "XY: L horizontal, R vertical. M/S rotates 45 deg so mono points "
        "upward and phase cancellation points sideways — the professional standard.",
        76
    );
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_gonio_delete(lv_event_t *e)
{
    GonioState *st = static_cast<GonioState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    if (st->buf)   heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_gonio_screen_create()
{
    GonioState *st = new GonioState{};

    st->buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(CW * CH * sizeof(lv_color32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st->buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        delete st;
        return theme_make_screen();
    }

    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_gonio_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("GONIOMETER");

    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // Axis labels — update dynamically when mode changes
    auto make_axis_lbl = [&](const char *text, lv_align_t align, int ox, int oy) -> lv_obj_t * {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_HINT, 0);
        lv_obj_align(l, align, ox, oy);
        return l;
    };
    // Horizontal axis label (left = negative direction): "L" in XY, "S" in M/S
    st->lbl_h_axis = make_axis_lbl("L", LV_ALIGN_CENTER, -(PLOT_R + 18), 0);
    // Vertical axis label (top = positive direction): "R" in XY, "M" in M/S
    st->lbl_v_axis = make_axis_lbl("R", LV_ALIGN_CENTER, 0, -(PLOT_R + 14));

    lv_obj_t *gonio_rz = foot_create_hub_back(scr);

    static const SettingOption decay_opts[] = { {"Fast"}, {"Medium"}, {"Slow"} };
    static const SettingOption display_opts[] = {
        { "XY",
          "XY — OSCILLOSCOPE\n"
          "L: horizontal axis   R: vertical axis\n"
          "Mono (L=R): 45 deg diagonal   Cancelled (L=-R): -45 deg" },
        { "M/S",
          "M/S — INDUSTRY STANDARD\n"
          "Mid (L+R): vertical axis   Side (L-R): horizontal axis\n"
          "Mono: vertical center line   Cancelled: horizontal line" },
    };
    auto *gonio_items = new SettingItem[2];
    gonio_items[0] = { "Decay",   decay_opts,   3, &st->decay_sel    };
    gonio_items[1] = { "Display", display_opts, 2, &st->display_mode };
    settings_btn_create(gonio_rz, scr, gonio_items, 2);

    // Info button — to the left of the gear icon
    lv_obj_t *info_btn = lv_btn_create(gonio_rz);
    lv_obj_remove_style_all(info_btn);
    lv_obj_set_size(info_btn, 44, 44);
    lv_obj_align(info_btn, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_set_style_bg_color(info_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(info_btn, THEME_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(info_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_btn, LV_RADIUS_CIRCLE, 0);
    theme_apply_glow(info_btn);
    lv_obj_t *info_lbl = lv_label_create(info_btn);
    lv_obj_remove_style_all(info_lbl);
    lv_label_set_text(info_lbl, "i");
    lv_obj_set_style_text_color(info_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(info_lbl, THEME_FONT_LABEL, 0);
    lv_obj_center(info_lbl);
    lv_obj_add_event_cb(info_btn, [](lv_event_t *e) {
        gonio_info_open(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scr);

    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    st->timer = lv_timer_create(gonio_timer_cb, 50, st);

    return scr;
}
