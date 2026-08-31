#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// Row & Column navigation controller
// ---------------------------------------------------------------------------
//
// Screen grid:
//   Row 0  AUDIO:    [Metering Hub] ↔ [Visuals]
//   Row 1  CONTROL:  [Studio One] ↔ [USB MIDI] ↔ [Routing]
//   Row 2  SYSTEM:   [Settings] ↔ [Dev Control]
//
// Home is a special hub outside the grid. nav_go_home() loads it.
// nav_swipe() is the single entry point called by all screen swipe lambdas.

struct NavPos { int row; int col; };

// Call once from ui_init (or equivalent) before any screen is loaded.
void nav_init();

// Navigate to grid position. animate=true uses slide animation derived from
// the direction of travel (col change → horizontal, row change → vertical).
void nav_go(NavPos pos, bool animate = true);

// Load home screen (always MOVE_RIGHT animation, i.e. slides in from left).
void nav_go_home();

// Current position in the grid. Returns {-1, -1} when Home is active.
NavPos nav_current();

// Process a 2D swipe from any screen. Called from SwipeCallback2D lambdas.
// dir_h: +1=right, -1=left, 0=none. dir_v: +1=down, -1=up, 0=none.
void nav_swipe(int dir_h, int dir_v);
