// meter_stereo.cpp — Combined Stereo Analysis
// Left:  Lissajous goniometer (phosphor, same scheme as meter_gonio)
// Right upper: Mid / Side level bars (green→yellow→red, same as meter_ms)
// Right lower: Phase correlation bar + numeric (same as meter_phase)
// Settings: Goniometer Decay (Fast/Medium/Slow)

#include <cmath>
#include <cstring>
#include <algorithm>
#include "meter_stereo.h"
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

static const char *TAG __attribute__((unused)) = "meter_stereo";

static constexpr int CW = 800;
static constexpr int CH = 480;

// ── Goniometer (left half) ────────────────────────────────────────────────────
static constexpr int GONIO_CX  = 192;
static constexpr int GONIO_CY  = 228;
static constexpr int GONIO_R   = 160;
static constexpr int GONIO_CAP = 512;

// ── Vertical divider ──────────────────────────────────────────────────────────
static constexpr int DIV_X = 385;

// ── M/S bars (right upper) ────────────────────────────────────────────────────
static constexpr int MS_BAR_W   = 40;
static constexpr int MS_BAR_H   = 164;
static constexpr int MS_BAR_BOT = 258;   // bottom y; top = BOT - H = 94
static constexpr int MS_MID_X   = 500;
static constexpr int MS_SIDE_X  = 600;
static constexpr float MS_DB_MIN   = -60.0f;
static constexpr float MS_DB_RANGE = 60.0f;

// ── Phase correlation bar (right lower) ──────────────────────────────────────
static constexpr int PH_X = 405;
static constexpr int PH_W = 370;
static constexpr int PH_Y = 342;
static constexpr int PH_H = 22;

// ── State ─────────────────────────────────────────────────────────────────────
struct StereoState {
    lv_timer_t   *timer   = nullptr;
    lv_obj_t     *canvas  = nullptr;
    lv_color32_t *buf     = nullptr;

    float  rb_l[GONIO_CAP] = {};
    float  rb_r[GONIO_CAP] = {};
    int    rb_head  = 0;
    int    rb_count = 0;

    float  mid_db   = MS_DB_MIN;
    float  side_db  = MS_DB_MIN;
    float  mid_peak = MS_DB_MIN;
    float  side_peak= MS_DB_MIN;
    float  peak_t_m = 0.0f;
    float  peak_t_s = 0.0f;

    float  corr_smooth = 0.0f;

    int    decay_sel    = 1;  // 0=Fast, 1=Medium, 2=Slow
    int    display_mode = 0;  // 0=XY, 1=M/S

    lv_obj_t *lbl_mid  = nullptr;
    lv_obj_t *lbl_side = nullptr;
    lv_obj_t *lbl_corr = nullptr;

    uint32_t last_seq = 0xFFFFFFFF;
};

// ── Pixel helpers ─────────────────────────────────────────────────────────────
static inline void put_px(lv_color32_t *buf, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
}

static void fill_rect(lv_color32_t *buf, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int ry = y; ry < y + h; ry++)
        for (int cx = x; cx < x + w; cx++)
            put_px(buf, cx, ry, r, g, b);
}

static inline void put_max(lv_color32_t *buf, int x, int y,
                            uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    if (r > p->red)   p->red   = r;
    if (g > p->green) p->green = g;
    if (b > p->blue)  p->blue  = b;
}

