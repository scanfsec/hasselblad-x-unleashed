#include "ppu.hpp"
#include "mappers/mapper9.hpp"

/* MMC2 (PxROM) - Mapper 9 */
/* Used by Mike Tyson's Punch-Out!! */

void Mapper9::apply()
{
    /* 8KB PRG ROM banking at $8000-$9FFF */
    map_prg<8>(0, prg_bank);

    /* Fixed last 24KB at $A000-$FFFF */
    map_prg<8>(1, -3);
    map_prg<8>(2, -2);
    map_prg<8>(3, -1);

    /* CHR ROM banking based on latches (0 = FD, 1 = FE) */
    u8 chr_bank_0 = (chr_latch[0] == 0) ? chr_bank_fd[0] : chr_bank_fe[0];
    u8 chr_bank_1 = (chr_latch[1] == 0) ? chr_bank_fd[1] : chr_bank_fe[1];

    map_chr<4>(0, chr_bank_0);
    map_chr<4>(1, chr_bank_1);

    /* Mirroring */
    set_mirroring(mirroring ? PPU::HORIZONTAL : PPU::VERTICAL);
}

u8 Mapper9::write(u16 addr, u8 v)
{
    switch (addr & 0xF000)
    {
        case 0xA000:  /* PRG ROM bank select */
            prg_bank = v & 0x0F;
            apply();
            break;

        case 0xB000:  /* CHR ROM $FD bank for $0000-$0FFF */
            chr_bank_fd[0] = v & 0x1F;
            apply();
            break;

        case 0xC000:  /* CHR ROM $FE bank for $0000-$0FFF */
            chr_bank_fe[0] = v & 0x1F;
            apply();
            break;

        case 0xD000:  /* CHR ROM $FD bank for $1000-$1FFF */
            chr_bank_fd[1] = v & 0x1F;
            apply();
            break;

        case 0xE000:  /* CHR ROM $FE bank for $1000-$1FFF */
            chr_bank_fe[1] = v & 0x1F;
            apply();
            break;

        case 0xF000:  /* Mirroring */
            mirroring = v & 0x01;
            apply();
            break;
    }
    return v;
}

u8 Mapper9::chr_read(u16 addr)
{
    // For nametables, delegate to base class
    if (addr >= 0x2000)
        return Mapper::chr_read(addr);

    u8 value = chr[chrMap[addr / 0x400] + (addr % 0x400)];

    /* Check for latch triggers AFTER the read */
    /* Left pattern table latch (only specific addresses) */
    if (addr == 0x0FD8)
    {
        chr_latch[0] = 0;  /* Set latch to FD state */
        apply();
    }
    else if (addr == 0x0FE8)
    {
        chr_latch[0] = 1;  /* Set latch to FE state */
        apply();
    }
    /* Right pattern table latch (address ranges) */
    else if (addr >= 0x1FD8 && addr <= 0x1FDF)
    {
        chr_latch[1] = 0;  /* Set latch to FD state */
        apply();
    }
    else if (addr >= 0x1FE8 && addr <= 0x1FEF)
    {
        chr_latch[1] = 1;  /* Set latch to FE state */
        apply();
    }

    return value;
}

u8 Mapper9::chr_write(u16 addr, u8 v)
{
    if (addr >= 0x2000)
        return Mapper::chr_write(addr, v);
    return chr[addr] = v;
}