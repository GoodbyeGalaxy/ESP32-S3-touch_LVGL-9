#include "home.h"
#include "lvgl.h"

void home_screen_create()
{
    // lv_obj_create(NULL) erstellt einen neuen Screen ohne Theme-Taint.
    // lv_screen_active() hat bereits Theme-Styles und padding → bg-child landet
    // in der Content-Area, nicht am absoluten Rand → PSRAM-Grün an den Kanten.
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "STUDIO CONTROL PANEL");
    lv_obj_set_style_text_color(title, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello Studio");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text(ver, "v0.1");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -16, -12);

    lv_screen_load(scr);  // am Ende laden — Screen ist jetzt vollständig konfiguriert
}