// ── Goniometer drawing ────────────────────────────────────────────────────────
static void draw_gonio_section(lv_color32_t *buf, StereoState *st)
{
    int mode = st->display_mode;

    // Bounding square
    for (int d = -GONIO_R; d <= GONIO_R; d++) {
        put_px(buf, GONIO_CX + d, GONIO_CY - GONIO_R, 0x28, 0x28, 0x28);
        put_px(buf, GONIO_CX + d, GONIO_CY + GONIO_R, 0x28, 0x28, 0x28);
        put_px(buf, GONIO_CX - GONIO_R, GONIO_CY + d, 0x28, 0x28, 0x28);
        put_px(buf, GONIO_CX + GONIO_R, GONIO_CY + d, 0x28, 0x28, 0x28);
    }
    // Center cross
    for (int d = -GONIO_R; d <= GONIO_R; d++) {
        put_px(buf, GONIO_CX + d, GONIO_CY,     0x22, 0x22, 0x22);
        put_px(buf, GONIO_CX,     GONIO_CY + d, 0x22, 0x22, 0x22);
    }

    if (mode == 0) {
        // XY: +45 deg guide = in-phase (green); -45 deg = cancelled (red)
        for (int d = -GONIO_R; d <= GONIO_R; d++)
            put_px(buf, GONIO_CX + d, GONIO_CY - d, 0x00, 0x28, 0x0A);
        for (int d = -GONIO_R; d <= GONIO_R; d++)
            put_px(buf, GONIO_CX + d, GONIO_CY + d, 0x28, 0x00, 0x00);
    } else {
        // M/S: vertical = mono direction (green); horizontal = cancelled (red)
        for (int d = -GONIO_R; d <= GONIO_R; d++)
            put_px(buf, GONIO_CX,     GONIO_CY + d, 0x00, 0x28, 0x0A);
        for (int d = -GONIO_R; d <= GONIO_R; d++)
            put_px(buf, GONIO_CX + d, GONIO_CY,     0x28, 0x00, 0x00);
    }

    // Phosphor dots (oldest → newest, cubic brightness ramp)
    int count = st->rb_count;
    for (int age = 0; age < count; age++) {
        float t      = (float)age / (float)(count > 1 ? count - 1 : 1);
        float bright = t * t * t;
        uint8_t g    = (uint8_t)(0xCC * bright);
        uint8_t b    = (uint8_t)(0xAA * bright);
        int idx = (st->rb_head - count + age + GONIO_CAP) % GONIO_CAP;
        float lv = st->rb_l[idx];
        float rv = st->rb_r[idx];

        float mx, my;
        if (mode == 0) {
            mx = lv; my = rv;
        } else {
            mx = (lv - rv) * 0.5f;
            my = (lv + rv) * 0.5f;
        }

        int px = GONIO_CX + (int)(mx * GONIO_R);
        int py = GONIO_CY - (int)(my * GONIO_R);
        put_max(buf, px,     py,     0, g, b);
        put_max(buf, px - 1, py,     0, g >> 1, b >> 1);
        put_max(buf, px + 1, py,     0, g >> 1, b >> 1);
        put_max(buf, px,     py - 1, 0, g >> 1, b >> 1);
        put_max(buf, px,     py + 1, 0, g >> 1, b >> 1);
    }
}

// ── M/S bar color (green → yellow → red) ─────────────────────────────────────
static void ms_color(float frac, uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (frac < 0.75f) {
        float t = frac / 0.75f;
        r = (uint8_t)(0xFF * t);
        g = (uint8_t)(0xDD - 0x11 * t);
        b = 0;
    } else {
        float t = (frac - 0.75f) / 0.25f;
        r = 0xFF;
        g = (uint8_t)(0xCC * (1.0f - t));
        b = 0;
    }
}

