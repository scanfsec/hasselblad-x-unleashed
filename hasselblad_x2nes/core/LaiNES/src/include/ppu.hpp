#pragma once
#include "common.hpp"

namespace PPU {


enum Scanline  { VISIBLE, POST, NMI, PRE };
enum Mirroring { VERTICAL, HORIZONTAL, ONE_SCREEN_HI, ONE_SCREEN_LO, FOUR_SCREEN };

/* Sprite buffer */
struct Sprite
{
    u8 id;     // Index in OAM.
    u8 x;      // X position.
    u8 y;      // Y position.
    u8 tile;   // Tile index.
    u8 attr;   // Attributes.
    u8 dataL;  // Tile data (low).
    u8 dataH;  // Tile data (high).
};

/* PPUCTRL ($2000) register */
union Ctrl
{
    struct
    {
        unsigned nt     : 2;  // Nametable ($2000 / $2400 / $2800 / $2C00).
        unsigned incr   : 1;  // Address increment (1 / 32).
        unsigned sprTbl : 1;  // Sprite pattern table ($0000 / $1000).
        unsigned bgTbl  : 1;  // BG pattern table ($0000 / $1000).
        unsigned sprSz  : 1;  // Sprite size (8x8 / 8x16).
        unsigned slave  : 1;  // PPU master/slave.
        unsigned nmi    : 1;  // Enable NMI.
    };
    u8 r;
};

/* PPUMASK ($2001) register */
union Mask
{
    struct
    {
        unsigned gray    : 1;  // Grayscale.
        unsigned bgLeft  : 1;  // Show background in leftmost 8 pixels.
        unsigned sprLeft : 1;  // Show sprite in leftmost 8 pixels.
        unsigned bg      : 1;  // Show background.
        unsigned spr     : 1;  // Show sprites.
        unsigned red     : 1;  // Intensify reds.
        unsigned green   : 1;  // Intensify greens.
        unsigned blue    : 1;  // Intensify blues.
    };
    u8 r;
};

/* PPUSTATUS ($2002) register */
union Status
{
    struct
    {
        unsigned bus    : 5;  // Not significant.
        unsigned sprOvf : 1;  // Sprite overflow.
        unsigned sprHit : 1;  // Sprite 0 Hit.
        unsigned vBlank : 1;  // In VBlank?
    };
    u8 r;
};

/* Loopy's VRAM address */
union Addr
{
    struct
    {
        unsigned cX : 5;  // Coarse X.
        unsigned cY : 5;  // Coarse Y.
        unsigned nt : 2;  // Nametable.
        unsigned fY : 3;  // Fine Y.
    };
    struct
    {
        unsigned l : 8;   // Low byte (bits 0-7)
        unsigned h : 6;   // High byte (bits 8-13)
        unsigned   : 1;   // Unused (bit 14)
    };
    unsigned addr : 14;
    unsigned r : 15;
};

template <bool write> u8 access(u16 index, u8 v = 0, bool rmw = false);
void set_mirroring(Mirroring mode);
void step();
void reset();

// Exposed for MMC5 (needs to know rendering phase and access nametables)
extern u8 ciRam[0x800];  // Nametable RAM
extern int dot;          // Current dot/cycle within scanline
extern Ctrl ctrl;        // PPUCTRL register (for sprite size)
extern Mask mask;        // PPUMASK register (for rendering enabled check)
extern Mirroring mirroring;  // Current mirroring mode
u16 nt_mirror(u16 addr); // Nametable mirroring function

// Exposed for save states
extern u8 cgRam[0x20];   // Palette RAM
extern u8 oamMem[0x100]; // OAM memory
extern int scanline;     // Current scanline
extern Status status;    // PPUSTATUS register

// Save state support - get/set PPU internal registers
struct PpuState {
    u16 vram_addr;      // vAddr.r
    u16 temp_addr;      // tAddr.r
    u8 fine_x;          // fX
    u8 oam_addr;        // oamAddr
    u8 read_buffer;     // readBuffer
    u8 open_bus;        // openBus
    bool write_latch;   // latch
    bool frame_odd;     // frameOdd
};

void get_state(PpuState& state);
void set_state(const PpuState& state);


}
