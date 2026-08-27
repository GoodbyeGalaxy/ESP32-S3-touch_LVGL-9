// nav_controller.cpp — Row & Column 2D navigation for the studio panel.
//
// Grid layout:
//   Row 0  AUDIO:    [Metering col 0] ↔ [Visuals col 1]
//   Row 1  CONTROL:  [Studio One col 0] ↔ [USB MIDI col 1] ↔ [Routing col 2]
//   Row 2  SYSTEM:   [Settings col 0] ↔ [Dev Control col 1]
//
// Spectrum is now a swipe-up sub-view within Metering (not a standalone screen).
//
// Navigation rules:
//   Swipe LEFT  (dir_h=-1): col+1 in same row. No-op at last col.
//   Swipe RIGHT (dir_h=+1): col-1 in same row. At col 0: go Home.
//   Swipe UP    (dir_v=-1): row-1, clamping col to new row's max. At row 0: go Home.
//   Swipe DOWN  (dir_v=+1): row+1, clamping col to new row's max. No-op at last row.
//   From Home:
//     Swipe LEFT  → row 0, col 0 (Metering)
//     Swipe UP    → previous row entry (cycles: Settings → Studio One → Metering)
//     Swipe DOWN  → next row entry    (cycles: Metering → Studio One → Settings)

#include "nav_controller.h"

#include "esp_timer.h"
#include "screens/metering.h"
#include "screens/visuals.h"
#include "screens/studio_one.h"
#include "screens/usb_midi.h"
#include "screens/routing.h"
#include "screens/settings.h"
#include "screens/dev_control.h"
#include "screens/gradient_test.h"
#include "screens/home.h"

// ---------------------------------------------------------------------------
// Grid definition
// ---------------------------------------------------------------------------

using ScreenFactory = lv_obj_t*(*)();

struct RowDef {
    int            col_count;
    ScreenFactory  factories[3];   // max 3 cols per row
};

static constexpr int ROW_COUNT = 3;

static const RowDef ROWS[ROW_COUNT] = {
    // Row 0: AUDIO  — Spectrum is now a swipe-up sub-view inside Metering
    { 2, { metering_screen_create, visuals_screen_create, nullptr } },
    // Row 1: CONTROL
    { 3, { studio_one_screen_create, usb_midi_screen_create, routing_screen_create } },
    // Row 2: SYSTEM
    { 3, { settings_screen_create, dev_control_screen_create, gradient_test_screen_create } },
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// {-1, -1} = Home
static NavPos s_current = { -1, -1 };

// Swipe debounce — 350ms guard prevents rapid-fire navigation from stacking transitions.
static int64_t s_last_nav_us = 0;
static constexpr int64_t NAV_DEBOUNCE_US = 350000;

// Used to track which row was last visited when cycling from Home (up/down).
// On startup, pointing to row 0 so the first home-swipe-down goes to row 1.
static int s_home_row_cursor = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int clamp_col(int row, int col)
{
    int max_col = ROWS[row].col_count - 1;
    if (col < 0)       col = 0;
    if (col > max_col) col = max_col;
    return col;
}

static bool is_home()
{
    return s_current.row == -1;
}

// Determine horizontal animation direction based on column movement.
// Going to a higher col index → move left (new screen slides in from right).
// Going to a lower col index → move right (new screen slides in from left).
static lv_scr_load_anim_t h_anim(int from_col, int to_col)
{
    return (to_col > from_col) ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
}

// Determine vertical animation direction based on row movement.
// Going to a higher row index → move up (new screen slides in from bottom).
// Going to a lower row index → move down (new screen slides in from top).
static lv_scr_load_anim_t v_anim(int from_row, int to_row)
{
    return (to_row > from_row) ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void nav_init()
{
    s_current        = { -1, -1 };
    s_home_row_cursor = 0;
}

NavPos nav_current()
{
    return s_current;
}

void nav_go_home()
{
    // Remember which row we left, so up/down from Home are coherent.
    if (!is_home()) {
        s_home_row_cursor = s_current.row;
    }
    s_current = { -1, -1 };
    home_screen_load();
}

void nav_go(NavPos pos, bool animate)
{
    if (pos.row < 0 || pos.row >= ROW_COUNT) { nav_go_home(); return; }
    pos.col = clamp_col(pos.row, pos.col);

    ScreenFactory factory = ROWS[pos.row].factories[pos.col];
    if (!factory) { nav_go_home(); return; }

    lv_obj_t *scr = factory();
    if (!scr) { nav_go_home(); return; }

    if (!animate) {
        lv_screen_load(scr);
        s_current = pos;
        return;
    }

    // Choose animation direction from current → target.
    lv_scr_load_anim_t anim = LV_SCR_LOAD_ANIM_MOVE_LEFT; // default

    if (is_home()) {
        // Coming from Home: row change drives vertical, col change drives horizontal.
        if (pos.row != 0) {
            anim = v_anim(0, pos.row);  // treat home as "above" row 0
        } else {
            anim = LV_SCR_LOAD_ANIM_MOVE_LEFT; // entering row 0 from home → move left
        }
    } else {
        int fr = s_current.row;
        int fc = s_current.col;
        int tr = pos.row;
        int tc = pos.col;

        if (fr == tr) {
            // Same row → horizontal
            anim = h_anim(fc, tc);
        } else {
            // Different row → vertical
            anim = v_anim(fr, tr);
        }
    }

    s_current = pos;
    lv_screen_load_anim(scr, anim, 200, 0, true);
}

void nav_swipe(int dir_h, int dir_v)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_nav_us < NAV_DEBOUNCE_US) return;
    s_last_nav_us = now;

    if (is_home()) {
        // ── From Home ──────────────────────────────────────────────────────
        if (dir_h == -1) {
            // Swipe left → enter row 0, col 0
            nav_go({ 0, 0 });
        } else if (dir_v == -1) {
            // Swipe up → previous row entry point (wraps: at row 0 stays at 0)
            int target_row = s_home_row_cursor - 1;
            if (target_row < 0) target_row = 0;
            s_home_row_cursor = target_row;
            nav_go({ target_row, 0 });
        } else if (dir_v == 1) {
            // Swipe down → next row entry point
            int target_row = s_home_row_cursor + 1;
            if (target_row >= ROW_COUNT) target_row = ROW_COUNT - 1;
            s_home_row_cursor = target_row;
            nav_go({ target_row, 0 });
        }
        // dir_h == +1 (swipe right from home): no-op
        return;
    }

    int row = s_current.row;
    int col = s_current.col;

    // ── From a grid screen ─────────────────────────────────────────────────
    if (dir_h == -1) {
        // Swipe left → next column. No-op at last col.
        int next_col = col + 1;
        if (next_col < ROWS[row].col_count) {
            nav_go({ row, next_col });
        }
    } else if (dir_h == 1) {
        // Swipe right → previous column. At col 0 → go Home.
        if (col > 0) {
            nav_go({ row, col - 1 });
        } else {
            nav_go_home();
        }
    } else if (dir_v == -1) {
        // Swipe up → previous row. At row 0 → go Home.
        if (row > 0) {
            int new_row = row - 1;
            int new_col = clamp_col(new_row, col);
            nav_go({ new_row, new_col });
        } else {
            nav_go_home();
        }
    } else if (dir_v == 1) {
        // Swipe down → next row. No-op at last row.
        int new_row = row + 1;
        if (new_row < ROW_COUNT) {
            int new_col = clamp_col(new_row, col);
            nav_go({ new_row, new_col });
        }
    }
}