static void draw_ms_bar(lv_color32_t *buf, int bar_x, float db, float peak_db)
{
    int top = MS_BAR_BOT - MS_BAR_H;
    fill_rect(buf, bar_x, top, MS_BAR_W, MS_BAR_H, 0x14, 0x14, 0x14);

    float frac = (db - MS_DB_MIN) / MS_DB_RANGE;
    frac = std::max(0.0f, std::min(1.0f, frac));
    int fill_h = (int)(frac * (float)(MS_BAR_H - 2));

    for (int row = 0; row < fill_h; row++) {
        float row_frac = (float)row / (float)(MS_BAR_H - 2);
        uint8_t cr, cg, cb;
        ms_color(row_frac, cr, cg, cb);
        int cy = MS_BAR_BOT - 1 - row;
        for (int col = bar_x + 2; col < bar_x + MS_BAR_W - 2; col++)
            put_px(buf, col, cy, cr, cg, cb);
    }

    // Tick marks at -12, -24, -36, -48 dBFS (side edges)
    for (float tick : {-12.0f, -24.0f, -36.0f, -48.0f}) {
        float tf = (tick - MS_DB_MIN) / MS_DB_RANGE;
        int ty = MS_BAR_BOT - 1 - (int)(tf * (float)(MS_BAR_H - 2));
        put_px(buf, bar_x,              ty, 0x40, 0x40, 0x40);
        put_px(buf, bar_x + MS_BAR_W - 1, ty, 0x40, 0x40, 0x40);
    }

    // Peak marker (2px)
    float pk = (peak_db - MS_DB_MIN) / MS_DB_RANGE;
    pk = std::max(0.0f, std::min(1.0f, pk));
    if (pk > 0.02f) {
        int py = MS_BAR_BOT - 1 - (int)(pk * (float)(MS_BAR_H - 2));
        for (int col = bar_x + 1; col < bar_x + MS_BAR_W - 1; col++) {
            put_px(buf, col, py,     0xE0, 0xE0, 0xE0);
            put_px(buf, col, py - 1, 0x80, 0x80, 0x80);
        }
    }
}

// ── Phase correlation bar ─────────────────────────────────────────────────────
static void ph_color(float c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (c >= 0.0f) {
        float t = c;
        r = (uint8_t)(0xFF * (1.0f - t));
        g = (uint8_t)(0xBB + 0x44 * t);
        b = 0;
    } else {
        float t = -c;
        r = 0xFF;
        g = (uint8_t)(0xBB * (1.0f - t));
        b = 0;
    }
}

static void draw_phase_bar(lv_color32_t *buf, float corr)
{
    fill_rect(buf, PH_X, PH_Y, PH_W, PH_H, 0x18, 0x18, 0x18);

    int cx = PH_X + PH_W / 2;
    int fill_px = (int)(corr * (float)(PH_W / 2 - 2));
    int bx = (corr >= 0.0f) ? cx : cx + fill_px;
    int bw = std::abs(fill_px);

    for (int col = bx; col < bx + bw && col < PH_X + PH_W - 1; col++) {
        float v = (float)(col - cx) / (float)(PH_W / 2);
        v = std::max(-1.0f, std::min(1.0f, v));
        uint8_t cr, cg, cb;
        ph_color(v, cr, cg, cb);
        for (int row = PH_Y + 2; row < PH_Y + PH_H - 2; row++)
            put_px(buf, col, row, cr, cg, cb);
    }

    // Border
    for (int col = PH_X; col < PH_X + PH_W; col++) {
        put_px(buf, col, PH_Y,           0x40, 0x40, 0x40);
        put_px(buf, col, PH_Y + PH_H - 1, 0x40, 0x40, 0x40);
    }
    for (int row = PH_Y; row < PH_Y + PH_H; row++) {
        put_px(buf, PH_X,            row, 0x40, 0x40, 0x40);
        put_px(buf, PH_X + PH_W - 1, row, 0x40, 0x40, 0x40);
    }

    // Tick marks
    for (float tv : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        int tx = cx + (int)(tv * (float)(PH_W / 2));
        for (int row = PH_Y - 4; row < PH_Y; row++)
            put_px(buf, tx, row, 0x50, 0x50, 0x50);
        for (int row = PH_Y + PH_H; row < PH_Y + PH_H + 4; row++)
            put_px(buf, tx, row, 0x50, 0x50, 0x50);
    }
}

