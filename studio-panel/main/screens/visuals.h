#pragma once
#include "lvgl.h"

// ── Visuals Screen ─────────────────────────────────────────────────────────
// Tile-Picker (4×2, 8 visual modes) + Fullscreen per mode.
// NVS persists the last active mode index across power cycles.

// ── Color palette system ───────────────────────────────────────────────────
// All visual modes source their colors from the active palette.
// Mood score modulates brightness/saturation only — never the hue.

struct VisualPalette {
    const char *name;
    lv_color_t  primary;    // main content (curves, particles, rings)
    lv_color_t  secondary;  // trails, fades, background glow
    lv_color_t  accent;     // peaks, beats, transients
    lv_color_t  bg_tint;    // subtle dark background tint (very dark)
};

// IN: nothing. OUT: pointer to currently active VisualPalette (never null).
const VisualPalette *visuals_get_palette();

// IN: index 0..4. OUT: nothing. Persists to NVS, notifies active mode.
void visuals_set_palette(int index);

// IN: nothing. OUT: tile-picker screen lv_obj_t* (not loaded — caller calls
// lv_screen_load / lv_screen_load_anim).
lv_obj_t *visuals_screen_create();

// IN: mode_index 0..7. OUT: loads fullscreen for that mode (replaces current screen).
void visuals_fullscreen_enter(int mode_index);

// IN: nothing. OUT: returns to tile-picker screen.
void visuals_fullscreen_exit();

// IN: nothing. OUT: advance to next mode (wraps). Called by fullscreen swipe-left.
void visuals_mode_next();

// IN: nothing. OUT: go to previous mode (wraps). Called by fullscreen swipe-right.
void visuals_mode_prev();

// Called by fullscreen render timer — dispatches to active mode renderer.
// IN: lv_timer_t* with user_data = canvas lv_obj_t*. OUT: nothing.
void visuals_render_tick(lv_timer_t *t);
