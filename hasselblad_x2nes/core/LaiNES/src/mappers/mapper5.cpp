#include <cstdio>
#include "cpu.hpp"
#include "ppu.hpp"
#include "mappers/mapper5.hpp"


void Mapper5::apply_prg_banking()
{
    switch (prgMode)
    {
        case 0:  // 32KB mode
            // $5117 selects 32KB bank (ignore bits 0-1)
            // All 4 slots use same RAM/ROM type
            {
                u8 bank = prgBanks[4];
                bool isRam = !(bank & 0x80);
                for (int i = 0; i < 4; i++)
                    prgIsRam[i] = isRam;
                if (!isRam)
                    // For 32KB banks, ignore bits 0-1 of bank number
                    map_prg<32>(0, (bank & 0x7C) >> 2);
            }
            break;

        case 1:  // 16KB + 16KB mode
            // $5115 selects first 16KB, $5117 selects second 16KB
            {
                u8 bank0 = prgBanks[2];
                u8 bank1 = prgBanks[4];
                prgIsRam[0] = prgIsRam[1] = !(bank0 & 0x80);
                prgIsRam[2] = prgIsRam[3] = !(bank1 & 0x80);
                // For 16KB banks, ignore LSB (bit 0) of bank number
                if (bank0 & 0x80) map_prg<16>(0, (bank0 & 0x7E) >> 1);
                if (bank1 & 0x80) map_prg<16>(1, (bank1 & 0x7E) >> 1);
            }
            break;

        case 2:  // 16KB + 8KB + 8KB mode
            {
                u8 bank0 = prgBanks[2];  // $5115: Controls $8000-$BFFF (16KB)
                u8 bank2 = prgBanks[3];  // $5116: Controls $C000-$DFFF (8KB)
                u8 bank3 = prgBanks[4];  // $5117: Controls $E000-$FFFF (8KB)
                prgIsRam[0] = prgIsRam[1] = !(bank0 & 0x80);
                prgIsRam[2] = !(bank2 & 0x80);
                prgIsRam[3] = !(bank3 & 0x80);
                if (bank0 & 0x80) {
                    // For 16KB banks, ignore LSB (bit 0) of bank number
                    map_prg<16>(0, (bank0 & 0x7E) >> 1);
                }
                if (bank2 & 0x80) {
                    map_prg<8>(2, bank2 & 0x7F);
                }
                if (bank3 & 0x80) {
                    map_prg<8>(3, bank3 & 0x7F);
                }
            }
            break;

        case 3:  // Four 8KB banks
            // $5114-$5117 control $8000-$FFFF in 8KB chunks
            for (int i = 0; i < 4; i++)
            {
                u8 bank = prgBanks[i + 1];
                prgIsRam[i] = !(bank & 0x80);
                if (bank & 0x80) {
                    map_prg<8>(i, bank & 0x7F);
                }
            }
            break;
    }
}

