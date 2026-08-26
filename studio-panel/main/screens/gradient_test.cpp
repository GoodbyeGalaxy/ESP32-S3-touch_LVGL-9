#include "gradient_test.h"
#include "theme.h"
#include "screens/statusbar.h"
#include "screens/nav_controller.h"
#include "screens/touch_nav.h"
#include "lvgl.h"
#include <cstdio>

// Dark-calibration: 20 gray steps 0x000000 → 0x262626 (V=0..38 on 0-255 scale, step 2).
// Goal: find the minimum hex value that renders as true gray (no IPS green tint).
// Current baseline: 0x0A0A0A (V=10, step 5) — marked with * and blue label.
// RGB565 note: below V=8 (0x080808), red/blue channels round to 0 while green may not.

static constexpr int STRIPS    = 20;
static constexpr int V_STEP    = 2;       // 0, 2, 4, … 38
static constexpr int STRIP_W   = 800 / STRIPS;  // 40 px
static constexpr int TITLE_H   = 32;
static constexpr int STRIP_H   = 480 - TITLE_H; // 448 px — full height below title
static constexpr int V_CURRENT = 10;      // 0x0A0A0A = current screen bg standard

lv_obj_t *gradient_test_screen_create()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    statusbar_set_screen_name("DARK CAL");

    // Title bar
    {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 800, TITLE_H);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x161B22), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_t *title = lv_label_create(bar);
        lv_label_set_text(title, "DARK CAL  0x000000 \xe2\x86\x92 0x262626   *=baseline 0x0A0A0A");
        lv_obj_set_style_text_color(title, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(title, THEME_FONT_HINT, 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    }

    for (int i = 0; i < STRIPS; i++) {
        int v = i * V_STEP;  // 0, 2, 4, … 38
        lv_color_t col = lv_color_make((uint8_t)v, (uint8_t)v, (uint8_t)v);

        // Strip — full height below title bar
        lv_obj_t *strip = lv_obj_create(scr);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, STRIP_W, STRIP_H);
        lv_obj_set_pos(strip, i * STRIP_W, TITLE_H);
        lv_obj_set_style_bg_color(strip, col, 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(strip, 0, 0);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

        // Hex label inside strip — top area
        bool is_current = (v == V_CURRENT);
        char buf[6];
        snprintf(buf, sizeof(buf), is_current ? "*%02X" : "%02X", (unsigned)v);

        lv_obj_t *lbl = lv_label_create(strip);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(is_current ? 0x58A6FFu : 0xA0A0A0u), 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);
    }

    // Swipe right → back via nav_controller (gradient is at {2,2} in the nav grid)
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    return scr;
}
