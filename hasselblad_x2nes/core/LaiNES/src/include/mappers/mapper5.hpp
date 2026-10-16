#pragma once
#include "mapper.hpp"


class Mapper5 : public Mapper
{
    // Banking mode registers
    u8 prgMode;     // $5100: PRG banking mode (0-3)
    u8 chrMode;     // $5101: CHR banking mode (0-3)

    // RAM protection (not fully implemented yet)
    u8 prgRamProtect1;  // $5102
    u8 prgRamProtect2;  // $5103

    // Extended RAM mode
    u8 exRamMode;   // $5104: Extended RAM mode (0-3)

    // Nametable mapping
    u8 ntMapping;   // $5105: Nametable mapping

    // Fill mode (for nametable fill)
    u8 fillTile;    // $5106: Fill mode tile
    u8 fillAttr;    // $5107: Fill mode attribute

    // PRG bank registers
    u8 prgBanks[5]; // $5113-$5117: PRG banks (RAM + 4 ROM banks)
    bool prgIsRam[4]; // Track if each 8KB slot at $8000-$FFFF is RAM (not ROM)

    // CHR bank registers: $5120-$5127 for sprites (8 banks), $5128-$512B for background (4 banks)
    u8 chrBanks[12]; // $5120-$512B: CHR banks
    u8 chrRegHigh;   // $5130: Upper 2 bits of CHR address (for >256KB CHR)
    u8 ab_mode;      // Track which CHR bank set was written last (0='A' regs, 1='B' regs)

    // Split screen
    u8 splitMode;   // $5200: Vertical split mode
    u8 splitScroll; // $5201: Vertical split scroll
    u8 splitBank;   // $5202: Vertical split CHR bank

    // IRQ
    u8 irqScanline;    // $5203: IRQ target scanline
    u8 irqStatus;      // $5204: IRQ status/enable
    bool irqEnabled;
    u8 irqCounter;     // Current scanline counter
    bool inFrame;      // Tracking if we're in a frame

    // PPU read tracking for scanline detection
    u16 lastReadAddr;
    u8 sameAddrCount;

    // Split screen tracking
    u8 splitTileCount;  // Current tile count for split detection
    bool splitActive;   // Whether split is currently active
    u8 splitSide;       // Which side the split is on

    // Multiply
    u8 multiplicand;   // $5205: Multiplicand
    u8 multiplier;     // $5206: Multiplier

    // Extended RAM (1KB internal RAM)
    u8 exRam[0x400];   // $5C00-$5FFF

    // CHR banking: MMC5 uses different banks for BG vs sprite fetches
    u32 chrMapBG[8];      // CHR map for background fetches
    u32 chrMapSprite[8];  // CHR map for sprite fetches
    bool fetchingSprite;  // True when PPU is fetching sprite data
    u8 fetchCount;        // Counts pattern table fetches per scanline (0-84)
    int lastScanline;     // Last scanline number seen (for detecting scanline changes)

    // Fetch detection state machine
    enum FetchState { FETCH_IDLE, FETCH_NT, FETCH_AT, FETCH_PT_LOW, FETCH_PT_HIGH };
    FetchState fetchState;
    u16 lastNTAddr;

    // Apply current banking configuration
    void apply_prg_banking();
    void apply_chr_banking();

    // Helper functions for nametable/attribute handling
    u8 read_nametable(u16 addr);
    u8 read_attribute(u16 addr);

    // CHR mapping function (BizHawk-style)
    u32 map_chr(u16 addr);

