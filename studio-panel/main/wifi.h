#pragma once
#include <cstddef>

// Starts WiFi station mode. Non-blocking — connection happens asynchronously.
// Calls net_receiver_start() via event loop when IP is assigned.
void wifi_init();

// Thread-safe: returns true if station has a valid IP address.
bool wifi_is_connected();

// Fills buf with current IP in "A.B.C.D" form, or "--" if not connected.
void wifi_get_ip(char *buf, size_t len);

// Returns the last WIFI_EVENT_STA_DISCONNECTED reason code, 0 if never disconnected.
int wifi_get_disconnect_reason();

// Trigger a reconnect (no-op if already connected).
void wifi_reconnect();

// Toggle WiFi on/off at runtime (calls esp_wifi_stop / esp_wifi_start).
void wifi_toggle();
// Returns true if WiFi is currently started (regardless of connection state).
bool wifi_is_enabled();
// Returns RSSI in dBm for current AP, or 0 if not associated.
int  wifi_get_rssi();
