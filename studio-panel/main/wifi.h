#pragma once
#include <cstddef>

// Starts WiFi station mode. Non-blocking — connection happens asynchronously.
// Calls net_receiver_start() via event loop when IP is assigned.
void wifi_init();

// Thread-safe: returns true if station has a valid IP address.
bool wifi_is_connected();

// Fills buf with current IP in "A.B.C.D" form, or "--" if not connected.
void wifi_get_ip(char *buf, size_t len);

// Trigger a reconnect (no-op if already connected).
void wifi_reconnect();
