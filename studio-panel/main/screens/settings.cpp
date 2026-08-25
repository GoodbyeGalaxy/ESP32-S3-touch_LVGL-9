#include "settings.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "wifi.h"
#include "audio_data.h"
#include "lvgl.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdio>

// ── Layout constants ──────────────────────────────────────────────────────────

static constexpr int PAD      = 20;
static constexpr int CARD_W   = (800 - 3 * PAD) / 2;   // 370
static constexpr int CARD_H   = 160;
static constexpr int ROW1_Y   = 52;                       // below statusbar
static constexpr int ROW2_Y   = ROW1_Y + CARD_H + PAD;   // 232
static constexpr int COL2_X   = PAD + CARD_W + PAD;      // 410

// ── State ─────────────────────────────────────────────────────────────────────

struct SettingsData {
    lv_obj_t   *wifi_dot    = nullptr;
    lv_obj_t   *wifi_status = nullptr;
    lv_obj_t   *audio_dot   = nullptr;
    lv_obj_t   *audio_mode  = nullptr;
    lv_obj_t   *audio_seq   = nullptr;
    lv_obj_t   *sys_heap    = nullptr;
    lv_timer_t *timer       = nullptr;
};

// ── Timer: refresh live data at 1Hz ──────────────────────────────────────────

// Called at 1Hz from LVGL timer (already locked). Reads wifi_is_connected() and g_audio_queue.
static void settings_timer_cb(lv_timer_t *t)
{
    auto *d = static_cast<SettingsData*>(lv_timer_get_user_data(t));

    // WiFi
    bool connected = wifi_is_connected();
    lv_obj_set_style_bg_color(d->wifi_dot,
        lv_color_hex(connected ? 0x3B82F6u : 0x707070u), 0);
    lv_label_set_text(d->wifi_status, connected ? "Connected" : "Disconnected");

    // Audio queue: peek latest packet (non-destructive)
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

    // Free heap
    char heap_buf[24];
    snprintf(heap_buf, sizeof(heap_buf), "%lu kB free",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    lv_label_set_text(d->sys_heap, heap_buf);
}

static void on_delete(lv_event_t *e)
{
    auto *d = static_cast<SettingsData*>(lv_event_get_user_data(e));
    if (d->timer) lv_timer_delete(d->timer);
    delete d;
}

// ── Navigation ────────────────────────────────────────────────────────────────

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

static void on_swipe(int direction, void *user_data)
{
    if (direction == -1) {
        lv_obj_t *home = home_screen_create();
        lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    }
}

// ── Widget helpers ────────────────────────────────────────────────────────────

// IN: parent, pos (x,y), size (w,h), title string. OUT: styled card container.
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl, 12, 8);
    return card;
}

// IN: parent card, relative pos (rx, ry). OUT: small status-dot lv_obj_t (initially gray).
static lv_obj_t *make_dot(lv_obj_t *card, int rx, int ry)
{
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_pos(dot, rx, ry);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x707070), 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    return dot;
}

// IN: parent, text, font, color, pos (rx, ry). OUT: label (LVGL-owned).
static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color, int rx, int ry)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_pos(lbl, rx, ry);
    return lbl;
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *settings_screen_create()
{
    auto *d = new SettingsData{};
    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, d);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, PAD, 8);

    // ── WiFi card ─────────────────────────────────────────────────────────────
    lv_obj_t *c_wifi = make_card(scr, PAD, ROW1_Y, CARD_W, CARD_H, "NETWORK");
    d->wifi_dot    = make_dot(c_wifi, 12, 36);
    d->wifi_status = make_label(c_wifi, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 30, 30);
    make_label(c_wifi, "WiFi Station", THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── Audio card ────────────────────────────────────────────────────────────
    lv_obj_t *c_audio = make_card(scr, COL2_X, ROW1_Y, CARD_W, CARD_H, "AUDIO IN");
    d->audio_dot  = make_dot(c_audio, 12, 36);
    d->audio_mode = make_label(c_audio, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 30, 30);
    d->audio_seq  = make_label(c_audio, "", THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── System card ───────────────────────────────────────────────────────────
    lv_obj_t *c_sys = make_card(scr, PAD, ROW2_Y, CARD_W, CARD_H, "SYSTEM");
    d->sys_heap = make_label(c_sys, "--- kB", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_label(c_sys, IDF_VER, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── Build card ────────────────────────────────────────────────────────────
    lv_obj_t *c_build = make_card(scr, COL2_X, ROW2_Y, CARD_W, CARD_H, "BUILD");
    make_label(c_build, __DATE__, THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_label(c_build, __TIME__, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 90, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(btn_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(btn_lbl);

    touch_nav_attach(scr, on_swipe, nullptr);

    d->timer = lv_timer_create(settings_timer_cb, 1000, d);
    settings_timer_cb(d->timer);  // populate immediately on screen open

    return scr;
}
