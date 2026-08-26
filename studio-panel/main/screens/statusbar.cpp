#include "statusbar.h"
#include "theme.h"
#include "touch.h"
#include "lvgl.h"
#include "esp_lcd_touch.h"

static lv_obj_t *s_bar   = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_wifi  = nullptr;
static lv_obj_t *s_time  = nullptr;
static lv_obj_t *s_build = nullptr;

void statusbar_init()
{
    lv_obj_t *top = lv_layer_top();

    s_bar = lv_obj_create(top);
    lv_obj_set_size(s_bar, LV_HOR_RES, THEME_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, THEME_STATUSBAR_BG, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Screen-Name links (dynamisch via statusbar_set_screen_name)
    s_title = lv_label_create(s_bar);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, THEME_TEXT_TITLE, 0);
    lv_obj_set_style_text_font(s_title, THEME_FONT_LABEL, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 12, 0);

    // Build-Timestamp zentriert — Montserrat 18, gut lesbar für Deploy-Verifikation
    // Format: "Aug 24  21:43"  (__DATE__ liefert "Mmm DD YYYY", __TIME__ "HH:MM:SS")
    s_build = lv_label_create(s_bar);
    lv_obj_remove_style_all(s_build);
    lv_obj_set_size(s_build, 220, THEME_STATUSBAR_H);
    lv_label_set_long_mode(s_build, LV_LABEL_LONG_CLIP);
    lv_label_set_text_fmt(s_build, "%.6s  %.5s", __DATE__, __TIME__);
    lv_obj_set_style_text_color(s_build, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_build, THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_align(s_build, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_build, LV_ALIGN_CENTER, 0, 0);

    // WiFi-Status (ganz rechts)
    s_wifi = lv_label_create(s_bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, THEME_TEXT_SECONDARY, 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -12, 0);

    // Zeit (links vom WiFi-Icon; zunächst hidden bis SNTP sync)
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "");
    lv_obj_set_style_text_color(s_time, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_time, THEME_FONT_HINT, 0);
    lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
}

void statusbar_set_screen_name(const char *name)
{
    if (!s_title || !name) return;
    lv_label_set_text(s_title, name);
}

void statusbar_update_time(const char *time_str)
{
    if (!s_time) return;
    lv_label_set_text(s_time, time_str);
    lv_obj_clear_flag(s_time, LV_OBJ_FLAG_HIDDEN);  // reveal on first SNTP sync
}

void statusbar_update_wifi(bool connected, const char *ip_str)
{
    if (!s_wifi) return;
    // Symbol only — green when connected, gray when not. No IP displayed.
    lv_obj_set_style_text_color(s_wifi,
        connected ? lv_color_hex(0x22C55Eu) : THEME_TEXT_HINT, 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
}