void Mapper5::apply_chr_banking()
{
    // CHR banking depends on mode AND sprite size:
    // - 8x8 sprites: $5120-$5127 used for BOTH sprites AND backgrounds, $5128-$512B ignored
    // - 8x16 sprites: $5120-$5127 for sprites, $5128-$512B for backgrounds
    //
    // CRITICAL: Bank register values represent different chunk sizes per mode:
    // - 8KB mode: bank value selects 8KB chunks (multiply by 0x2000)
    // - 4KB mode: bank value selects 4KB chunks (multiply by 0x1000)
    // - 2KB mode: bank value selects 2KB chunks (multiply by 0x800)
    // - 1KB mode: bank value selects 1KB chunks (multiply by 0x400)

    // Read current sprite size from PPU (like Mesen does)
    bool sprite8x16 = PPU::ctrl.sprSz;

    switch (chrMode)
    {
        case 0:  // 8KB mode - use last register written
            // Sprites: use $5127 (chrBanks[7]) for entire 8KB
            for (int i = 0; i < 8; i++) {
                chrMapSprite[i] = (0x2000 * chrBanks[7] + 0x400 * i) % chrSize;
            }
            // Backgrounds: use $512B (chrBanks[11]) for 8x16 mode, $5127 (chrBanks[7]) for 8x8 mode
            for (int i = 0; i < 8; i++) {
                u8 bgBank = sprite8x16 ? chrBanks[11] : chrBanks[7];
                chrMapBG[i] = (0x2000 * bgBank + 0x400 * i) % chrSize;
            }
            break;

        case 1:  // 4KB banks
            // Sprites: $5123 and $5127 (chrBanks[3] and chrBanks[7])
            for (int i = 0; i < 4; i++) {
                chrMapSprite[i] = (0x1000 * chrBanks[3] + 0x400 * i) % chrSize;
                chrMapSprite[i + 4] = (0x1000 * chrBanks[7] + 0x400 * i) % chrSize;
            }
            // Backgrounds: $512A/$512B for 8x16, $5123/$5127 for 8x8
            for (int i = 0; i < 4; i++) {
                u8 bgBank0 = sprite8x16 ? chrBanks[10] : chrBanks[3];
                u8 bgBank1 = sprite8x16 ? chrBanks[11] : chrBanks[7];
                chrMapBG[i] = (0x1000 * bgBank0 + 0x400 * i) % chrSize;
                chrMapBG[i + 4] = (0x1000 * bgBank1 + 0x400 * i) % chrSize;
            }
            break;

        case 2:  // 2KB banks
            // Sprites use $5121, $5123, $5125, $5127
            for (int i = 0; i < 2; i++) {
                chrMapSprite[i] = (0x800 * chrBanks[1] + 0x400 * i) % chrSize;
                chrMapSprite[i + 2] = (0x800 * chrBanks[3] + 0x400 * i) % chrSize;
                chrMapSprite[i + 4] = (0x800 * chrBanks[5] + 0x400 * i) % chrSize;
                chrMapSprite[i + 6] = (0x800 * chrBanks[7] + 0x400 * i) % chrSize;
            }
            // Backgrounds: $5128-$512B for 8x16, $5121/3/5/7 for 8x8
            for (int i = 0; i < 2; i++) {
                u8 bgBank0 = sprite8x16 ? chrBanks[8] : chrBanks[1];
                u8 bgBank1 = sprite8x16 ? chrBanks[9] : chrBanks[3];
                u8 bgBank2 = sprite8x16 ? chrBanks[10] : chrBanks[5];
                u8 bgBank3 = sprite8x16 ? chrBanks[11] : chrBanks[7];
                chrMapBG[i] = (0x800 * bgBank0 + 0x400 * i) % chrSize;
                chrMapBG[i + 2] = (0x800 * bgBank1 + 0x400 * i) % chrSize;
                chrMapBG[i + 4] = (0x800 * bgBank2 + 0x400 * i) % chrSize;
                chrMapBG[i + 6] = (0x800 * bgBank3 + 0x400 * i) % chrSize;
            }
            break;

        case 3:  // 1KB banks
            // Sprites use chrBanks[0-7] (full 8KB split into 1KB chunks)
            for (int i = 0; i < 8; i++) {
                chrMapSprite[i] = (0x400 * chrBanks[i]) % chrSize;
            }
            // Backgrounds: chrBanks[8-11] for 8x16, chrBanks[0-7] for 8x8
            // In 1KB mode with 8x16 sprites:
            //   $5128 -> $0000-$03FF and $1000-$13FF
            //   $5129 -> $0400-$07FF and $1400-$17FF
            //   $512A -> $0800-$0BFF and $1800-$1BFF
            //   $512B -> $0C00-$0FFF and $1C00-$1FFF
            if (sprite8x16) {
                for (int i = 0; i < 8; i++) {
                    chrMapBG[i] = (0x400 * chrBanks[8 + (i % 4)]) % chrSize;
                }
            } else {
                for (int i = 0; i < 8; i++) {
                    chrMapBG[i] = (0x400 * chrBanks[i]) % chrSize;
                }
            }
            break;
    }

}

