#pragma once
#include "mapper.hpp"


class Mapper10 : public Mapper
{
    u8 prg_bank;
    u8 chr_bank_fd[2];
    u8 chr_bank_fe[2];
    u8 chr_latch[2];
    u8 mirroring;

    void apply();

    public:
    Mapper10(u8* rom) : Mapper(rom)
    {
        prg_bank = 0;
        chr_bank_fd[0] = chr_bank_fd[1] = 0;
        chr_bank_fe[0] = chr_bank_fe[1] = 0;
        chr_latch[0] = chr_latch[1] = 1;  /* Latch states are 0 or 1, start at FE state */
        mirroring = 0;
        apply();
    }

    u8 write(u16 addr, u8 v) override;
    u8 chr_read(u16 addr) override;
    u8 chr_write(u16 addr, u8 v) override;

    // Save state support
    u32 get_state_size() const override {
        return sizeof(prg_bank) + sizeof(chr_bank_fd) + sizeof(chr_bank_fe) +
               sizeof(chr_latch) + sizeof(mirroring);
    }

    void save_state(u8* buffer) const override {
        int offset = 0;
        buffer[offset++] = prg_bank;
        memcpy(buffer + offset, chr_bank_fd, sizeof(chr_bank_fd));
        offset += sizeof(chr_bank_fd);
        memcpy(buffer + offset, chr_bank_fe, sizeof(chr_bank_fe));
        offset += sizeof(chr_bank_fe);
        memcpy(buffer + offset, chr_latch, sizeof(chr_latch));
        offset += sizeof(chr_latch);
        buffer[offset++] = mirroring;
    }

    void load_state(const u8* buffer) override {
        int offset = 0;
        prg_bank = buffer[offset++];
        memcpy(chr_bank_fd, buffer + offset, sizeof(chr_bank_fd));
        offset += sizeof(chr_bank_fd);
        memcpy(chr_bank_fe, buffer + offset, sizeof(chr_bank_fe));
        offset += sizeof(chr_bank_fe);
        memcpy(chr_latch, buffer + offset, sizeof(chr_latch));
        offset += sizeof(chr_latch);
        mirroring = buffer[offset++];
        apply();
    }
};