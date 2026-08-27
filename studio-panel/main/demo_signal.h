#pragma once

// ── Demo Signal Generator ────────────────────────────────────────────────────
// Synthesises AudioPacket data and writes into g_audio_queue when no real
// UDP signal is present (auto-mode) or when explicitly forced by the user.
//
// Call demo_signal_init() once from app_main after g_audio_queue is created.
// Call demo_signal_notify_packet() from net_receiver.cpp on every valid UDP rx.

// IN: nothing. OUT: nothing. Creates FreeRTOS task (Core 0, priority 3).
// Safe to call once; subsequent calls are no-ops.
void demo_signal_init();

// IN: forced=true → always generate demo regardless of real signal.
//     forced=false → auto-mode (only when no real packet for >2s).
// OUT: nothing. Thread-safe (atomic write).
void demo_signal_set_forced(bool forced);

// IN: nothing. OUT: nothing. Call each time a valid UDP packet is received.
// Resets the 2-second silence counter. Thread-safe (atomic timestamp write).
void demo_signal_notify_packet();

// IN: nothing. OUT: true if demo signal is currently active.
bool demo_signal_is_active();
