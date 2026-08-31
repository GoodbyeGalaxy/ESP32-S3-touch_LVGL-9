#pragma once

// IN: nothing. OUT: initialises head bar on lv_layer_top().
void statusbar_init();

// IN: screen name string (e.g. "METERING"). OUT: updates left label in head bar.
// Call from each screen's create function before lv_screen_load.
void statusbar_set_screen_name(const char *name);

// IN: connected state + optional IP string. OUT: WiFi symbol green/gray; IP shown in center.
void statusbar_update_wifi(bool connected, const char *ip_str = nullptr);

// IN: time string e.g. "21:43". OUT: updates time label (shown once SNTP syncs).
void statusbar_update_time(const char *time_str);

