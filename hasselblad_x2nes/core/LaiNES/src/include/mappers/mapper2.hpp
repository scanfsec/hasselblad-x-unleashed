#pragma once
#include "mapper.hpp"


class Mapper2 : public Mapper
{
    u8 regs[1];
    bool vertical_mirroring;

    void apply();

  public:
    Mapper2(u8* rom) : Mapper(rom)
    {
        regs[0] = 0;
        vertical_mirroring = rom[6] & 0x01;
        apply();
    }

    u8 write(u16 addr, u8 v) override;
    u8 chr_write(u16 addr, u8 v) override;

    // Save state support
    u32 get_state_size() const override { return sizeof(regs); }

    void save_state(u8* buffer) const override {
        memcpy(buffer, regs, sizeof(regs));
    }

    void load_state(const u8* buffer) override {
        memcpy(regs, buffer, sizeof(regs));
        apply();
    }
};
