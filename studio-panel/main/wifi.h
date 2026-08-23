#pragma once

// Starts WiFi station mode. Non-blocking — connection happens asynchronously.
// Calls net_receiver_start() via event loop when IP is assigned.
void wifi_init();

// Thread-safe: returns true if station has a valid IP address.
bool wifi_is_connected();
