#include "dev_control.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "lvgl.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include <cstdio>

// Device diagnostics and control (Phase 4: JSON Profiles / SysEx).
// Shows chip info and memory stats; profile/SysEx wiring comes in Phase 4.

// ── Navigation ────────────────────────────────────────────────────────────────

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

// IN: direction (+1=fwd, -1=back). OUT: loads home on back swipe.
static void on_swipe(int direction, void *user_data)
{
    if (direction == -1) {
        lv_obj_t *home = home_screen_create();
        lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    }
}

// ── Widget helpers ────────────────────────────────────────────────────────────

// IN: parent, pos (x,y), size (w,h), title. OUT: styled info card.
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

// IN: parent, text, font, color, pos (rx, ry). OUT: label (LVGL-owned).
static lv_obj_t *make_row(lv_obj_t *parent, const char *text,
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
lv_obj_t *dev_control_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "DEVICE CTRL");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, 20, 8);

    constexpr int PAD = 20, CW = 360, CH = 165;
    constexpr int ROW1_Y = 52, ROW2_Y = ROW1_Y + CH + PAD;
    constexpr int COL2_X = PAD + CW + PAD;

    // ── Chip card ─────────────────────────────────────────────────────────────
    esp_chip_info_t chip{};
    esp_chip_info(&chip);

    lv_obj_t *c_chip = make_card(scr, PAD, ROW1_Y, CW, CH, "CHIP");
    make_row(c_chip, "ESP32-S3", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);

    char core_buf[32];
    snprintf(core_buf, sizeof(core_buf), "%d cores  rev %d",
             chip.cores, chip.revision);
    make_row(c_chip, core_buf, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── Memory card ───────────────────────────────────────────────────────────
    lv_obj_t *c_mem = make_card(scr, COL2_X, ROW1_Y, CW, CH, "MEMORY");

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    char flash_buf[32];
    snprintf(flash_buf, sizeof(flash_buf), "Flash  %lu MB", (unsigned long)(flash_size >> 20));
    make_row(c_mem, flash_buf, THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);

    char psram_buf[32];
    size_t psram_size = esp_psram_get_size();
    snprintf(psram_buf, sizeof(psram_buf), "PSRAM  %zu MB", psram_size >> 20);
    make_row(c_mem, psram_buf, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── Profiles card ─────────────────────────────────────────────────────────
    lv_obj_t *c_prof = make_card(scr, PAD, ROW2_Y, CW, CH, "PROFILES");
    make_row(c_prof, "JSON Profiles", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_row(c_prof, "Phase 4 — NVS storage pending", THEME_FONT_HINT,
             THEME_TEXT_HINT, 12, 72);

    // ── SysEx card ────────────────────────────────────────────────────────────
    lv_obj_t *c_sx = make_card(scr, COL2_X, ROW2_Y, CW, CH, "SYSEX");
    make_row(c_sx, "SysEx Monitor", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_row(c_sx, "Phase 4 — USB MIDI pending", THEME_FONT_HINT,
             THEME_TEXT_HINT, 12, 72);

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

    return scr;
}
