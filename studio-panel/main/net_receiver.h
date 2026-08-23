#pragma once

// Starts the UDP receiver task on port 4210. Idempotent — safe to call
// multiple times (e.g. on WiFi reconnect); only one task runs at a time.
void net_receiver_start();