  public:
    Mapper5(u8* rom) : Mapper(rom)
    {
        // Initialize registers
        prgMode = 3;  // Default to mode 3 (8KB banks)
        chrMode = 0;  // Default to mode 0 (8KB banks)
        prgRamProtect1 = prgRamProtect2 = 0;
        exRamMode = 0;
        ntMapping = 0;
        fillTile = fillAttr = 0;

        // Initialize PRG banks
        // FCEUX-style: All banks start at 0xFF, then bit 7 = ROM, bits 0-6 = bank number
        // For reset vector to work, last banks must map to end of ROM with bit 7 set
        u8 lastBank = ((prgSize / 0x2000) - 1) & 0x7F;  // Calculate last 8KB bank

        prgBanks[0] = 0;  // $5113 (RAM bank - bit 7 clear)
        prgBanks[1] = 0x80 | lastBank;  // $5114 - last ROM bank (power-on state)
        prgBanks[2] = 0x80 | lastBank;  // $5115 - last ROM bank (power-on state)
        prgBanks[3] = 0x80 | lastBank;  // $5116 - last ROM bank
        prgBanks[4] = 0x80 | lastBank;  // $5117 - last ROM bank

        for (int i = 0; i < 12; i++)
            chrBanks[i] = 0;
        chrRegHigh = 0;
        ab_mode = 0;  // Default to 'A' registers

        for (int i = 0; i < 4; i++)
            prgIsRam[i] = false;

        splitMode = splitScroll = splitBank = 0;
        splitTileCount = 0;
        splitActive = false;
        splitSide = 0;

        irqScanline = irqStatus = irqCounter = 0;
        irqEnabled = false;
        inFrame = false;
        lastReadAddr = 0;
        sameAddrCount = 0;

        multiplicand = multiplier = 0;

        for (int i = 0; i < 0x400; i++)
            exRam[i] = 0;

        // Initialize CHR maps
        for (int i = 0; i < 8; i++) {
            chrMapBG[i] = 0;
            chrMapSprite[i] = 0;
        }
        fetchingSprite = false;
        fetchState = FETCH_IDLE;
        lastNTAddr = 0;
        fetchCount = 0;
        lastScanline = -1;

        // Set up initial banking
        apply_prg_banking();
        apply_chr_banking();
    }

    u8 read(u16 addr) override;
    u8 write(u16 addr, u8 v) override;
    u8 chr_read(u16 addr) override;
    u8 chr_write(u16 addr, u8 v) override;

    // MMC5 handles expansion area addresses ($5000-$5FFF)
    bool handles_expansion_addr(u16 addr) override {
        return (addr >= 0x5000 && addr < 0x6000);
    }

    void signal_scanline(int scanline) override;
    void ppu_read_hook(u16 addr) override;
    void ppu_write_hook(u16 index, u8 v) override;

    // IRQ support
    bool check_irq(int elapsed) override;

    // Save state support
    u32 get_state_size() const override {
        return sizeof(prgMode) + sizeof(chrMode) + sizeof(prgRamProtect1) + sizeof(prgRamProtect2) +
               sizeof(exRamMode) + sizeof(ntMapping) + sizeof(fillTile) + sizeof(fillAttr) +
               sizeof(prgBanks) + sizeof(prgIsRam) + sizeof(chrBanks) + sizeof(chrRegHigh) +
               sizeof(ab_mode) + sizeof(splitMode) + sizeof(splitScroll) + sizeof(splitBank) +
               sizeof(irqScanline) + sizeof(irqStatus) + sizeof(irqEnabled) + sizeof(irqCounter) +
               sizeof(inFrame) + sizeof(lastReadAddr) + sizeof(sameAddrCount) + sizeof(splitTileCount) +
               sizeof(splitActive) + sizeof(splitSide) + sizeof(multiplicand) + sizeof(multiplier) +
               sizeof(exRam) + sizeof(fetchingSprite) + sizeof(fetchCount) + sizeof(lastScanline) +
               sizeof(u8) + sizeof(lastNTAddr);  // fetchState as u8
    }

