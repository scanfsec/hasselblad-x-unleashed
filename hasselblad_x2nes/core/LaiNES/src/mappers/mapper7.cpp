#include "ppu.hpp"
#include "mappers/mapper7.hpp"

/* Based off of https://wiki.nesdev.com/w/index.php/AxROM */

/* Apply the registers state */
void Mapper7::apply()
{
   /*
    * 32 kb PRG ROM Banks
    * 0x8000 - 0xFFFF swappable
    * Register format: xxxSPPPP
    * PPPP = PRG bank (bits 0-3: up to 16 banks / 512KB)
    * S = Screen select (bit 4: 0 = $2000, 1 = $2400)
    */
    map_prg<32>(0, regs[0] & 0x0F);  // 4 bits for PRG bank

    /* 8k of CHR RAM - AxROM has no CHR ROM, only CHR RAM */
    map_chr<8>(0, 0);

    /* Single-screen mirroring based on bit 4
     * Bit 4: 0 = Mirror PPU $2000 (ONE_SCREEN_LO)
     *        1 = Mirror PPU $2400 (ONE_SCREEN_HI)
     */
    set_mirroring((regs[0] & 0x10) ? PPU::ONE_SCREEN_HI : PPU::ONE_SCREEN_LO);
}

u8 Mapper7::write(u16 addr, u8 v)
{
    /* check for bus contingency? (addr & 0x8000 == v?)
     * Seems not neccesary */

    /* bank switching */
    if (addr & 0x8000)
    {
        regs[0] = v;
        apply();
    }
    return v;
}

u8 Mapper7::chr_write(u16 addr, u8 v)
{
    // Only write to CHR RAM for pattern table addresses
    if (addr < 0x2000)
        return chr[addr] = v;

    // For nametable addresses, delegate to base class
    return Mapper::chr_write(addr, v);
}
