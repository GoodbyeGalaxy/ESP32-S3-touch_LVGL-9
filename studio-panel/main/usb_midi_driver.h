#pragma once
#include <cstdint>

// Initialise TinyUSB as a USB MIDI 1.0 Class device.
// Must be called once during boot, before ui_init().
// After this call the ESP32-S3 enumerates as a MIDI device on the host.
void usb_midi_driver_init();

// Send a MIDI Control Change (CC) message.
//   channel : 0-based MIDI channel (0 = channel 1, ..., 15 = channel 16)
//   cc      : CC number  (0–127)
//   value   : CC value   (0–127)
// Safe to call from any task; internally thread-safe via the TinyUSB FIFO.
// No-op if USB is not yet connected / enumerated.
void usb_midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value);