u8 Mapper5::read_nametable(u16 addr)
{
    // Determine which nametable quadrant: 0=$2000, 1=$2400, 2=$2800, 3=$2C00
    u8 quadrant = (addr >> 10) & 0x03;

    // Get the 2-bit field from $5105 for this quadrant
    u8 source = (ntMapping >> (quadrant * 2)) & 0x03;

    // Get offset within nametable (0-0x3FF)
    u16 offset = addr & 0x3FF;

    switch (source)
    {
        case 0:  // CIRAM page 0
            return PPU::ciRam[offset];

        case 1:  // CIRAM page 1
            return PPU::ciRam[0x400 + offset];

        case 2:  // Extended RAM (when exRamMode allows)
            if (exRamMode == 0 || exRamMode == 1)
                return exRam[offset];
            return 0;

        case 3:  // Fill mode
            return fillTile;
    }

    return 0;
}

u8 Mapper5::read_attribute(u16 addr)
{
    // Determine which nametable quadrant
    u8 quadrant = (addr >> 10) & 0x03;

    // Get the 2-bit field from $5105 for this quadrant
    u8 source = (ntMapping >> (quadrant * 2)) & 0x03;

    // Get offset within attribute table (0-0x3F)
    u16 offset = addr & 0x3FF;

    switch (source)
    {
        case 0:  // CIRAM page 0
            return PPU::ciRam[offset];

        case 1:  // CIRAM page 1
            return PPU::ciRam[0x400 + offset];

        case 2:  // Extended RAM
            if (exRamMode == 0)
            {
                // Mode 0: Use ExRAM as nametable (attributes from ExRAM)
                return exRam[offset];
            }
            else if (exRamMode == 1)
            {
                // Extended Attribute Mode: Get attribute from top 2 bits of ExRAM tile data
                // The attribute for a tile is stored in bits 6-7 of the ExRAM byte for that tile
                u8 exramByte = exRam[lastNTAddr & 0x3FF];
                u8 attribute = (exramByte >> 6) & 0x03;  // Extract bits 6-7

                // Calculate tile position from lastNTAddr
                u16 tx = lastNTAddr & 0x1F;        // Tile X (0-31)
                u16 ty = (lastNTAddr >> 5) & 0x1F; // Tile Y (0-31)

                // Calculate position within 2x2 tile group
                u8 atx = tx >> 1;  // Which 2-tile column
                u8 aty = ty >> 1;  // Which 2-tile row

                // Calculate shift amount to place attribute in correct position
                // Each attribute byte covers a 2x2 tile group, with 2 bits per tile:
                // [TL TR BL BR] = [bits 1-0, bits 3-2, bits 5-4, bits 7-6]
                u8 shift = ((aty & 1) << 1) + (atx & 1);  // 0-3 depending on position in 2x2
                shift <<= 1;  // Multiply by 2 (2 bits per tile)
                attribute <<= shift;

                return attribute;
            }
            return 0;

        case 3:  // Fill mode
            // Fill mode uses $5107 for attributes (bits 0-1)
            // Replicate across all 4 2x2 groups in the attribute byte
            {
                u8 pal = fillAttr & 0x03;
                return pal | (pal << 2) | (pal << 4) | (pal << 6);
            }
    }

    return 0;
}