    void save_state(u8* buffer) const override {
        int offset = 0;
        buffer[offset++] = prgMode;
        buffer[offset++] = chrMode;
        buffer[offset++] = prgRamProtect1;
        buffer[offset++] = prgRamProtect2;
        buffer[offset++] = exRamMode;
        buffer[offset++] = ntMapping;
        buffer[offset++] = fillTile;
        buffer[offset++] = fillAttr;
        memcpy(buffer + offset, prgBanks, sizeof(prgBanks));
        offset += sizeof(prgBanks);
        memcpy(buffer + offset, prgIsRam, sizeof(prgIsRam));
        offset += sizeof(prgIsRam);
        memcpy(buffer + offset, chrBanks, sizeof(chrBanks));
        offset += sizeof(chrBanks);
        buffer[offset++] = chrRegHigh;
        buffer[offset++] = ab_mode;
        buffer[offset++] = splitMode;
        buffer[offset++] = splitScroll;
        buffer[offset++] = splitBank;
        buffer[offset++] = irqScanline;
        buffer[offset++] = irqStatus;
        buffer[offset++] = irqEnabled ? 1 : 0;
        buffer[offset++] = irqCounter;
        buffer[offset++] = inFrame ? 1 : 0;
        memcpy(buffer + offset, &lastReadAddr, sizeof(lastReadAddr));
        offset += sizeof(lastReadAddr);
        buffer[offset++] = sameAddrCount;
        buffer[offset++] = splitTileCount;
        buffer[offset++] = splitActive ? 1 : 0;
        buffer[offset++] = splitSide;
        buffer[offset++] = multiplicand;
        buffer[offset++] = multiplier;
        memcpy(buffer + offset, exRam, sizeof(exRam));
        offset += sizeof(exRam);
        buffer[offset++] = fetchingSprite ? 1 : 0;
        buffer[offset++] = fetchCount;
        memcpy(buffer + offset, &lastScanline, sizeof(lastScanline));
        offset += sizeof(lastScanline);
        buffer[offset++] = (u8)fetchState;
        memcpy(buffer + offset, &lastNTAddr, sizeof(lastNTAddr));
    }

    void load_state(const u8* buffer) override {
        int offset = 0;
        prgMode = buffer[offset++];
        chrMode = buffer[offset++];
        prgRamProtect1 = buffer[offset++];
        prgRamProtect2 = buffer[offset++];
        exRamMode = buffer[offset++];
        ntMapping = buffer[offset++];
        fillTile = buffer[offset++];
        fillAttr = buffer[offset++];
        memcpy(prgBanks, buffer + offset, sizeof(prgBanks));
        offset += sizeof(prgBanks);
        memcpy(prgIsRam, buffer + offset, sizeof(prgIsRam));
        offset += sizeof(prgIsRam);
        memcpy(chrBanks, buffer + offset, sizeof(chrBanks));
        offset += sizeof(chrBanks);
        chrRegHigh = buffer[offset++];
        ab_mode = buffer[offset++];
        splitMode = buffer[offset++];
        splitScroll = buffer[offset++];
        splitBank = buffer[offset++];
        irqScanline = buffer[offset++];
        irqStatus = buffer[offset++];
        irqEnabled = buffer[offset++] != 0;
        irqCounter = buffer[offset++];
        inFrame = buffer[offset++] != 0;
        memcpy(&lastReadAddr, buffer + offset, sizeof(lastReadAddr));
        offset += sizeof(lastReadAddr);
        sameAddrCount = buffer[offset++];
        splitTileCount = buffer[offset++];
        splitActive = buffer[offset++] != 0;
        splitSide = buffer[offset++];
        multiplicand = buffer[offset++];
        multiplier = buffer[offset++];
        memcpy(exRam, buffer + offset, sizeof(exRam));
        offset += sizeof(exRam);
        fetchingSprite = buffer[offset++] != 0;
        fetchCount = buffer[offset++];
        memcpy(&lastScanline, buffer + offset, sizeof(lastScanline));
        offset += sizeof(lastScanline);
        fetchState = (FetchState)buffer[offset++];
        memcpy(&lastNTAddr, buffer + offset, sizeof(lastNTAddr));
        apply_prg_banking();
        apply_chr_banking();
    }
};
