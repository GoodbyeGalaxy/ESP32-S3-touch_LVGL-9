#pragma once
#include "lvgl.h"

// ── Farben ────────────────────────────────────────────────────
#define THEME_BG_PRIMARY      lv_color_hex(0x606060)   // Screen-Hintergrund
#define THEME_BG_CARD         lv_color_hex(0x747474)   // Kacheln / Cards
#define THEME_BG_CARD_HOVER   lv_color_hex(0x888888)   // Pressed-State
#define THEME_ACCENT          lv_color_hex(0x3B82F6)   // Primärfarbe (Blau)
#define THEME_ACCENT_DIM      lv_color_hex(0x1E3A5F)   // Gedämpftes Akzent
#define THEME_TEXT_PRIMARY    lv_color_hex(0xF0F0F0)   // Haupttext
#define THEME_TEXT_TITLE      lv_color_hex(0xFFFFFF)   // Titel-Highlights — reines Weiß
#define THEME_TEXT_SECONDARY  lv_color_hex(0x888888)   // Sekundärtext
#define THEME_TEXT_HINT       lv_color_hex(0x909090)   // Hinweise — über 38% Luminanz (IPS-Grüntint-Grenze)
#define THEME_SEPARATOR       lv_color_hex(0x707070)   // Trennlinien

// ── Geometrie ─────────────────────────────────────────────────
#define THEME_TILE_W          236    // Kachelbreite (3 × 236 + 2 × 36 = 800)
#define THEME_TILE_H          196    // Kachelhöhe   (2 × 196 + 3 × 29 = 479)
#define THEME_TILE_GAP        36     // Horizontaler Abstand
#define THEME_TILE_GAP_V      29     // Vertikaler Abstand
#define THEME_STATUSBAR_H     32     // Höhe der Statusleiste
#define THEME_RADIUS          8      // Globaler Border-Radius

// ── Typografie ────────────────────────────────────────────────
#define THEME_LETTER_SPACE_TITLE  2    // px zwischen Titelzeichen — verbessert Lesbarkeit

// ── Fonts ─────────────────────────────────────────────────────
// Montserrat für Titel (geschwungen, groß — kein Bleeding bei 24px)
// unscii_16 für Labels/Hints — pixel-perfekt, kein Anti-Aliasing-Bleeding
#define THEME_FONT_TITLE      (&lv_font_montserrat_24)
#define THEME_FONT_LABEL      (&lv_font_montserrat_16)
#define THEME_FONT_HINT       (&lv_font_montserrat_16)

// ── Helper: neuen Screen mit Dark-Background erstellen ────────
// Gibt IMMER lv_obj_create(nullptr) zurück — nie lv_screen_active() verwenden.
static inline lv_obj_t *theme_make_screen()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, THEME_BG_PRIMARY, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}
