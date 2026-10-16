#pragma once
#include "mapper.hpp"
#include <Nes_Vrc6.h>


class Mapper24 : public Mapper
{
    // VRC6 expansion audio
    Nes_Vrc6 vrc6;

    // PRG banking
    u8 prg_bank_16k;  // 16K bank at $8000-$BFFF
    u8 prg_bank_8k;   // 8K bank at $C000-$DFFF

    // CHR banking
    u8 chr_banks[8];  // Eight 1K banks at $0000-$1FFF

    // IRQ state
    u8 irq_latch;
    u8 irq_counter;
    bool irq_enabled;
    bool irq_mode;           // false = scanline mode, true = cycle mode
    bool irq_enable_after_ack;
    bool irq_active;         // Track whether IRQ line is currently asserted
    int last_cpu_cycle;      // Track last CPU cycle for both modes
    int irq_prescaler;       // Scanline mode prescaler (341 PPU dots = ~113.67 CPU cycles)

    // VRC6a vs VRC6b (address line differences)
    bool is_vrc6b;

    void apply_prg_banks();
    void apply_chr_banks();
    void clock_irq();           // Clock the IRQ counter (increments/checks counter)

    // Decode register address accounting for VRC6a/b differences
    u16 decode_addr(u16 addr);

  public:
    Mapper24(u8* rom, bool vrc6b = false);

    u8 read(u16 addr) override;
    u8 write(u16 addr, u8 v) override;
    u8 chr_read(u16 addr) override;
    void signal_scanline(int scanline) override;

    // Expansion audio support
    bool has_audio() override { return true; }
    void run_audio(int elapsed) override;
    void end_audio_frame(int elapsed) override;

    // IRQ support
    bool check_irq(int elapsed) override;

    // Save state support
    u32 get_state_size() const override {
        return sizeof(vrc6_snapshot_t) + sizeof(prg_bank_16k) + sizeof(prg_bank_8k) +
               sizeof(chr_banks) + sizeof(irq_latch) + sizeof(irq_counter) +
               sizeof(irq_enabled) + sizeof(irq_mode) + sizeof(irq_enable_after_ack) +
               sizeof(irq_active) + sizeof(last_cpu_cycle) + sizeof(irq_prescaler);
    }

    void save_state(u8* buffer) const override {
        int offset = 0;

        // Save VRC6 audio state
        vrc6_snapshot_t vrc6_state;
        vrc6.save_snapshot(&vrc6_state);
        memcpy(buffer + offset, &vrc6_state, sizeof(vrc6_state));
        offset += sizeof(vrc6_state);

        // Save banking state
        buffer[offset++] = prg_bank_16k;
        buffer[offset++] = prg_bank_8k;
        memcpy(buffer + offset, chr_banks, sizeof(chr_banks));
        offset += sizeof(chr_banks);

        // Save IRQ state
        buffer[offset++] = irq_latch;
        buffer[offset++] = irq_counter;
        buffer[offset++] = irq_enabled ? 1 : 0;
        buffer[offset++] = irq_mode ? 1 : 0;
        buffer[offset++] = irq_enable_after_ack ? 1 : 0;
        buffer[offset++] = irq_active ? 1 : 0;
        memcpy(buffer + offset, &last_cpu_cycle, sizeof(last_cpu_cycle));
        offset += sizeof(last_cpu_cycle);
        memcpy(buffer + offset, &irq_prescaler, sizeof(irq_prescaler));
    }

    void load_state(const u8* buffer) override {
        int offset = 0;

        // Load VRC6 audio state
        vrc6_snapshot_t vrc6_state;
        memcpy(&vrc6_state, buffer + offset, sizeof(vrc6_state));
        vrc6.load_snapshot(vrc6_state);
        offset += sizeof(vrc6_state);

        // Load banking state
        prg_bank_16k = buffer[offset++];
        prg_bank_8k = buffer[offset++];
        memcpy(chr_banks, buffer + offset, sizeof(chr_banks));
        offset += sizeof(chr_banks);

        // Load IRQ state
        irq_latch = buffer[offset++];
        irq_counter = buffer[offset++];
        irq_enabled = buffer[offset++] != 0;
        irq_mode = buffer[offset++] != 0;
        irq_enable_after_ack = buffer[offset++] != 0;
        irq_active = buffer[offset++] != 0;
        memcpy(&last_cpu_cycle, buffer + offset, sizeof(last_cpu_cycle));
        offset += sizeof(last_cpu_cycle);
        memcpy(&irq_prescaler, buffer + offset, sizeof(irq_prescaler));

        apply_prg_banks();
        apply_chr_banks();
    }
};
