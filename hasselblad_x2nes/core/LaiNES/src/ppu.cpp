#include "cartridge.hpp"
#include "cpu.hpp"
#include "gui.hpp"
#include "ppu.hpp"
#include <cstdio>

namespace PPU {
#include "palette.inc"


Mirroring mirroring;       // Mirroring mode.
u8 ciRam[0x800];           // VRAM for nametables.
u8 cgRam[0x20];            // VRAM for palettes.
u8 oamMem[0x100];          // VRAM for sprite properties.
Sprite oam[8], secOam[8];  // Sprite buffers.
u32 pixels[256 * 240];     // Video buffer.

Addr vAddr, tAddr;  // Loopy V, T.
u8 fX;              // Fine X.
u8 oamAddr;         // OAM address.
u8 readBuffer;      // VRAM read buffer for $2007.
u8 openBus;         // PPU I/O bus value for open bus behavior.

Ctrl ctrl;      // PPUCTRL   ($2000) register.
Mask mask;      // PPUMASK   ($2001) register.
Status status;  // PPUSTATUS ($2002) register.
bool vBlankSuppressed = false;  // VBlank suppression flag

// Background latches:
u8 nt, at, bgL, bgH;
// Background shift registers:
u8 atShiftL, atShiftH; u16 bgShiftL, bgShiftH;
bool atLatchL, atLatchH;

// Rendering counters:
int scanline, dot;
bool frameOdd;

// PPU warmup counter (PPU ignores certain writes for ~29658 CPU cycles after reset)
static int warmupCycles;

inline bool rendering() { return mask.bg || mask.spr; }
inline int spr_height() { return ctrl.sprSz ? 16 : 8; }

/* Get CIRAM address according to mirroring */
u16 nt_mirror(u16 addr)
{
    switch (mirroring)
    {
        case VERTICAL:    return addr % 0x800;
        case HORIZONTAL:  return ((addr / 2) & 0x400) + (addr % 0x400);
        case ONE_SCREEN_LO:
        case ONE_SCREEN_HI:
                          return (mirroring == ONE_SCREEN_HI) ? 0x400 + (addr & 0x3ff) : (addr & 0x3ff);
        default:          return addr - 0x2000;
    }
}
void set_mirroring(Mirroring mode) { mirroring = mode; }

/* Access PPU memory */
u8 rd(u16 addr)
{
    switch (addr)
    {
        case 0x0000 ... 0x1FFF:  return Cartridge::chr_access<0>(addr);  // CHR-ROM/RAM.
        case 0x2000 ... 0x3EFF:  return Cartridge::chr_access<0>(addr);  // Nametables (via mapper).
        case 0x3F00 ... 0x3FFF:  // Palettes:
            if ((addr & 0x13) == 0x10) addr &= ~0x10;
            return cgRam[addr & 0x1F] & (mask.gray ? 0x30 : 0xFF);
        default: return addr & 0xFF;  // Open bus on video memory bus returns low byte of address
    }
}
void wr(u16 addr, u8 v)
{
    switch (addr)
    {
        case 0x0000 ... 0x1FFF:  Cartridge::chr_access<1>(addr, v); break;  // CHR-ROM/RAM.
        case 0x2000 ... 0x3EFF:  Cartridge::chr_access<1>(addr, v); break;  // Nametables (via mapper).
        case 0x3F00 ... 0x3FFF:  // Palettes:
            if ((addr & 0x13) == 0x10) addr &= ~0x10;
            cgRam[addr & 0x1F] = v; break;
    }
}

static bool latch;  // Detect second reading (moved out for reset access)

/* Access PPU through registers. */
template <bool write> u8 access(u16 index, u8 v, bool rmw)
{
    // v parameter contains the current data bus value
    

    /* Write into register */
    if (write)
    {
        openBus = v;  // All writes update PPU open bus
        Cartridge::ppu_write_hook(index, v);  // Let mapper spy on PPU register writes
        switch (index)
        {
            case 0:  // PPUCTRL   ($2000).
                if (warmupCycles <= 0) {
                    ctrl.r = v;
                    tAddr.nt = ctrl.nt;
                    // Always update NMI line to reflect ctrl.nmi && status.vBlank
                    CPU::set_nmi(ctrl.nmi && status.vBlank);
                }
                break;
            case 1:  // PPUMASK   ($2001).
                if (warmupCycles <= 0) { mask.r = v; }
                break;
            case 2:  // PPUSTATUS is read-only, write does nothing but update open bus
                break;
            case 3:  oamAddr = v; break;                          // OAMADDR   ($2003).
            case 4:  oamMem[oamAddr++] = v; break;                // OAMDATA   ($2004).
            case 5:                                               // PPUSCROLL ($2005).
                if (warmupCycles <= 0) {
                    if (!latch) { fX = v & 7; tAddr.cX = v >> 3; }      // First write.
                    else  { tAddr.fY = v & 7; tAddr.cY = v >> 3; }      // Second write.
                    latch = !latch;
                }
                break;
            case 6:                                               // PPUADDR   ($2006).
                if (warmupCycles <= 0) {
                    if (!latch) {
                        // First write: set high 6 bits (bits 8-13 of address), clear bit 14
                        tAddr.r = (tAddr.r & 0x00FF) | ((v & 0x3F) << 8);
                    }                 // First write.
                    else {
                        // Second write: set low 8 bits (bits 0-7 of address)
                        tAddr.r = (tAddr.r & 0x3F00) | v;
                        vAddr.r = tAddr.r;
                    }     // Second write.
                    latch = !latch;
                }
                break;
            case 7:  // PPUDATA ($2007).
                if (warmupCycles <= 0) {
                    // For RMW instructions, perform an extra write
                    if (rmw && write) {
                        // Extra glitched write at (v & 0xFF00) | buffer
                        // The buffer contains the incremented/decremented value
                        u16 glitchAddr = (vAddr.addr & 0xFF00) | v;
                        wr(glitchAddr, v);
                    }
                    wr(vAddr.addr, v);
                    vAddr.addr += ctrl.incr ? 32 : 1;
                }
                break;
        }
        return v;  // Writes return the value written
    }
    /* Read from register */
    else
        switch (index)
        {
            case 0:  // PPUCTRL is write-only, return PPU open bus
                v = openBus;
                break;
            case 1:  // PPUMASK is write-only, return PPU open bus
                v = openBus;
                break;
            // PPUSTATUS ($2002):
            case 2:
                v = (openBus & 0x1F) | status.r;  // Bits 4-0 from open bus, 7-5 from status
                openBus = v;  // Reading $2002 updates open bus
                // Check if we're reading $2002 on the exact cycle VBlank would be set
                // VBlank is set at scanline 241, dot 1
                // With mid-cycle timing: after 2 PPU steps, dot has been incremented to 2
                // So we check for dot 0, 1, or 2 depending on CPU/PPU alignment
                if (scanline == 241 && (dot <= 2)) {
                    // Reading during dots 0-2 can suppress VBlank depending on exact timing
                    // For mid-cycle timing (ppu_sub_cycle==2), dot==2 means VBlank just set
                    vBlankSuppressed = true;  // Suppress VBlank flag setting (race condition)
                }
                status.vBlank = 0; latch = 0;
                // Update NMI line: VBlank cleared, so line = ctrl.nmi && false = false
                CPU::set_nmi(ctrl.nmi && status.vBlank);
                break;
            case 3:  // OAMADDR is write-only, return PPU open bus
                v = openBus;
                break;
            case 4:
                v = oamMem[oamAddr];
                openBus = v;  // Reading $2004 updates open bus
                break;  // OAMDATA ($2004).
            case 5:  // PPUSCROLL is write-only, return PPU open bus
                v = openBus;
                break;
            case 6:  // PPUADDR is write-only, return PPU open bus
                v = openBus;
                break;
            case 7:                                     // PPUDATA ($2007).
                if (vAddr.addr <= 0x3EFF)
                {
                    v = readBuffer;
                    readBuffer = rd(vAddr.addr);
                }
                else
                    v = readBuffer = rd(vAddr.addr);
                openBus = v;  // Reading $2007 updates open bus
                vAddr.addr += ctrl.incr ? 32 : 1; break;
            // Invalid registers return open bus
            default:
                v = openBus;
                break;
        }


    return v;
}
template u8 access<0>(u16, u8, bool); template u8 access<1>(u16, u8, bool);

/* Calculate graphics addresses */
inline u16 nt_addr() { return 0x2000 | (vAddr.r & 0xFFF); }
inline u16 at_addr() { return 0x23C0 | (vAddr.nt << 10) | ((vAddr.cY / 4) << 3) | (vAddr.cX / 4); }
inline u16 bg_addr() { return (ctrl.bgTbl * 0x1000) + (nt * 16) + vAddr.fY; }
/* Increment the scroll by one pixel */
inline void h_scroll() { if (!rendering()) return; if (vAddr.cX == 31) vAddr.r ^= 0x41F; else vAddr.cX++; }
inline void v_scroll()
{
    if (!rendering()) return;
    if (vAddr.fY < 7) vAddr.fY++;
    else
    {
        vAddr.fY = 0;
        if      (vAddr.cY == 31)   vAddr.cY = 0;
        else if (vAddr.cY == 29) { vAddr.cY = 0; vAddr.nt ^= 0b10; }
        else                       vAddr.cY++;
    }
}
/* Copy scrolling data from loopy T to loopy V */
inline void h_update() { if (!rendering()) return; vAddr.r = (vAddr.r & ~0x041F) | (tAddr.r & 0x041F); }
inline void v_update() { if (!rendering()) return; vAddr.r = (vAddr.r & ~0x7BE0) | (tAddr.r & 0x7BE0); }
/* Put new data into the shift registers */
inline void reload_shift()
{
    bgShiftL = (bgShiftL & 0xFF00) | bgL;
    bgShiftH = (bgShiftH & 0xFF00) | bgH;

    atLatchL = (at & 1);
    atLatchH = (at & 2);
}

/* Clear secondary OAM */
void clear_oam()
{
    for (int i = 0; i < 8; i++)
    {
        secOam[i].id    = 64;
        secOam[i].y     = 0xFF;
        secOam[i].tile  = 0xFF;
        secOam[i].attr  = 0xFF;
        secOam[i].x     = 0xFF;
        secOam[i].dataL = 0;
        secOam[i].dataH = 0;
    }
}

/* Fill secondary OAM with the sprite infos for the next scanline */
void eval_sprites()
{
    int n = 0;
    int m = 0;  // Sub-index for buggy overflow evaluation
    bool overflow_bug_active = false;
    int overflow_start_sprite = 0;

    for (int i = 0; i < 64; i++)
    {
        if (!overflow_bug_active)
        {
            int line = (scanline == 261 ? -1 : scanline) - oamMem[i*4 + 0];
            // If the sprite is in the scanline, copy its properties into secondary OAM:
            if (line >= 0 and line < spr_height())
            {
                if (n < 8)
                {
                    secOam[n].id   = i;
                    secOam[n].y    = oamMem[i*4 + 0];
                    secOam[n].tile = oamMem[i*4 + 1];
                    secOam[n].attr = oamMem[i*4 + 2];
                    secOam[n].x    = oamMem[i*4 + 3];
                    n++;
                }
                else
                {
                    // Found 9th sprite - set overflow flag and start buggy evaluation
                    status.sprOvf = true;
                    overflow_bug_active = true;
                    overflow_start_sprite = i;
                    m = 0;  // Start with Y coordinate check
                }
            }
        }
        else
        {
            // Buggy sprite overflow evaluation
            // The hardware bug: PPU continues checking but uses incorrect addressing
            int addr = (i * 4 + m) & 0xFF;
            int line = (scanline == 261 ? -1 : scanline) - oamMem[addr];

            if (line >= 0 && line < spr_height())
            {
                status.sprOvf = true;
                m = (m + 1) & 3;  // Increment byte index (wraps 0-3)
                // In real hardware, both sprite and byte indices increment incorrectly
            }
            else
            {
                // Incorrectly increment both indices when sprite not in range
                m = (m + 1) & 3;
                // The PPU increments sprite index when m wraps to 0
                if (m == 0)
                {
                    // Hardware increments sprite index here
                    // But due to the bug, it might skip sprites
                }
            }
        }
    }
}

/* Load the sprite info into primary OAM and fetch their tile data. */
void load_sprites()
{
    u16 addr;
    for (int i = 0; i < 8; i++)
    {
        oam[i] = secOam[i];  // Copy secondary OAM into primary.

        // Different address modes depending on the sprite height:
        if (spr_height() == 16)
            addr = ((oam[i].tile & 1) * 0x1000) + ((oam[i].tile & ~1) * 16);
        else
            addr = ( ctrl.sprTbl      * 0x1000) + ( oam[i].tile       * 16);

        unsigned sprY = (scanline - oam[i].y) % spr_height();  // Line inside the sprite.
        if (oam[i].attr & 0x80) sprY ^= spr_height() - 1;      // Vertical flip.
        addr += sprY + (sprY & 8);  // Select the second tile if on 8x16.

        oam[i].dataL = rd(addr + 0);
        oam[i].dataH = rd(addr + 8);
    }
}

/* Process a pixel, draw it if it's on screen */
void pixel()
{
    u8 palette = 0, objPalette = 0;
    bool objPriority = 0;
    int x = dot - 2;

    if (scanline < 240 and x >= 0 and x < 256)
    {
        if (mask.bg and not (!mask.bgLeft && x < 8))
        {
            // Background:
            palette = (NTH_BIT(bgShiftH, 15 - fX) << 1) |
                       NTH_BIT(bgShiftL, 15 - fX);
            if (palette)
                palette |= ((NTH_BIT(atShiftH,  7 - fX) << 1) |
                             NTH_BIT(atShiftL,  7 - fX))      << 2;
        }
        // Sprites:
        if (mask.spr and not (!mask.sprLeft && x < 8))
            for (int i = 7; i >= 0; i--)
            {
                if (oam[i].id == 64) continue;  // Void entry.
                unsigned sprX = x - oam[i].x;
                if (sprX >= 8) continue;            // Not in range.
                if (oam[i].attr & 0x40) sprX ^= 7;  // Horizontal flip.

                u8 sprPalette = (NTH_BIT(oam[i].dataH, 7 - sprX) << 1) |
                                 NTH_BIT(oam[i].dataL, 7 - sprX);
                if (sprPalette == 0) continue;  // Transparent pixel.

                // Check for sprite 0 hit
                if (oam[i].id == 0 && palette && x != 255) {
                    // Don't set sprite 0 hit if sprites or BG are masked at x < 8
                    if (x >= 8 || (mask.sprLeft && mask.bgLeft)) {
                        status.sprHit = true;
                    }
                }
                sprPalette |= (oam[i].attr & 3) << 2;
                objPalette  = sprPalette + 16;
                objPriority = oam[i].attr & 0x20;
            }
        // Evaluate priority:
        if (objPalette && (palette == 0 || objPriority == 0)) palette = objPalette;

        // Apply emphasis bits from mask register (bits 5-7)
        u8 emphasis = (mask.red << 0) | (mask.green << 1) | (mask.blue << 2);
        if (emphasis == 0) {
            // No emphasis - use base palette directly for compatibility
            pixels[scanline*256 + x] = basePalette[rd(0x3F00 + (rendering() ? palette : 0))];
        } else {
            // Use precomputed emphasis palette
            pixels[scanline*256 + x] = nesRgb[emphasis][rd(0x3F00 + (rendering() ? palette : 0))];
        }
    }
    // Perform background shifts:
    bgShiftL <<= 1; bgShiftH <<= 1;
    atShiftL = (atShiftL << 1) | atLatchL;
    atShiftH = (atShiftH << 1) | atLatchH;
}

/* Execute a cycle of a scanline */
template<Scanline s> void scanline_cycle()
{
    static u16 addr;

    if (s == NMI and dot == 1) {
        if (!vBlankSuppressed) {
            status.vBlank = true;
            // Update NMI line to reflect ctrl.nmi && status.vBlank
            CPU::set_nmi(ctrl.nmi && status.vBlank);
        }
        vBlankSuppressed = false;  // Clear suppression flag after the critical cycle
    }
    else if (s == POST and dot == 0) GUI::new_frame(pixels);
    else if (s == VISIBLE or s == PRE)
    {
        // Sprites:
        switch (dot)
        {
            case   1: clear_oam(); if (s == PRE) { status.sprOvf = status.sprHit = false; } break;
            case 257: if (rendering()) eval_sprites(); break;
            case 321: if (rendering()) load_sprites(); break;
        }
        // Background:
        switch (dot)
        {
            case 2 ... 255: case 322 ... 337:
                pixel();
                switch (dot % 8)
                {
                    // Nametable:
                    case 1:  addr  = nt_addr(); reload_shift(); break;
                    case 2:  nt    = rd(addr);  break;
                    // Attribute:
                    case 3:  addr  = at_addr(); break;
                    case 4:  at    = rd(addr);  if (vAddr.cY & 2) at >>= 4;
                                                if (vAddr.cX & 2) at >>= 2; break;
                    // Background (low bits):
                    case 5:  addr  = bg_addr(); break;
                    case 6:  bgL   = rd(addr);  break;
                    // Background (high bits):
                    case 7:  addr += 8;         break;
                    case 0:  bgH   = rd(addr); h_scroll(); break;
                } break;
            case         256:  pixel(); bgH = rd(addr); v_scroll(); break;  // Vertical bump.
            case         257:  pixel(); reload_shift(); h_update(); break;  // Update horizontal position.
            case 280 ... 304:  if (s == PRE)            v_update(); break;  // Update vertical position.

            // No shift reloading:
            case             1:  addr = nt_addr();
                                 if (s == PRE) {
                                     status.vBlank = false;
                                     // Update NMI line: VBlank cleared, so line = ctrl.nmi && false = false
                                     CPU::set_nmi(ctrl.nmi && status.vBlank);
                                 }
                                 break;
            case 321: case 339:  addr = nt_addr(); break;
            // Nametable fetch instead of attribute:
            case           338:  nt = rd(addr); break;
            case           340:  nt = rd(addr); if (s == PRE && rendering() && frameOdd) dot++;
        }
    }
}

/* Execute a PPU cycle. */
void step()
{
    // Decrement warmup counter (PPU step is called 3 times per CPU cycle)
    static int ppuStepCounter = 0;
    if (warmupCycles > 0) {
        if (++ppuStepCounter >= 3) {
            ppuStepCounter = 0;
            warmupCycles--;
        }
    }

    switch (scanline)
    {
        case 0 ... 239:  scanline_cycle<VISIBLE>(); break;
        case       240:  scanline_cycle<POST>();    break;
        case       241:  scanline_cycle<NMI>();     break;
        case       261:  scanline_cycle<PRE>();     break;
    }

    // Signal scanline to mapper at dot 260 (after sprite fetches, when A12 toggles for MMC3)
    if (dot == 260) Cartridge::signal_scanline(scanline);

    // Update dot and scanline counters:
    if (++dot > 340)
    {
        dot %= 341;
        if (++scanline > 261)
        {
            scanline = 0;
            frameOdd ^= 1;
        }
    }
}

void reset()
{
    frameOdd = false;
    scanline = dot = 0;
    ctrl.r = mask.r = status.r = 0;
    latch = 0;  // Reset the write latch
    readBuffer = 0;
    warmupCycles = 29658;  // PPU warmup period in CPU cycles

    memset(pixels, 0x00, sizeof(pixels));
    memset(ciRam,  0xFF, sizeof(ciRam));
    memset(oamMem, 0x00, sizeof(oamMem));

    // Precompute all emphasis combinations
    for (int emp = 0; emp < 8; emp++) {
        for (int i = 0; i < 64; i++) {
            u32 col = basePalette[i];
            
            if (emp == 0) {
                // No emphasis - use original colors exactly
                nesRgb[emp][i] = col;
            } else {
                // Apply emphasis: attenuate non-emphasized channels
                u8 r = (col >> 16) & 0xFF;
                u8 g = (col >> 8) & 0xFF;
                u8 b = col & 0xFF;
                
                if (!(emp & 1)) r = r * 3 / 4;  // If red not emphasized, attenuate it
                if (!(emp & 2)) g = g * 3 / 4;  // If green not emphasized, attenuate it  
                if (!(emp & 4)) b = b * 3 / 4;  // If blue not emphasized, attenuate it
                
                nesRgb[emp][i] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

void get_state(PpuState& state) {
    state.vram_addr = vAddr.r;
    state.temp_addr = tAddr.r;
    state.fine_x = fX;
    state.oam_addr = oamAddr;
    state.read_buffer = readBuffer;
    state.open_bus = openBus;
    state.write_latch = latch;
    state.frame_odd = frameOdd;
}

void set_state(const PpuState& state) {
    vAddr.r = state.vram_addr;
    tAddr.r = state.temp_addr;
    fX = state.fine_x;
    oamAddr = state.oam_addr;
    readBuffer = state.read_buffer;
    openBus = state.open_bus;
    latch = state.write_latch;
    frameOdd = state.frame_odd;
}


}