// ── Full render ────────────────────────────────────────────────────────────────
static void render_stereo(StereoState *st)
{
    lv_color32_t *buf = st->buf;
    lv_color32_t bg = {.blue = 0x0A, .green = 0x0A, .red = 0x0A, .alpha = 0xFF};
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    // Vertical divider
    for (int y = THEME_CONTENT_Y; y < THEME_CONTENT_Y + THEME_CONTENT_H; y++)
        put_px(buf, DIV_X, y, 0x24, 0x24, 0x24);

    draw_gonio_section(buf, st);
    draw_ms_bar(buf, MS_MID_X,  st->mid_db,  st->mid_peak);
    draw_ms_bar(buf, MS_SIDE_X, st->side_db, st->side_peak);
    draw_phase_bar(buf, st->corr_smooth);

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ─────────────────────────────────────────────────────────────
static constexpr float DT          = 0.05f;
static constexpr float PEAK_HOLD_S = 2.0f;
static constexpr float PEAK_FALL   = 18.0f;  // dB/s

static void stereo_timer_cb(lv_timer_t *t)
{
    StereoState *st = static_cast<StereoState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.seq != st->last_seq) {
        st->last_seq = pkt.seq;

        // Goniometer
        st->rb_l[st->rb_head] = pkt.gonio_l;
        st->rb_r[st->rb_head] = pkt.gonio_r;
        st->rb_head = (st->rb_head + 1) % GONIO_CAP;
        if (st->rb_count < GONIO_CAP) st->rb_count++;
        static constexpr int DECAY_CAPS[] = {96, 256, 512};
        if (st->rb_count > DECAY_CAPS[st->decay_sel])
            st->rb_count = DECAY_CAPS[st->decay_sel];

        // True M/S RMS from sample domain — phase-correct, sent by sender.
        float mid_db  = pkt.rms_mono;
        float side_db = pkt.rms_side;
        st->mid_db  += (mid_db  - st->mid_db)  * 0.25f;
        st->side_db += (side_db - st->side_db) * 0.25f;

        if (st->mid_db > st->mid_peak)       { st->mid_peak  = st->mid_db;  st->peak_t_m = PEAK_HOLD_S; }
        else if (st->peak_t_m > 0.0f)         { st->peak_t_m -= DT; }
        else st->mid_peak  = std::max(st->mid_peak  - PEAK_FALL * DT, MS_DB_MIN);

        if (st->side_db > st->side_peak)      { st->side_peak = st->side_db; st->peak_t_s = PEAK_HOLD_S; }
        else if (st->peak_t_s > 0.0f)         { st->peak_t_s -= DT; }
        else st->side_peak = std::max(st->side_peak - PEAK_FALL * DT, MS_DB_MIN);

        // Phase correlation
        float inst = pkt.gonio_l * pkt.gonio_r;
        inst = std::max(-1.0f, std::min(1.0f, inst));
        st->corr_smooth += (inst - st->corr_smooth) * 0.15f;

        // LVGL labels
        char bm[12], bs[12], bc[12];
        if (st->mid_db  < MS_DB_MIN + 1.0f) snprintf(bm, sizeof(bm), "-inf");
        else snprintf(bm, sizeof(bm), "%.1f", st->mid_db);
        if (st->side_db < MS_DB_MIN + 1.0f) snprintf(bs, sizeof(bs), "-inf");
        else snprintf(bs, sizeof(bs), "%.1f", st->side_db);
        snprintf(bc, sizeof(bc), "%+.2f", st->corr_smooth);

        lv_label_set_text(st->lbl_mid,  bm);
        lv_label_set_text(st->lbl_side, bs);
        lv_label_set_text(st->lbl_corr, bc);
    }

    render_stereo(st);
}

