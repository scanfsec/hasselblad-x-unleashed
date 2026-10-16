#include "ppu.hpp"
#include "mappers/mapper11.hpp"

/* Based off of https://wiki.nesdev.com/w/index.php/Color_Dreams */

/* Apply the registers state */
void Mapper11::apply()
{
    /* 32KB PRG banking */
    map_prg<32>(0, regs[0] & 0x03);

    /* 8KB CHR banking */
    map_chr<8>(0, (regs[0] >> 4) & 0x0F);

    /* mirroring is based on the header (soldered) */
    set_mirroring(vertical_mirroring ? PPU::VERTICAL : PPU::HORIZONTAL);
}

u8 Mapper11::write(u16 addr, u8 v)
{
    /* Bank switching at $8000-$FFFF */
    if (addr & 0x8000)
    {
        /* Bus conflicts - AND written value with value read from ROM */
        u8 rom_value = read(addr);
        regs[0] = v & rom_value;
        apply();
    }
    return v;
}

u8 Mapper11::chr_write(u16 addr, u8 v)
{
    if (addr >= 0x2000)
        return Mapper::chr_write(addr, v);
    return chr[addr] = v;
}