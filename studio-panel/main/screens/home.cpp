#include "home.h"
#include "lvgl.h"
#include "esp_log.h"

void home_screen_create()
{
    // Neuen Screen erstellen — Theme ist zu diesem Zeitpunkt bereits NULL
    // sodass keine Theme-Styles vorbelegt sind
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Titelzeile oben (subtiles Grau)
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "STUDIO CONTROL PANEL");
    lv_obj_set_style_text_color(title, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    // Haupttext zentriert (helles Weiß)
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello Studio");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // Version unten rechts
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text(ver, "v0.1");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -16, -12);

    // Roter Touch-Dot: visuelles Feedback bei Berührung
    static lv_obj_t *touch_dot = nullptr;
    touch_dot = lv_obj_create(scr);
    lv_obj_set_size(touch_dot, 20, 20);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(0xFF4444), LV_PART_MAIN);
    lv_obj_set_style_border_width(touch_dot, 0, LV_PART_MAIN);
    lv_obj_add_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);

    // Kontext-Struct: Dot-Pointer an Callback übergeben
    struct TouchCtx { lv_obj_t *dot; };
    static TouchCtx ctx;
    ctx.dot = touch_dot;

    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI("home", "Touch: x=%d y=%d", (int)p.x, (int)p.y);
        TouchCtx *c = static_cast<TouchCtx *>(lv_event_get_user_data(e));
        lv_obj_align(c->dot, LV_ALIGN_TOP_LEFT, p.x - 10, p.y - 10);
        lv_obj_clear_flag(c->dot, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_PRESSING, &ctx);

    lv_screen_load(scr);
}