u8 Mapper5::read(u16 addr)
{
    // Check for NMI vector read - this signals end of frame
    if (addr == 0xFFFA || addr == 0xFFFB)
    {
        inFrame = false;
        irqStatus &= 0xBF;  // Clear in-frame flag (bit 6)
    }

    // Extended RAM read ($5C00-$5FFF)
    if (addr >= 0x5C00 && addr < 0x6000)
    {
        // ExRAM can be read in modes 2 and 3
        if (exRamMode >= 2)
            return exRam[addr - 0x5C00];
        return 0;
    }

    // Register reads
    switch (addr)
    {
        case 0x5204:  // IRQ status
        {
            u8 status = irqStatus;
            irqStatus &= 0x7F;  // Clear pending bit on read
            CPU::set_irq(false);
            return status;
        }

        case 0x5205:  // Multiply result low
            return (multiplicand * multiplier) & 0xFF;

        case 0x5206:  // Multiply result high
            return (multiplicand * multiplier) >> 8;

        // For unimplemented registers, return open bus (0)
        default:
            if (addr >= 0x5000 && addr < 0x5C00)
                return 0;
            break;
    }

    // PRG ROM/RAM access at $8000-$FFFF
    if (addr >= 0x8000)
    {
        // Determine which 8KB slot (0-3)
        int slot = (addr - 0x8000) / 0x2000;

        // Check if this slot is mapped to RAM or ROM
        if (prgIsRam[slot])
        {
            // Read from PRG RAM
            // Bits 0-2 of bank register select which 8KB chunk of 64KB RAM
            u8 ramBank = prgBanks[slot + 1] & 0x07;
            u16 offset = (addr - 0x8000) % 0x2000;
            u32 ramAddr = (ramBank * 0x2000) + offset;

            // Clamp to prgRamSize
            if (ramAddr < prgRamSize)
                return prgRam[ramAddr];
            return 0;
        }
        else
        {
            // Read from PRG ROM via standard banking
            return Mapper::read(addr);
        }
    }
    else if (addr >= 0x6000)
    {
        // PRG RAM access via $5113 bank
        // $5113 controls which 8KB bank is mapped to $6000-$7FFF
        u8 ramBank = prgBanks[0] & 0x07;
        u16 offset = addr - 0x6000;
        u32 ramAddr = (ramBank * 0x2000) + offset;

        if (ramAddr < prgRamSize)
            return prgRam[ramAddr];
        return 0;
    }

    return 0;
}

