#pragma once
#include "lvgl.h"
#include <stdint.h>

// ── Visual Mode Interface ─────────────────────────────────────────────────────
// All mode renderers live in visuals_modes.cpp and are dispatched via index.

// Global mood score (0=dark/bass-heavy, 1=bright/treble-rich).
// Updated by visuals.cpp render timer; read by all mode renderers.
extern float g_visuals_mood;

// IN: mode_idx 0..7, canvas. OUT: nothing. Initialises mode state, clears canvas.
void visuals_mode_init(int mode_idx, lv_obj_t *canvas);

// IN: mode_idx 0..7, canvas, t_ms monotonic timestamp. OUT: nothing.
// Called every 33ms from the render timer.
void visuals_mode_tick(int mode_idx, lv_obj_t *canvas, uint32_t t_ms);

// IN: mode_idx 0..7, canvas. OUT: nothing. Frees any mode-local state.
void visuals_mode_deinit(int mode_idx, lv_obj_t *canvas);

// IN: mode_idx 0..7, canvas, touch coordinates. OUT: nothing.
void visuals_mode_touch(int mode_idx, lv_obj_t *canvas, int x, int y);

// Returns true when demo was explicitly forced by the user (DEMO button).
// Mirrors the forced state set via visuals_modes_set_demo_forced().
bool demo_signal_is_forced_by_user();

// IN: v — forced state from DEMO button. OUT: nothing. Call alongside
// demo_signal_set_forced(v) so button label stays in sync.
void visuals_modes_set_demo_forced(bool v);
