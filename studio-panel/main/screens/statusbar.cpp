#include "statusbar.h"
#include "theme.h"
#include "lvgl.h"

static lv_obj_t *s_bar   = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_wifi  = nullptr;
static lv_obj_t *s_time  = nullptr;

void statusbar_init()
{
    lv_obj_t *top = lv_layer_top();

    s_bar = lv_obj_create(top);
    lv_obj_set_size(s_bar, LV_HOR_RES, THEME_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x686868), 0);  // ≥38% Luminanz für IPS-Panel
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Titel links
    s_title = lv_label_create(s_bar);
    lv_label_set_text(s_title, "STUDIO PANEL");
    lv_obj_set_style_text_color(s_title, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_title, THEME_FONT_HINT, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 12, 0);

    // Zeit rechts
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_time, THEME_FONT_HINT, 0);
    lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -12, 0);

    // WiFi-Status
    s_wifi = lv_label_create(s_bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, THEME_TEXT_HINT, 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -50, 0);
}

void statusbar_update_wifi(bool connected)
{
    if (!s_wifi) return;
    lv_obj_set_style_text_color(s_wifi,
        connected ? THEME_ACCENT : THEME_TEXT_HINT, 0);
}

void statusbar_update_time(const char *time_str)
{
    if (!s_time) return;
    lv_label_set_text(s_time, time_str);
}
