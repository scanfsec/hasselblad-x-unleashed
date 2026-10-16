#include "gui.hpp"

namespace Joypad {


u8 joypad_bits[2] = {0, 0};  // Joypad shift registers (initialized to 0).
bool strobe = false;         // Joypad strobe latch (initialized to false).
bool strobe_was_set_by_put = false; // Track if strobe bit was set to 1 from 0 during a put cycle

/* Read joypad state (NES register format) */
u8 read_state(int n)
{
    // When strobe is high, it keeps reading A:
    if (strobe)
        return (GUI::get_joypad_state(n) & 1);

    // Get the status of a button and shift the register:
    u8 j = (joypad_bits[n] & 1);
    joypad_bits[n] = 0x80 | (joypad_bits[n] >> 1);
    return j;
}

void write_strobe(bool v, int cycle, bool is_put_cycle)
{
    // Controller strobing behavior:
    // Strobe occurs on 1->0 transition, but only if:
    // 1. The bit was set to 1 from 0 during a put cycle (0->1 transition)
    // 2. The bit is now being cleared to 0 during a put cycle (1->0 transition)
    // This prevents DEC $4016 from strobing because the initial 1 wasn't
    // set by a proper 0->1 transition during a put cycle

    bool old_strobe = strobe;

    // Check for 1->0 transition
    if (old_strobe && !v && is_put_cycle) {
        // Only strobe if the bit was originally set from 0->1 during a put cycle
        if (strobe_was_set_by_put) {
            for (int i = 0; i < 2; i++)
                joypad_bits[i] = GUI::get_joypad_state(i);
        }
    }

    // Track if strobe was properly set via a 0->1 transition during a put cycle
    if (!old_strobe && v) {
        // 0->1 transition: only valid if it's a put cycle
        strobe_was_set_by_put = is_put_cycle;
    } else if (!v) {
        // When clearing to 0, reset the flag
        strobe_was_set_by_put = false;
    }
    // If v is 1 and old_strobe is 1 (maintaining 1), keep strobe_was_set_by_put unchanged

    // Update strobe state
    strobe = v;
}


}
