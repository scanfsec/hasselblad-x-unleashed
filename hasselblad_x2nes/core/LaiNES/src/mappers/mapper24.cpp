#include "cpu.hpp"
#include "apu.hpp"
#include "ppu.hpp"
#include "mappers/mapper24.hpp"


Mapper24::Mapper24(u8* rom, bool vrc6b) : Mapper(rom), is_vrc6b(vrc6b)
{
    // Initialize banking
    prg_bank_16k = 0;
    prg_bank_8k = 0;
    for (int i = 0; i < 8; i++)
        chr_banks[i] = i;

    // Initialize IRQ state
    irq_latch = 0;
    irq_counter = 0;
    irq_enabled = false;
    irq_mode = false;
    irq_enable_after_ack = false;
    irq_active = false;
    last_cpu_cycle = 0;
    irq_prescaler = 341;  // Start prescaler at 341 (simulates 341 PPU dots = ~113.67 CPU cycles)

    // Set up initial PRG banking
    // $8000-$BFFF: 16K switchable
    // $C000-$DFFF: 8K switchable
    // $E000-$FFFF: Fixed to last 8K bank
    apply_prg_banks();
    apply_chr_banks();

    // Initialize VRC6 expansion audio
    vrc6.output(&APU::get_buffer());
    vrc6.volume(0.5);  // Reduce volume to prevent clipping when mixed with APU
    vrc6.reset();
}

u16 Mapper24::decode_addr(u16 addr)
{
    // VRC6a (mapper 24): A0, A1 connected normally
    // VRC6b (mapper 26): A0 and A1 lines are swapped
    if (!is_vrc6b)
        return addr;

    // Swap A0 and A1
    u16 base = addr & 0xFFFC;  // Clear A0 and A1
    u16 a0 = (addr & 0x01) << 1;  // A0 -> A1
    u16 a1 = (addr & 0x02) >> 1;  // A1 -> A0
    return base | a0 | a1;
}

void Mapper24::apply_prg_banks()
{
    // $8000-$BFFF: 16K switchable bank
    map_prg<16>(0, prg_bank_16k);

    // $C000-$DFFF: 8K switchable bank
    map_prg<8>(2, prg_bank_8k);

    // $E000-$FFFF: Fixed to last 8K bank
    map_prg<8>(3, -1);
}

void Mapper24::apply_chr_banks()
{
    // Eight 1K banks
    for (int i = 0; i < 8; i++)
        map_chr<1>(i, chr_banks[i]);
}

void Mapper24::clock_irq()
{
    // VRC IRQ logic: Check counter BEFORE incrementing
    // "If IRQ counter is $FF, reload with latch and trip IRQ, otherwise increment by 1"
    if (irq_counter == 0xFF)
    {
        irq_counter = irq_latch;
        irq_active = true;
        CPU::set_irq(true);
    }
    else
    {
        irq_counter++;
    }
}

u8 Mapper24::read(u16 addr)
{
    // Use base class read implementation
    return Mapper::read(addr);
}

u8 Mapper24::chr_read(u16 addr)
{
    // VRC IRQ is CPU-cycle based, don't clock it on PPU CHR reads
    // Use base class chr_read implementation
    return Mapper::chr_read(addr);
}

