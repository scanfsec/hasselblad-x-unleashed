#pragma once
#include "mapper.hpp"


class Mapper1 : public Mapper
{
    int writeN;
    u8 tmpReg;
    u8 regs[4];

    void apply();

  public:
    Mapper1(u8* rom) : Mapper(rom)
    {
        regs[0] = 0x0C;
        writeN = tmpReg = regs[1] = regs[2] = regs[3] = 0;
        apply();
    }

    u8 write(u16 addr, u8 v) override;
    u8 chr_write(u16 addr, u8 v) override;

    // Save state support
    u32 get_state_size() const override { return sizeof(regs) + sizeof(writeN) + sizeof(tmpReg); }

    void save_state(u8* buffer) const override {
        memcpy(buffer, regs, sizeof(regs));
        buffer[sizeof(regs)] = writeN;
        buffer[sizeof(regs) + 1] = tmpReg;
    }

    void load_state(const u8* buffer) override {
        memcpy(regs, buffer, sizeof(regs));
        writeN = buffer[sizeof(regs)];
        tmpReg = buffer[sizeof(regs) + 1];
        apply();
    }
};
