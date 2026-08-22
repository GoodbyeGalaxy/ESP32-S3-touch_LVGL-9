#include "home.h"
#include "lvgl.h"

void home_screen_create()
{
    lv_obj_t *scr = lv_scr_act();

    // Hintergrund: fast schwarz (#0A0A0A)
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

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
}
