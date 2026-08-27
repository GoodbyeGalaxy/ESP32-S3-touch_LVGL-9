#include "home.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"

struct TileDef {
    const char *symbol;
    const char *label;
    const char *hint;
    NavPos      pos;
};

static const TileDef TILES[6] = {
    { LV_SYMBOL_AUDIO,    "METERING",    "Pegel / RMS / LUFS", {0, 0} },
    { LV_SYMBOL_PLAY,     "STUDIO ONE",  "DAW Control",        {1, 0} },
    { LV_SYMBOL_SHUFFLE,  "USB MIDI",    "CC / Nord Lead 2X",  {1, 1} },
    { LV_SYMBOL_IMAGE,    "VISUALS",     "Visual Modes",       {0, 1} },
    { LV_SYMBOL_SETTINGS, "DEVICE CTRL", "JSON Profiles",      {2, 1} },
    { LV_SYMBOL_SETTINGS, "SETTINGS",    "Config / OTA",       {2, 0} },
};

static void on_tile_clicked(lv_event_t *e)
{
    auto *def = static_cast<const TileDef *>(lv_event_get_user_data(e));
    nav_go(def->pos);
}

lv_obj_t *home_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    statusbar_set_screen_name("HOME");

    // Tile layout for Content area (Y=32..424, H=392):
    // 3 cols × 256px + 2 gaps × 16px + 2 margins × 16px = 768+32 = 800 ✓
    // 2 rows × 178px + 3 gaps × 12px = 356+36 = 392 ✓
    constexpr int TILE_W      = 256;
    constexpr int TILE_H      = 178;
    constexpr int GAP_H       = 16;   // horizontal gap between tiles
    constexpr int GAP_V       = 12;   // vertical gap (also used as top/bottom margin)
    constexpr int LEFT_MARGIN = 10;   // left margin

    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;

        int x = LEFT_MARGIN + col * (TILE_W + GAP_H);
        int y = THEME_CONTENT_Y + GAP_V
              + row * (TILE_H + GAP_V);

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD, 0);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, THEME_ACCENT, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, THEME_RADIUS, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        // Symbol
        lv_obj_t *sym = lv_label_create(tile);
        lv_label_set_text(sym, TILES[i].symbol);
        lv_obj_set_style_text_color(sym, THEME_ACCENT, 0);
        lv_obj_set_style_text_font(sym, THEME_FONT_TITLE, 0);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 28);

        // Label
        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, TILES[i].label);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_LABEL, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        // Hint
        lv_obj_t *hint = lv_label_create(tile);
        lv_label_set_text(hint, TILES[i].hint);
        lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

        lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED,
                            const_cast<TileDef *>(&TILES[i]));
        theme_apply_glow(tile);
    }

    // Foot bar — Home button links, kein screen-spezifischer right_zone-Inhalt nötig
    foot_create(scr);

    // 2D Swipe-Navigation — routes through nav_controller.
    // Gesture-Events bubbeln von den Tiles hoch (kein CLICKABLE-Overlay noetig).
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    return scr;
}

void home_screen_load()
{
    lv_obj_t *scr = home_screen_create();
    lv_screen_load(scr);
}
