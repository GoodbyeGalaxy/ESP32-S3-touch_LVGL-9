// system.cpp — Merged system/device screen (was Settings + DevCtrl).
// Cards: Network (WiFi toggle + RSSI), Audio In, Chip, Memory, System overview.
// Timer: 1 Hz live updates for WiFi status, RSSI, audio queue, free heap.

#include "system.h"
#include "theme.h"
#include "wifi.h"
#include "audio_data.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/gradient_test.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "lvgl.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdio>

// ── Layout ────────────────────────────────────────────────────────────────────

static constexpr int PAD     = 16;
static constexpr int CARD_W  = (800 - 3 * PAD) / 2;   // 376
static constexpr int CARD_H  = 118;
static constexpr int WIDE_H  = 92;
static constexpr int ROW1_Y  = THEME_CONTENT_Y + 8;    // 40
static constexpr int GAP     = 10;
static constexpr int ROW2_Y  = ROW1_Y + CARD_H + GAP;  // 168
static constexpr int ROW3_Y  = ROW2_Y + CARD_H + GAP;  // 296
static constexpr int COL2_X  = PAD + CARD_W + PAD;     // 408

// ── State ─────────────────────────────────────────────────────────────────────

struct SystemData {
    lv_obj_t   *wifi_dot      = nullptr;
    lv_obj_t   *wifi_status   = nullptr;
    lv_obj_t   *wifi_ip       = nullptr;
    lv_obj_t   *wifi_toggle   = nullptr;   // toggle button label
    lv_obj_t   *audio_dot     = nullptr;
    lv_obj_t   *audio_mode    = nullptr;
    lv_obj_t   *audio_seq     = nullptr;
    lv_obj_t   *sys_line      = nullptr;   // heap + idf + build in one label
    lv_timer_t *timer         = nullptr;
};

// ── Timer: 1 Hz live refresh ──────────────────────────────────────────────────

static void system_timer_cb(lv_timer_t *t)
{
    auto *d = static_cast<SystemData *>(lv_timer_get_user_data(t));

    // ── WiFi ─────────────────────────────────────────────────────────────────
    bool enabled   = wifi_is_enabled();
    bool connected = wifi_is_connected();

    lv_color_t dot_col;
    if (!enabled)   dot_col = lv_color_hex(0x505050);
    else if (connected) dot_col = lv_color_hex(0x3B82F6);
    else            dot_col = lv_color_hex(0xF59E0B);  // amber = trying
    lv_obj_set_style_bg_color(d->wifi_dot, dot_col, 0);

    if (!enabled) {
        lv_label_set_text(d->wifi_status, "Disabled");
        lv_label_set_text(d->wifi_ip, "(WiFi off)");
    } else if (connected) {
        int rssi = wifi_get_rssi();
        char buf[40];
        snprintf(buf, sizeof(buf), "Connected  %d dBm", rssi);
        lv_label_set_text(d->wifi_status, buf);
        char ip[18];
        wifi_get_ip(ip, sizeof(ip));
        lv_label_set_text(d->wifi_ip, ip);
    } else {
        int reason = wifi_get_disconnect_reason();
        char buf[40];
        if (reason) snprintf(buf, sizeof(buf), "Connecting  (dc=%d)", reason);
        else        snprintf(buf, sizeof(buf), "Connecting...");
        lv_label_set_text(d->wifi_status, buf);
        lv_label_set_text(d->wifi_ip, "--");
    }
    lv_label_set_text(d->wifi_toggle, enabled ? "DISABLE WiFi" : "ENABLE WiFi");

    // ── Audio queue ───────────────────────────────────────────────────────────
    AudioPacket pkt{};
    bool live = (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) && (pkt.magic == 0xAB);
    lv_obj_set_style_bg_color(d->audio_dot,
                              lv_color_hex(live ? 0x22C55Eu : 0x707070u), 0);
    lv_label_set_text(d->audio_mode, live ? "Live  UDP 4210" : "Demo  (no signal)");
    if (live) {
        char buf[24];
        snprintf(buf, sizeof(buf), "seq %lu", (unsigned long)pkt.seq);
        lv_label_set_text(d->audio_seq, buf);
    } else {
        lv_label_set_text(d->audio_seq, "");
    }

    // ── System overview ───────────────────────────────────────────────────────
    char sys_buf[64];
    snprintf(sys_buf, sizeof(sys_buf), "%lu kB free   IDF %s   %s %s",
             (unsigned long)(esp_get_free_heap_size() / 1024),
             IDF_VER, __DATE__, __TIME__);
    lv_label_set_text(d->sys_line, sys_buf);
}

static void on_delete(lv_event_t *e)
{
    auto *d = static_cast<SystemData *>(lv_event_get_user_data(e));
    if (d->timer) lv_timer_delete(d->timer);
    delete d;
}

// ── Widget helpers ────────────────────────────────────────────────────────────

static lv_obj_t *make_card(lv_obj_t *scr, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    theme_apply_glow(card);
    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl, 12, 7);
    return card;
}

static lv_obj_t *make_dot(lv_obj_t *card, int rx, int ry)
{
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_pos(dot, rx, ry);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x707070), 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    return dot;
}

static lv_obj_t *make_lbl(lv_obj_t *card, const char *text,
                           const lv_font_t *font, lv_color_t color, int rx, int ry)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_remove_style_all(l);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, rx, ry);
    return l;
}

// ── Screen entry point ────────────────────────────────────────────────────────