u8 Mapper5::write(u16 addr, u8 v)
{
    // Extended RAM write ($5C00-$5FFF)
    if (addr >= 0x5C00 && addr < 0x6000)
    {
        // ExRAM can be written in modes 2 and 3
        if (exRamMode >= 2)
            exRam[addr - 0x5C00] = v;
        return v;
    }

    // Register writes
    if (addr >= 0x5000 && addr < 0x5C00)
    {
        switch (addr)
        {
            case 0x5100:  // PRG banking mode
                prgMode = v & 0x03;
                apply_prg_banking();
                break;

            case 0x5101:  // CHR banking mode
                chrMode = v & 0x03;
                apply_chr_banking();
                break;

            case 0x5102:  // PRG RAM protect 1
                prgRamProtect1 = v & 0x03;
                break;

            case 0x5103:  // PRG RAM protect 2
                prgRamProtect2 = v & 0x03;
                break;

            case 0x5104:  // Extended RAM mode
                exRamMode = v & 0x03;
                break;

            case 0x5105:  // Nametable mapping
                ntMapping = v;
                // Set FOUR_SCREEN mode to allow custom mapping
                set_mirroring(PPU::FOUR_SCREEN);
                break;

            case 0x5106:  // Fill mode tile
                fillTile = v;
                break;

            case 0x5107:  // Fill mode attribute
                fillAttr = v & 0x03;
                break;

            case 0x5113:  // PRG RAM bank ($6000-$7FFF)
            case 0x5114:  // PRG bank 0 ($8000-$9FFF)
            case 0x5115:  // PRG bank 1 ($A000-$BFFF)
            case 0x5116:  // PRG bank 2 ($C000-$DFFF)
            case 0x5117:  // PRG bank 3 ($E000-$FFFF)
                prgBanks[addr - 0x5113] = v;
                apply_prg_banking();
                break;

            case 0x5120:  // CHR bank 0 (sprite)
            case 0x5121:  // CHR bank 1 (sprite)
            case 0x5122:  // CHR bank 2 (sprite)
            case 0x5123:  // CHR bank 3 (sprite)
            case 0x5124:  // CHR bank 4 (sprite)
            case 0x5125:  // CHR bank 5 (sprite)
            case 0x5126:  // CHR bank 6 (sprite)
            case 0x5127:  // CHR bank 7 (sprite)
                ab_mode = 0;  // Track that 'A' registers were written
                {
                    int bankIdx = addr - 0x5120;
                    // Only apply banking if value actually changed
                    if (chrBanks[bankIdx] != v) {
                        chrBanks[bankIdx] = v;
                        apply_chr_banking();
                    }
                }
                break;

            case 0x5128:  // CHR bank 8 (background)
            case 0x5129:  // CHR bank 9 (background)
            case 0x512A:  // CHR bank A (background)
            case 0x512B:  // CHR bank B (background)
                ab_mode = 1;  // Track that 'B' registers were written
                {
                    int bankIdx = addr - 0x5120;
                    // Only apply banking if value actually changed
                    if (chrBanks[bankIdx] != v) {
                        chrBanks[bankIdx] = v;
                        apply_chr_banking();
                    }
                }
                break;

            case 0x5130:  // CHR high bits (upper 2 bits of CHR address)
                chrRegHigh = v & 0x03;
                break;

            case 0x5200:  // Vertical split mode
                splitMode = v;
                splitSide = (v >> 6) & 0x01;  // Bit 6: which side
                break;

            case 0x5201:  // Vertical split scroll
                splitScroll = v;
                break;

            case 0x5202:  // Vertical split CHR bank
                splitBank = v;
                break;

            case 0x5203:  // IRQ scanline compare value
                irqScanline = v;
                break;

            case 0x5204:  // IRQ enable
                irqEnabled = v & 0x80;
                if (!irqEnabled)
                {
                    irqStatus &= 0x7F;  // Clear pending
                    CPU::set_irq(false);
                }
                break;

            case 0x5205:  // Multiplicand
                multiplicand = v;
                break;

            case 0x5206:  // Multiplier
                multiplier = v;
                break;
        }
        return v;
    }

    // PRG RAM write at $8000-$FFFF
    if (addr >= 0x8000)
    {
        // Determine which 8KB slot (0-3)
        int slot = (addr - 0x8000) / 0x2000;

        // Check if this slot is mapped to RAM
        if (prgIsRam[slot])
        {
            // Check write protection
            bool writeProtected = !((prgRamProtect1 == 0x02) && (prgRamProtect2 == 0x01));
            if (!writeProtected)
            {
                u8 ramBank = prgBanks[slot + 1] & 0x07;
                u16 offset = (addr - 0x8000) % 0x2000;
                u32 ramAddr = (ramBank * 0x2000) + offset;

                if (ramAddr < prgRamSize)
                    prgRam[ramAddr] = v;
            }
        }
        return v;
    }

    // PRG RAM write at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000)
    {
        // Check write protection
        bool writeProtected = !((prgRamProtect1 == 0x02) && (prgRamProtect2 == 0x01));
        if (!writeProtected)
        {
            u8 ramBank = prgBanks[0] & 0x07;
            u16 offset = addr - 0x6000;
            u32 ramAddr = (ramBank * 0x2000) + offset;

            if (ramAddr < prgRamSize)
                prgRam[ramAddr] = v;
        }
        return v;
    }

    return v;
}

