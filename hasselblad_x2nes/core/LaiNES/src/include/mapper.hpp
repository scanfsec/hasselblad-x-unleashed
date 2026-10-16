#pragma once
#include <cstring>
#include "common.hpp"


class Mapper
{
    u8* rom;

  protected:
    bool chrRam = false;
    u32 prgMap[4];
    u32 chrMap[8];

    u8 *prg, *chr, *prgRam;
    u32 prgSize, chrSize, prgRamSize;

    template <int pageKBs> void map_prg(int slot, int bank);
    template <int pageKBs> void map_chr(int slot, int bank);

  public:
    Mapper(u8* rom);
    ~Mapper();

    virtual u8 read(u16 addr);
    virtual u8 write(u16 addr, u8 v) { return v; }

    // Returns true if mapper handles CPU addresses in $5000-$5FFF (expansion area)
    // Base implementation returns false (open bus behavior)
    virtual bool handles_expansion_addr(u16 addr) { return false; }

    virtual u8 chr_read(u16 addr);
    virtual u8 chr_write(u16 addr, u8 v);

    virtual void signal_scanline(int scanline) {}

    // Expansion audio support
    virtual bool has_audio() { return false; }
    virtual void run_audio(int elapsed) {}
    virtual void end_audio_frame(int elapsed) {}

    // IRQ support
    virtual bool check_irq(int elapsed) { return false; }
    virtual void ppu_read_hook(u16 addr) {}
    virtual void ppu_write_hook(u16 index, u8 v) {}  // Hook for PPU register writes ($2000-$2007)

    // Save state support
    // Returns size of mapper-specific state data (in bytes)
    virtual u32 get_state_size() const { return 0; }
    // Write mapper-specific state to buffer (PRG RAM and CHR RAM are handled separately)
    virtual void save_state(u8* buffer) const {}
    // Load mapper-specific state from buffer
    virtual void load_state(const u8* buffer) {}

    // Accessors for save state system
    u32 get_prg_ram_size() const { return prgRamSize; }
    u32 get_chr_ram_size() const { return chrRam ? chrSize : 0; }
    u8* get_prg_ram() { return prgRam; }
    u8* get_chr_ram() { return chrRam ? chr : nullptr; }
};