lv_obj_t *system_screen_create()
{
    auto *d = new SystemData{};
    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, d);
    statusbar_set_screen_name("SYSTEM");

    // ── NETWORK card ─────────────────────────────────────────────────────────
    lv_obj_t *c_net = make_card(scr, PAD, ROW1_Y, CARD_W, CARD_H, "NETWORK");
    d->wifi_dot    = make_dot(c_net, 12, 30);
    d->wifi_status = make_lbl(c_net, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 28, 24);
    d->wifi_ip     = make_lbl(c_net, "--", THEME_FONT_HINT, THEME_TEXT_PRIMARY, 28, 52);

    // WiFi toggle button
    lv_obj_t *tog_btn = lv_btn_create(c_net);
    lv_obj_remove_style_all(tog_btn);
    lv_obj_set_size(tog_btn, 128, 28);
    lv_obj_set_pos(tog_btn, 12, CARD_H - 38);
    lv_obj_set_style_bg_color(tog_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(tog_btn, THEME_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(tog_btn, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(tog_btn, 1, 0);
    lv_obj_set_style_radius(tog_btn, THEME_RADIUS, 0);
    lv_obj_add_event_cb(tog_btn, [](lv_event_t *) { wifi_toggle(); }, LV_EVENT_CLICKED, nullptr);
    d->wifi_toggle = lv_label_create(tog_btn);
    lv_obj_remove_style_all(d->wifi_toggle);
    lv_label_set_text(d->wifi_toggle, "DISABLE WiFi");
    lv_obj_set_style_text_color(d->wifi_toggle, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(d->wifi_toggle, THEME_FONT_HINT, 0);
    lv_obj_center(d->wifi_toggle);

    // Reconnect button
    lv_obj_t *rc_btn = lv_btn_create(c_net);
    lv_obj_remove_style_all(rc_btn);
    lv_obj_set_size(rc_btn, 110, 28);
    lv_obj_set_pos(rc_btn, CARD_W - 118, CARD_H - 38);
    lv_obj_set_style_bg_color(rc_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(rc_btn, THEME_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(rc_btn, THEME_SEPARATOR, 0);
    lv_obj_set_style_border_width(rc_btn, 1, 0);
    lv_obj_set_style_radius(rc_btn, THEME_RADIUS, 0);
    lv_obj_add_event_cb(rc_btn, [](lv_event_t *) { wifi_reconnect(); }, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *rl = lv_label_create(rc_btn);
        lv_obj_remove_style_all(rl);
        lv_label_set_text(rl, LV_SYMBOL_REFRESH " Reconnect");
        lv_obj_set_style_text_color(rl, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(rl, THEME_FONT_HINT, 0);
        lv_obj_center(rl);
    }

    // ── AUDIO IN card ─────────────────────────────────────────────────────────
    lv_obj_t *c_aud = make_card(scr, COL2_X, ROW1_Y, CARD_W, CARD_H, "AUDIO IN");
    d->audio_dot  = make_dot(c_aud, 12, 30);
    d->audio_mode = make_lbl(c_aud, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 28, 24);
    d->audio_seq  = make_lbl(c_aud, "", THEME_FONT_HINT, THEME_TEXT_HINT, 28, 52);
    make_lbl(c_aud, "UDP port 4210  30 Hz  1080 B",
             THEME_FONT_HINT, THEME_TEXT_HINT, 12, CARD_H - 26);

    // ── CHIP card ─────────────────────────────────────────────────────────────
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    char core_buf[40];
    snprintf(core_buf, sizeof(core_buf), "%d cores  rev %d", chip.cores, chip.revision);

    lv_obj_t *c_chip = make_card(scr, PAD, ROW2_Y, CARD_W, CARD_H, "CHIP");
    make_lbl(c_chip, "ESP32-S3", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 28);
    make_lbl(c_chip, core_buf, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 56);
    make_lbl(c_chip, "Xtensa LX7  dual-core  240 MHz",
             THEME_FONT_HINT, THEME_TEXT_HINT, 12, CARD_H - 26);

    // ── MEMORY card ───────────────────────────────────────────────────────────
    lv_obj_t *c_mem = make_card(scr, COL2_X, ROW2_Y, CARD_W, CARD_H, "MEMORY");
    make_lbl(c_mem, "Flash  16 MB", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 28);
    make_lbl(c_mem, "PSRAM   8 MB  OPI 80 MHz",
             THEME_FONT_HINT, THEME_TEXT_HINT, 12, 56);
    make_lbl(c_mem, "Waveshare ESP32-S3-Touch-LCD-7",
             THEME_FONT_HINT, THEME_TEXT_HINT, 12, CARD_H - 26);

    // ── SYSTEM overview card (full width) ─────────────────────────────────────
    lv_obj_t *c_sys = make_card(scr, PAD, ROW3_Y, 800 - 2 * PAD, WIDE_H, "SYSTEM");
    d->sys_line = make_lbl(c_sys, "---", THEME_FONT_HINT, THEME_TEXT_PRIMARY, 12, 28);

    // ── Footer ────────────────────────────────────────────────────────────────
    lv_obj_t *right_zone = foot_create(scr);

    lv_obj_t *grad_btn = lv_btn_create(right_zone);
    lv_obj_remove_style_all(grad_btn);
    lv_obj_set_size(grad_btn, 130, 36);
    lv_obj_align(grad_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(grad_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(grad_btn, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(grad_btn, THEME_RADIUS, 0);
    lv_obj_add_event_cb(grad_btn, [](lv_event_t *) {
        lv_screen_load_anim(gradient_test_screen_create(),
                            LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, true);
    }, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *gl = lv_label_create(grad_btn);
        lv_obj_remove_style_all(gl);
        lv_label_set_text(gl, "Gradient " LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(gl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(gl, THEME_FONT_HINT, 0);
        lv_obj_center(gl);
    }

    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    d->timer = lv_timer_create(system_timer_cb, 1000, d);
    system_timer_cb(d->timer);   // populate immediately on open
    return scr;
}
