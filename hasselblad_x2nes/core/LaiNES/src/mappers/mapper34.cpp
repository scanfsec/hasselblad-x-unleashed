#include "mappers/mapper34.hpp"

/* Mapper 34: BNROM and NINA-001
 * BNROM: 32KB PRG banking at $8000-$FFFF
 * NINA-001: 32KB PRG + 4KB CHR banking with registers at $7FFD-$7FFF
 */

void Mapper34::apply()
{
    if (is_nina)
    {
        /* NINA-001: 32KB PRG bank, 2x 4KB CHR banks */
        map_prg<32>(0, prg_bank & 0x01);  // NINA-001 has max 64KB PRG (2 banks)
        map_chr<4>(0, chr_bank[0] & 0x0F);  // Lower 4KB CHR bank
        map_chr<4>(1, chr_bank[1] & 0x0F);  // Upper 4KB CHR bank
    }
    else
    {
        /* BNROM: 32KB PRG bank, 8KB CHR RAM */
        map_prg<32>(0, prg_bank & 0x03);  // BNROM typically has 128KB PRG (4 banks)
        map_chr<8>(0, 0);  // CHR RAM
    }
}

u8 Mapper34::write(u16 addr, u8 v)
{
    if (is_nina)
    {
        /* NINA-001 registers at $7FFD-$7FFF */
        switch (addr)
        {
            case 0x7FFD:
                prg_bank = v;
                apply();
                break;
            case 0x7FFE:
                chr_bank[0] = v;
                apply();
                break;
            case 0x7FFF:
                chr_bank[1] = v;
                apply();
                break;
        }
    }
    else if (addr & 0x8000)
    {
        /* BNROM: Bank select at $8000-$FFFF */
        prg_bank = v;
        apply();
    }

    return v;
}

u8 Mapper34::chr_write(u16 addr, u8 v)
{
    if (addr >= 0x2000)
        return Mapper::chr_write(addr, v);

    if (!is_nina)
    {
        /* BNROM uses CHR RAM */
        return chr[addr] = v;
    }
    return v;  // NINA-001 has CHR ROM, no writes
}