u32 Mapper5::map_chr(u16 addr)
{
    // BizHawk-style CHR mapping
    // Extract 1KB bank slot (0-7) and offset within 1KB
    int bank_1k = addr >> 10;  // Which 1KB slot
    int ofs = addr & 0x3FF;    // Offset within 1KB

    // ExRAM mode 1: Extended Attribute Mode - per-tile CHR banking for backgrounds
    // Determine if we're in sprite or background phase based on dot position
    // load_sprites() is called at dot 321 and fetches all sprite tile data there
    bool isSprite = (PPU::dot >= 257 && PPU::dot <= 321);
    bool rendering = (PPU::mask.bg || PPU::mask.spr);

    if (exRamMode == 1 && !isSprite && rendering)
    {
        // Use ExRAM to determine which 4KB CHR bank this tile uses
        u8 exramByte = exRam[lastNTAddr & 0x3FF];
        u32 bank4k = exramByte & 0x3F;  // Bits 0-5: 4KB bank selector

        // Combine with chrRegHigh for upper 2 bits (supports >256KB CHR)
        u32 bank1k = (bank4k * 4) + ((u32)chrRegHigh << 8);

        // Offset within 4KB bank (12 bits)
        u32 offset = addr & 0x0FFF;
        return ((bank1k & (chrSize / 0x400 - 1)) * 0x400) + offset;
    }

    // Normal CHR banking
    // Check PPU's current sprite size (like Mesen does)
    if (PPU::ctrl.sprSz)  // 8x16 sprite mode
    {
        if (isSprite && rendering)
            bank_1k = chrMapSprite[bank_1k] / 0x400;  // Use 'A' banks for sprites
        else if (!isSprite && rendering)
            bank_1k = chrMapBG[bank_1k] / 0x400;      // Use 'B' banks for backgrounds
        else
            // Not rendering: use whichever bank set was written last
            bank_1k = (ab_mode == 0 ? chrMapSprite[bank_1k] : chrMapBG[bank_1k]) / 0x400;
    }
    else  // 8x8 sprite mode
    {
        // ALWAYS use 'A' banks (sprite banks) for both sprites and backgrounds
        bank_1k = chrMapSprite[bank_1k] / 0x400;
    }

    // Mask to valid CHR size and return final address
    bank_1k &= (chrSize / 0x400 - 1);
    return (bank_1k * 0x400) + ofs;
}

u8 Mapper5::chr_read(u16 addr)
{
    // Nametable reads ($2000-$2FFF)
    if (addr >= 0x2000 && addr < 0x3000)
    {
        // Check if this is attribute table read ($23C0-$23FF, etc.)
        if ((addr & 0x3FF) >= 0x3C0)
            return read_attribute(addr);
        else
            return read_nametable(addr);
    }

    // Mirror $3000-$3FFF to $2000-$2FFF
    if (addr >= 0x3000 && addr < 0x4000)
        return chr_read(addr - 0x1000);

    // Pattern table read
    if (addr < 0x2000)
    {
        // Use BizHawk-style CHR mapping
        u32 chrAddr = map_chr(addr);

        if (chrAddr < chrSize)
            return chr[chrAddr];
        return 0;
    }

    return 0;
}

u8 Mapper5::chr_write(u16 addr, u8 v)
{
    // Nametable writes ($2000-$2FFF)
    if (addr >= 0x2000 && addr < 0x3000)
    {
        // Determine which nametable quadrant
        u8 quadrant = (addr >> 10) & 0x03;
        u8 source = (ntMapping >> (quadrant * 2)) & 0x03;
        u16 offset = addr & 0x3FF;

        switch (source)
        {
            case 0:  // CIRAM page 0
                PPU::ciRam[offset] = v;
                break;

            case 1:  // CIRAM page 1
                PPU::ciRam[0x400 + offset] = v;
                break;

            case 2:  // Extended RAM (writable during rendering in modes 0/1)
                if (exRamMode == 0 || exRamMode == 1)
                    exRam[offset] = v;
                break;

            case 3:  // Fill mode (not writable)
                break;
        }
        return v;
    }

    // Mirror $3000-$3FFF to $2000-$2FFF
    if (addr >= 0x3000 && addr < 0x4000)
        return chr_write(addr - 0x1000, v);

    // CHR RAM write - only if we have CHR-RAM (not CHR-ROM)
    if (addr < 0x2000 && chrRam)
    {
        // Use the same banking logic as chr_read
        u32 chrAddr = map_chr(addr);

        if (chrAddr < chrSize)
            chr[chrAddr] = v;
    }
    // CHR-ROM: writes are ignored (read-only)

    return v;
}

