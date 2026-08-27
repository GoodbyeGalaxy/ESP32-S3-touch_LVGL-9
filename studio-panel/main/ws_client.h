#pragma once

// WebSocket client for Studio One DAW integration (Phase 6).
//
// Connects to ws://<CONFIG_WS_HOST>:<CONFIG_WS_PORT>/studio-one and receives
// newline-delimited JSON messages from the Mac-side helper:
//
//   {"type":"transport","bpm":120.5,"timesig":"4/4","pos":"001.01.000","state":"playing"}
//
// states: "playing" | "stopped" | "recording" | "paused"
//
// Received data is published to g_studio_one_queue via xQueueOverwrite so the
// studio_one screen always reads the most recent snapshot.

// Starts the WebSocket client task. Idempotent — safe to call on WiFi reconnect.
// IN: nothing. OUT: nothing (background task; data written to g_studio_one_queue).
void ws_client_start();

// Thread-safe: returns true if the WebSocket is currently connected.
// IN: nothing. OUT: bool.
bool ws_client_connected();