// ── Screen lifecycle ───────────────────────────────────────────────────────────
static void on_stereo_delete(lv_event_t *e)
{
    StereoState *st = static_cast<StereoState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    if (st->buf)   heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_stereo_screen_create()
{
    StereoState *st = new StereoState{};

    st->buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(CW * CH * sizeof(lv_color32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st->buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        delete st;
        return theme_make_screen();
    }

    lv_color32_t bg = {.blue = 0x0A, .green = 0x0A, .red = 0x0A, .alpha = 0xFF};
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_stereo_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("STEREO");

    // Full-screen canvas (behind labels)
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // ── LVGL labels ─────────────────────────────────────────────────────────

    auto make_hint_lbl = [&](const char *text, int x, int y) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_HINT, 0);
        lv_obj_set_pos(l, x, y);
    };

    // Section headers
    make_hint_lbl("GONIOMETER",  24,              THEME_CONTENT_Y + 6);
    make_hint_lbl("M / S",       DIV_X + 20,      THEME_CONTENT_Y + 6);
    make_hint_lbl("CORRELATION", PH_X,            PH_Y - 48);

    // M/S column labels
    constexpr int COL_LBL_Y = MS_BAR_BOT - MS_BAR_H - 30;
    auto make_col_lbl = [&](const char *text, int bar_x) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, THEME_TEXT_SECONDARY, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_HINT, 0);
        lv_obj_set_size(l, MS_BAR_W, 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, bar_x, COL_LBL_Y);
    };
    make_col_lbl("MID",  MS_MID_X);
    make_col_lbl("SIDE", MS_SIDE_X);

    // M/S numeric labels
    constexpr int NUM_LBL_Y = MS_BAR_BOT - MS_BAR_H - 48;
    auto make_num_lbl = [&](lv_obj_t **out, const char *init, int bar_x) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, init);
        lv_obj_set_style_text_color(l, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_HINT, 0);
        lv_obj_set_size(l, MS_BAR_W + 14, 18);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, bar_x - 7, NUM_LBL_Y);
        *out = l;
    };
    make_num_lbl(&st->lbl_mid,  "-inf", MS_MID_X);
    make_num_lbl(&st->lbl_side, "-inf", MS_SIDE_X);

    // "dBFS" below bars
    make_hint_lbl("dBFS", (MS_MID_X + MS_SIDE_X + MS_BAR_W) / 2 - 14, MS_BAR_BOT + 6);

    // Correlation numeric
    lv_obj_t *lbl_corr = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_corr);
    lv_label_set_text(lbl_corr, "+0.00");
    lv_obj_set_style_text_color(lbl_corr, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl_corr, THEME_FONT_LABEL, 0);
    lv_obj_set_size(lbl_corr, 90, 24);
    lv_label_set_long_mode(lbl_corr, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_corr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_corr, PH_X + PH_W / 2 - 45, PH_Y - 26);
    st->lbl_corr = lbl_corr;

    // Phase scale: -1, 0, +1
    int ph_cx = PH_X + PH_W / 2;
    struct { float v; const char *text; } ph_ticks[] = {
        {-1.0f, "-1"}, {0.0f, "0"}, {1.0f, "+1"}
    };
    for (const auto &tk : ph_ticks) {
        int tx = ph_cx + (int)(tk.v * (float)(PH_W / 2)) - 8;
        make_hint_lbl(tk.text, tx, PH_Y + PH_H + 6);
    }
    // "OUT OF PHASE" / "IN PHASE"
    make_hint_lbl("OUT OF PHASE", PH_X + 20,          PH_Y + PH_H + 6);
    make_hint_lbl("IN PHASE",     PH_X + PH_W - 78,   PH_Y + PH_H + 6);

    // ── Foot bar + Settings ────────────────────────────────────────────────────
    lv_obj_t *right_zone = foot_create_hub_back(scr);

    static const SettingOption decay_opts[] = {{"Fast"}, {"Medium"}, {"Slow"}};
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
    auto *stereo_items = new SettingItem[2];
    stereo_items[0] = {"Gonio Decay", decay_opts,   3, &st->decay_sel    };
    stereo_items[1] = {"Display",     display_opts, 2, &st->display_mode };
    settings_btn_create(right_zone, scr, stereo_items, 2);

    // ── 2D swipe ──────────────────────────────────────────────────────────────
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    st->timer = lv_timer_create(stereo_timer_cb, 50, st);
    render_stereo(st);

    return scr;
}