void Mapper5::signal_scanline(int scanline)
{
    // MMC5 doesn't use PPU scanline signals - it detects scanlines via PPU reads
    // Just reset split screen state and fetch counter per scanline
    splitTileCount = 0;
    splitActive = false;
    fetchCount = 0;
}

void Mapper5::ppu_read_hook(u16 addr)
{
    // Track nametable reads for:
    // 1. Scanline detection (for IRQ)
    // 2. Split screen
    // 3. lastNTAddr (for ExRAM mode 1)

    if (addr >= 0x2000 && addr < 0x3000)
    {
        // Track nametable tile address for ExRAM mode 1
        u16 nt_entry = addr & 0x3FF;
        if (nt_entry < 0x3C0)  // Not attribute table
            lastNTAddr = nt_entry;

        // Track consecutive reads from EXACT same address for scanline detection
        if (addr == lastReadAddr)
        {
            sameAddrCount++;

            // On third consecutive read from same address, increment scanline counter
            if (sameAddrCount == 3)
            {
                // First time we see this pattern, set in-frame
                if (!inFrame)
                {
                    inFrame = true;
                    irqCounter = 0;
                    irqStatus |= 0x40;  // Set in-frame flag
                }
                else
                {
                    // Already in frame, just increment scanline counter
                    if (irqCounter < 240)
                        irqCounter++;
                }

                // Check if we've hit the target scanline
                if (irqCounter == irqScanline)
                {
                    irqStatus |= 0x80;  // Set pending flag
                    if (irqEnabled)
                        CPU::set_irq(true);
                }

                // Reset counter so we can detect next scanline
                // (keep lastReadAddr so 4th, 5th read still count as "same")
                sameAddrCount = 0;
            }
        }
        else
        {
            // Different address, reset tracking
            lastReadAddr = addr;
            sameAddrCount = 1;
        }

        // Handle vertical split screen
        if (splitMode & 0x80)  // Split enabled
        {
            splitTileCount++;

            u8 splitThreshold = splitMode & 0x1F;  // Bits 0-4 = tile count

            // Check if we've crossed the split threshold
            if (splitSide == 0)  // Split on left side
            {
                if (splitTileCount <= splitThreshold)
                    splitActive = true;
                else
                    splitActive = false;
            }
            else  // Split on right side
            {
                if (splitTileCount > splitThreshold)
                    splitActive = true;
                else
                    splitActive = false;
            }
        }
    }
    else if (addr >= 0x3000)
    {
        // Non-nametable read, reset consecutive counter
        lastReadAddr = 0xFFFF;
        sameAddrCount = 0;
    }
}

void Mapper5::ppu_write_hook(u16 index, u8 v)
{
    // MMC5 watches writes to $2000 (PPUCTRL) to detect sprite size changes
    // When switching to 8x8 sprites, reset last written bank to 'A' set (like Mesen does)
    if (index == 0 && !(v & 0x20))  // PPUCTRL, 8x8 sprite mode
    {
        ab_mode = 0;  // Reset to 'A' banks when switching to 8x8
    }
}

bool Mapper5::check_irq(int elapsed)
{
    // MMC5 IRQ is fired immediately when scanline is detected (in ppu_read_hook)
    // This function just returns whether IRQ is currently active
    return (irqStatus & 0x80) && irqEnabled;
}
