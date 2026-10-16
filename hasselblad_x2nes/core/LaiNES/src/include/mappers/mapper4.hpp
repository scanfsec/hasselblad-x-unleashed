#pragma once
#include "mapper.hpp"


class Mapper4 : public Mapper
{
    u8 reg8000;
    u8 regs[8];
    bool horizMirroring;

    u8 irqPeriod;
    u8 irqCounter;
    bool irqEnabled;

    void apply();

  public:
    Mapper4(u8* rom) : Mapper(rom)
    {
        for (int i = 0; i < 8; i++)
            regs[i] = 0;

        horizMirroring = true;
        irqEnabled = false;
        irqPeriod = irqCounter = 0;

        map_prg<8>(3, -1);
        apply();
    }

    u8 write(u16 addr, u8 v) override;
    u8 chr_write(u16 addr, u8 v) override;

    void signal_scanline(int scanline) override;

    // Save state support
    u32 get_state_size() const override {
        return sizeof(reg8000) + sizeof(regs) + sizeof(horizMirroring) +
               sizeof(irqPeriod) + sizeof(irqCounter) + sizeof(irqEnabled);
    }

    void save_state(u8* buffer) const override {
        int offset = 0;
        buffer[offset++] = reg8000;
        memcpy(buffer + offset, regs, sizeof(regs));
        offset += sizeof(regs);
        buffer[offset++] = horizMirroring ? 1 : 0;
        buffer[offset++] = irqPeriod;
        buffer[offset++] = irqCounter;
        buffer[offset++] = irqEnabled ? 1 : 0;
    }

    void load_state(const u8* buffer) override {
        int offset = 0;
        reg8000 = buffer[offset++];
        memcpy(regs, buffer + offset, sizeof(regs));
        offset += sizeof(regs);
        horizMirroring = buffer[offset++] != 0;
        irqPeriod = buffer[offset++];
        irqCounter = buffer[offset++];
        irqEnabled = buffer[offset++] != 0;
        apply();
    }
};
