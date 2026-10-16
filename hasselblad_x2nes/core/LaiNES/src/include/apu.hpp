#pragma once
#include "common.hpp"
#include <Blip_Buffer.h>
#include <Nes_Apu.h>

namespace APU {


template <bool write> u8 access(int elapsed, u16 addr, u8 v = 0, bool is_put_cycle = false);
void run_frame(int elapsed);
void end_buffer_frame(int elapsed);  // Finalize buffer after all audio sources
void reset();
void init();
bool check_irq(int elapsed);  // Check if APU IRQ should be active at given time
Blip_Buffer& get_buffer();     // Get audio buffer for mapper expansion audio

// Save state support - get APU instance
Nes_Apu& get_apu();


}
