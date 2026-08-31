#pragma once
#include "lvgl.h"

// Chromatic bass tuner — HPS pitch detection from 256 FFT bins.
// Displays note name + octave + cents needle bar + Hz readout.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_tuner_screen_create();
