#include "statusbar.h"
#include "theme.h"
#include "touch.h"
#include "lvgl.h"
#include "esp_lcd_touch.h"

static lv_obj_t *s_bar       = nullptr;
static lv_obj_t *s_title     = nullptr;
static lv_obj_t *s_wifi      = nullptr;
static lv_obj_t *s_time      = nullptr;
static lv_obj_t *s_build     = nullptr;
static lv_obj_t *s_touch_dot = nullptr;

void statusbar_init()
{
    lv_obj_t *top = lv_layer_top();

    s_bar = lv_obj_create(top);
    lv_obj_set_size(s_bar, LV_HOR_RES, THEME_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x686868), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Titel links
    s_title = lv_label_create(s_bar);
    lv_label_set_text(s_title, "STUDIO PANEL");
    lv_obj_set_style_text_color(s_title, THEME_TEXT_TITLE, 0);
    lv_obj_set_style_text_font(s_title, THEME_FONT_HINT, 0);
    lv_obj_set_style_bg_color(s_title, lv_color_hex(0x484848), 0);
    lv_obj_set_style_bg_opa(s_title, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_title, 5, 0);
    lv_obj_set_style_pad_hor(s_title, 8, 0);
    lv_obj_set_style_pad_ver(s_title, 2, 0);
    lv_obj_set_style_shadow_width(s_title, 8, 0);
    lv_obj_set_style_shadow_color(s_title, THEME_ACCENT, 0);
    lv_obj_set_style_shadow_opa(s_title, LV_OPA_20, 0);
    lv_obj_set_style_shadow_spread(s_title, 1, 0);
    lv_obj_set_style_shadow_offset_x(s_title, 0, 0);
    lv_obj_set_style_shadow_offset_y(s_title, 0, 0);
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

    // Touch-Indikator — Pill, leuchtet hell bei Touch
    s_touch_dot = lv_obj_create(s_bar);
    lv_obj_remove_style_all(s_touch_dot);
    lv_obj_set_size(s_touch_dot, 20, 20);
    lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_touch_dot, lv_color_hex(0x404040), 0);
    lv_obj_align(s_touch_dot, LV_ALIGN_LEFT_MID, 170, 0);

    // Touch-Poll alle 100ms — aktualisiert Dot-Farbe
    lv_timer_create([](lv_timer_t *) {
        esp_lcd_touch_handle_t h = touch_get_handle();
        if (!h || !s_touch_dot) return;
        esp_lcd_touch_read_data(h);
        uint16_t x[1], y[1], strength[1];
        uint8_t cnt = 0;
        bool touched = esp_lcd_touch_get_coordinates(h, x, y, strength, &cnt, 1) && cnt > 0;
        lv_obj_set_style_bg_color(s_touch_dot,
            touched ? lv_color_hex(0x00FF00) : lv_color_hex(0x404040), 0);
    }, 100, nullptr);

    // Zeit rechts
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_time, THEME_FONT_HINT, 0);
    lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -12, 0);

    // WiFi-Status
    s_wifi = lv_label_create(s_bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, THEME_TEXT_SECONDARY, 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -60, 0);
}

void statusbar_update_wifi(bool connected, const char *ip_str)
{
    if (!s_wifi) return;
    lv_obj_set_style_text_color(s_wifi,
        connected ? THEME_ACCENT : THEME_TEXT_HINT, 0);
    if (connected && ip_str) {
        lv_label_set_text_fmt(s_wifi, LV_SYMBOL_WIFI " %s", ip_str);
    } else if (!connected) {
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    }
}

void statusbar_update_time(const char *time_str)
{
    if (!s_time) return;
    lv_label_set_text(s_time, time_str);
}