u8 Mapper24::write(u16 addr, u8 v)
{
    // Get current CPU cycle time for audio
    cpu_time_t elapsed = CPU::elapsed();

    if (addr < 0x8000)
    {
        // PRG RAM at $6000-$7FFF
        prgRam[addr - 0x6000] = v;
        return v;
    }

    // Decode address for VRC6a/b differences
    u16 decoded = decode_addr(addr);
    u16 reg = decoded & 0xF003;

    switch (reg)
    {
        // PRG Banking
        case 0x8000 ... 0x8003:
            prg_bank_16k = v & 0x0F;
            apply_prg_banks();
            break;

        // Audio - Pulse 1
        case 0x9000:  // Volume/Duty
            vrc6.write_osc(elapsed, 0, 0, v);
            break;
        case 0x9001:  // Period low
            vrc6.write_osc(elapsed, 0, 1, v);
            break;
        case 0x9002:  // Period high
            vrc6.write_osc(elapsed, 0, 2, v);
            break;

        // Audio - Pulse 2
        case 0xA000:  // Volume/Duty
            vrc6.write_osc(elapsed, 1, 0, v);
            break;
        case 0xA001:  // Period low
            vrc6.write_osc(elapsed, 1, 1, v);
            break;
        case 0xA002:  // Period high
            vrc6.write_osc(elapsed, 1, 2, v);
            break;

        // Audio - Sawtooth
        case 0xB000:  // Accumulator rate
            vrc6.write_osc(elapsed, 2, 0, v);
            break;
        case 0xB001:  // Period low
            vrc6.write_osc(elapsed, 2, 1, v);
            break;
        case 0xB002:  // Period high
            vrc6.write_osc(elapsed, 2, 2, v);
            break;

        // PPU Banking mode / Mirroring
        case 0xB003:
            // Bits 2-3: Mirroring mode
            switch ((v >> 2) & 0x03)
            {
                case 0:  set_mirroring(PPU::VERTICAL);      break;
                case 1:  set_mirroring(PPU::HORIZONTAL);    break;
                case 2:  set_mirroring(PPU::ONE_SCREEN_LO); break;
                case 3:  set_mirroring(PPU::ONE_SCREEN_HI); break;
            }
            // Bits 0-1: PPU banking mode (not implemented for now)
            break;

        // PRG Banking (8K)
        case 0xC000 ... 0xC003:
            prg_bank_8k = v & 0x1F;
            apply_prg_banks();
            break;

        // CHR Banking
        case 0xD000:  chr_banks[0] = v; apply_chr_banks(); break;
        case 0xD001:  chr_banks[1] = v; apply_chr_banks(); break;
        case 0xD002:  chr_banks[2] = v; apply_chr_banks(); break;
        case 0xD003:  chr_banks[3] = v; apply_chr_banks(); break;
        case 0xE000:  chr_banks[4] = v; apply_chr_banks(); break;
        case 0xE001:  chr_banks[5] = v; apply_chr_banks(); break;
        case 0xE002:  chr_banks[6] = v; apply_chr_banks(); break;
        case 0xE003:  chr_banks[7] = v; apply_chr_banks(); break;

        // IRQ Latch
        case 0xF000:
            irq_latch = v;
            break;

        // IRQ Control
        case 0xF001:
            irq_enable_after_ack = v & 0x01;
            irq_enabled = v & 0x02;
            irq_mode = v & 0x04;
            if (irq_enabled)
            {
                irq_counter = irq_latch;
                irq_prescaler = 341;  // Reset prescaler when IRQ enabled
            }
            irq_active = false;
            CPU::set_irq(false);
            break;

        // IRQ Acknowledge
        case 0xF002:
            irq_active = false;
            CPU::set_irq(false);
            irq_enabled = irq_enable_after_ack;
            break;
    }

    return v;
}

void Mapper24::signal_scanline(int scanline)
{
    // VRC6 IRQ is CPU-cycle based in both modes, not PPU scanline-based
    // Scanline mode uses a prescaler that's processed in check_irq()
    // This function is not used for VRC6 IRQ timing
}

void Mapper24::run_audio(int elapsed)
{
    // VRC6 audio is generated via write_osc() calls during the frame,
    // and end_frame() will catch up any remaining cycles
    // IRQ processing is handled in check_irq()
}

void Mapper24::end_audio_frame(int elapsed)
{
    // Finalize VRC6 audio
    vrc6.end_frame(elapsed);

    // Reset cycle counter for next frame
    last_cpu_cycle = 0;
}

bool Mapper24::check_irq(int elapsed)
{
    if (elapsed <= last_cpu_cycle)
    {
        return irq_active;
    }

    int cycles_to_process = elapsed - last_cpu_cycle;
    last_cpu_cycle = elapsed;

    if (!irq_enabled)
    {
        return irq_active;
    }

    if (irq_mode)
    {
        // Cycle mode: clock IRQ counter every CPU cycle
        for (int i = 0; i < cycles_to_process; i++)
        {
            clock_irq();
        }
    }
    else
    {
        // Scanline mode: use prescaler to divide CPU cycles by ~113.67
        // Pattern: start at 341, subtract 3 per CPU cycle, when ≤0 add 341 and clock
        for (int i = 0; i < cycles_to_process; i++)
        {
            irq_prescaler -= 3;
            if (irq_prescaler <= 0)
            {
                irq_prescaler += 341;
                clock_irq();
            }
        }
    }

    // Return whether IRQ line is currently asserted
    return irq_active;
}
