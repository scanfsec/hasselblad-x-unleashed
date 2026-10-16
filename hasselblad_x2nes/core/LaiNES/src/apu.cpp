#include "gui.hpp"
#include "cpu.hpp"
#include "apu.hpp"

namespace APU {


Nes_Apu apu;
Blip_Buffer buf;

const int OUT_SIZE = 4096;
blip_sample_t outBuf[OUT_SIZE];

// IRQ callback - called when APU IRQ state changes
// This is called whenever earliest_irq_ changes in the APU
void apu_irq_changed(void*)
{
    // Callback exists but we don't use it - we check IRQ state during polling instead
    // This is because IRQ state depends on current time, which changes between callbacks
}

void init()
{
    buf.sample_rate(96000);
    buf.clock_rate(1789773);

    apu.output(&buf);
    apu.dmc_reader(CPU::dmc_read);
    apu.irq_notifier(apu_irq_changed);
}

void reset()
{
    apu.reset();
    buf.clear();
}

template <bool write> u8 access(int elapsed, u16 addr, u8 v, bool is_put_cycle)
{
    if (write) {
        apu.write_register(elapsed, addr, v, is_put_cycle);
    } else if (addr == 0x4015) {
        // Status register
        u8 status = apu.read_status(elapsed, is_put_cycle);
        // Bit 5 is open bus, preserve it from input value
        v = (status & 0xDF) | (v & 0x20);
    }

    return v;
}
template u8 access<0>(int, u16, u8, bool); template u8 access<1>(int, u16, u8, bool);

// Track last time we ran APU for IRQ checking
static int last_irq_check_time = -1;

void run_frame(int elapsed)
{
    apu.end_frame(elapsed);
    // Note: buf.end_frame() is NOT called here - it's called from CPU after mapper audio

    // Reset IRQ check tracker at frame end
    last_irq_check_time = -1;

    // Note: Samples are NOT read here - they're read in end_buffer_frame()
    // after the buffer has been finalized
}

// Finalize the buffer after all audio sources (APU + mapper) have written to it
void end_buffer_frame(int elapsed)
{
    buf.end_frame(elapsed);

    // Now that the buffer is finalized, read the samples
    if (buf.samples_avail() >= OUT_SIZE)
        GUI::new_samples(outBuf, buf.read_samples(outBuf, OUT_SIZE));

    // During fast forward, clear the buffer to prevent buildup
    if (GUI::is_fast_forward() && buf.samples_avail() > 0)
        buf.clear();
}

// Check if APU IRQ should be active at the given time
bool check_irq(int elapsed)
{
    // Only run APU if time has advanced since last check
    // This prevents running APU multiple times per CPU cycle
    if (elapsed != last_irq_check_time) {
        apu.run_until(elapsed);
        last_irq_check_time = elapsed;
    }

    cpu_time_t irq_time = apu.earliest_irq();

    // IRQ is active if:
    // - irq_waiting (0): IRQ should fire immediately
    // - Any value <= current time: IRQ time has passed
    return (irq_time == Nes_Apu::irq_waiting) ||
           (irq_time != Nes_Apu::no_irq && irq_time <= elapsed);
}

// Get audio buffer for mapper expansion audio
Blip_Buffer& get_buffer()
{
    return buf;
}

Nes_Apu& get_apu() {
    return apu;
}


}

