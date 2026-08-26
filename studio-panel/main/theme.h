#pragma once
#include "lvgl.h"

// ── Theme System ──────────────────────────────────────────────────────────────
// Swap g_theme pointer at runtime to switch themes. All macros resolve at
// runtime via the pointer — callers need no changes when switching themes.

struct ThemeColors {
    lv_color_t bg_primary;      // screen background
    lv_color_t bg_card;         // card / tile surface
    lv_color_t bg_card_hover;   // card pressed state
    lv_color_t accent;          // primary accent (also used for glow)
    lv_color_t accent_dim;      // dimmed accent for inactive elements
    lv_color_t text_primary;    // main text
    lv_color_t text_title;      // titles / headings
    lv_color_t text_secondary;  // secondary info
    lv_color_t text_hint;       // subtle hints / labels
    lv_color_t separator;       // divider lines
    lv_color_t statusbar_bg;    // head bar background
    lv_color_t foot_bg;         // foot bar background
    // Glow / shadow applied by theme_apply_glow()
    lv_color_t glow_color;
    lv_opa_t   glow_opa;
    int32_t    glow_width;
    int32_t    glow_spread;
};

// Active theme — swap via theme_set() to change themes at runtime.
extern const ThemeColors *g_theme;

// IN: non-null pointer to a static-lifetime ThemeColors. OUT: switches active theme.
void theme_set(const ThemeColors *t);

// Predefined themes
extern const ThemeColors THEME_DARK_GLOW;

// ── Color macros (syntax unchanged in all callers) ────────────────────────────
#define THEME_BG_PRIMARY      (g_theme->bg_primary)
#define THEME_BG_CARD         (g_theme->bg_card)
#define THEME_BG_CARD_HOVER   (g_theme->bg_card_hover)
#define THEME_ACCENT          (g_theme->accent)
#define THEME_ACCENT_DIM      (g_theme->accent_dim)
#define THEME_TEXT_PRIMARY    (g_theme->text_primary)
#define THEME_TEXT_TITLE      (g_theme->text_title)
#define THEME_TEXT_SECONDARY  (g_theme->text_secondary)
#define THEME_TEXT_HINT       (g_theme->text_hint)
#define THEME_SEPARATOR       (g_theme->separator)
#define THEME_STATUSBAR_BG    (g_theme->statusbar_bg)
#define THEME_FOOT_BG         (g_theme->foot_bg)

// ── Glow helper ───────────────────────────────────────────────────────────────
// Applies the active theme's glow/shadow to an LVGL object.
// Call on cards, tiles, accent buttons — NOT on display canvases.
static inline void theme_apply_glow(lv_obj_t *obj)
{
    lv_obj_set_style_shadow_color(obj,  g_theme->glow_color,  0);
    lv_obj_set_style_shadow_width(obj,  g_theme->glow_width,  0);
    lv_obj_set_style_shadow_spread(obj, g_theme->glow_spread, 0);
    lv_obj_set_style_shadow_opa(obj,    g_theme->glow_opa,    0);
}

// ── Geometry ──────────────────────────────────────────────────────────────────
#define THEME_TILE_W          236
#define THEME_TILE_H          196
#define THEME_TILE_GAP        36
#define THEME_TILE_GAP_V      29
#define THEME_STATUSBAR_H     32
#define THEME_FOOT_H          56
#define THEME_FOOT_Y          (480 - THEME_FOOT_H)
#define THEME_CONTENT_Y       THEME_STATUSBAR_H
#define THEME_CONTENT_H       (THEME_FOOT_Y - THEME_STATUSBAR_H)
#define THEME_RADIUS          8

// ── Typography ────────────────────────────────────────────────────────────────
#define THEME_LETTER_SPACE_TITLE  2
#define THEME_FONT_TITLE      (&lv_font_montserrat_24)
#define THEME_FONT_LABEL      (&lv_font_montserrat_18)
#define THEME_FONT_HINT       (&lv_font_montserrat_14)
#define THEME_FONT_NUM        (&lv_font_unscii_16)

// ── Screen helper ─────────────────────────────────────────────────────────────
// Always use lv_obj_create(nullptr) — never lv_screen_active().
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
