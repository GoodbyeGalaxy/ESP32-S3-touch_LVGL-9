#pragma once
#include <cstdint>

void ch422g_init();
void ch422g_set(uint8_t mask);        // setzt gesamten Output (überschreibt alles)
void ch422g_set_bits(uint8_t bits);   // setzt nur angegebene Bits (andere bleiben)
void ch422g_clear_bits(uint8_t bits); // löscht nur angegebene Bits (andere bleiben)
