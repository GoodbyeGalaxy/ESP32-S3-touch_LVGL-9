#include "gradient_test.h"
#include "theme.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "lvgl.h"

// Brightness calibration screen — 20 strips from V=0 to V=57 (HSV, S=0, H=0).
// Identifies the minimum brightness before IPS green-tint appears.
// Temporary diagnostic screen; remove once calibration is done.

// IN: nothing. OUT: full-screen brightness gradient lv_obj_t* (not loaded by this fn).
lv_obj_t *gradient_test_screen_create()
{
    lv_obj_t *scr = lv_obj_create(nullptr);

    statusbar_set_screen_name("GRADIENT TEST");
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static constexpr int STRIPS     = 20;
    static constexpr int STRIP_W    = 800 / STRIPS;   // 40 px
    static constexpr int STRIP_H    = 380;
    static constexpr int LABEL_Y    = STRIP_H + 4;

    for (int i = 0; i < STRIPS; i++) {
        // V on 0–255 scale: 0 (black) → 255 (white), steps of 13
        uint8_t v = (uint8_t)(i * 255 / (STRIPS - 1));
        lv_color_t col = lv_color_hsv_to_rgb(0, 0, v);

        // Colour strip
        lv_obj_t *strip = lv_obj_create(scr);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, STRIP_W, STRIP_H);
        lv_obj_set_pos(strip, i * STRIP_W, 32);
        lv_obj_set_style_bg_color(strip, col, 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(strip, 0, 0);

        // V= label
        // Show both 0-255 scale value and approximate hex
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", v);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, buf);
        // Label colour: invert against strip brightness so it's always readable
        lv_obj_set_style_text_color(lbl, lv_color_hex(v < 30 ? 0xC8C8C8u : 0x202020u), 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_pos(lbl, i * STRIP_W + 2, 32 + LABEL_Y);
    }

    // Title bar
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, 32);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x686868), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "BRIGHTNESS CALIBRATION  —  V=0 (left) … V=57 (right)");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_HINT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    foot_create(scr);

    return scr;
}
