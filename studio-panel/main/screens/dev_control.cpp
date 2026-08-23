#include "dev_control.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

lv_obj_t *dev_control_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "DEVICE CTRL");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, THEME_STATUSBAR_H + 24);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Phase 4 — JSON Profiles / SysEx");
    lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(btn_label, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(btn_label);

    return scr;
}
