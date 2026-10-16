/* x2nes platform shim for the LaiNES core (replaces the SDL-facing gui.hpp).
 * The core calls these; implementations live in main.cpp. */
#pragma once

#include <Blip_Buffer.h> /* blip_sample_t */
#include "common.hpp"

namespace GUI {

void new_frame(u32* pixels);                           /* 256x240, 0x00RRGGBB */
void new_samples(const blip_sample_t* samples, size_t count); /* 96kHz mono s16 */
u8 get_joypad_state(int n);
bool is_fast_forward();

}
