#include "home.h"
#include "theme.h"
#include "screens/metering.h"
#include "screens/studio_one.h"
#include "screens/usb_midi.h"
#include "screens/routing.h"
#include "screens/dev_control.h"
#include "screens/settings.h"
#include "screens/spectrum.h"
#include "lvgl.h"

struct TileDef {
    const char *symbol;
    const char *label;
    const char *hint;
    lv_obj_t *(*create_screen)();
};

static const TileDef TILES[6] = {
    { LV_SYMBOL_AUDIO,    "METERING",    "Pegel / RMS / LUFS",  metering_screen_create    },
    { LV_SYMBOL_PLAY,     "STUDIO ONE",  "DAW Control",         studio_one_screen_create  },
    { LV_SYMBOL_SHUFFLE,  "USB MIDI",    "CC / Nord Lead 2X",   usb_midi_screen_create    },
    { LV_SYMBOL_BARS,     "SPECTRUM",    "FFT / Waterfall",     spectrum_screen_create    },
    { LV_SYMBOL_SETTINGS, "DEVICE CTRL", "JSON Profiles",       dev_control_screen_create },
    { LV_SYMBOL_SETTINGS, "SETTINGS",    "Config / OTA",        settings_screen_create    },
};

static void on_tile_clicked(lv_event_t *e)
{
    auto *def = static_cast<const TileDef *>(lv_event_get_user_data(e));
    lv_obj_t *target = def->create_screen();
    lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, true);
}

lv_obj_t *home_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;

        int x = col * (THEME_TILE_W + THEME_TILE_GAP);
        int y = THEME_STATUSBAR_H + THEME_TILE_GAP_V
              + row * (THEME_TILE_H + THEME_TILE_GAP_V);

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_set_size(tile, THEME_TILE_W, THEME_TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD, 0);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
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
    }

    return scr;
}

void home_screen_load()
{
    lv_obj_t *scr = home_screen_create();
    lv_screen_load(scr);
}
