#pragma once
#include "common.hpp"
#include <Nes_Apu.h>
#include <apu_snapshot.h>
#include <string>

namespace SaveState {

// Magic number "LNES" for LaiNES save states
const u32 MAGIC = 0x534e454c;  // "LNES" in little-endian
const u32 VERSION = 1;

// Main save state structure
struct State {
    u32 magic;
    u32 version;

    // CPU state
    u8 cpu_ram[0x800];
    u8 cpu_a, cpu_x, cpu_y, cpu_s;
    u16 cpu_pc;
    u8 cpu_flags;

    // PPU state
    u8 ppu_ram[0x800];
    u8 ppu_palette[0x20];
    u8 ppu_oam[0x100];

    // PPU internal registers
    int ppu_scanline;
    int ppu_dot;
    u8 ppu_ctrl;
    u8 ppu_mask;
    u8 ppu_status;
    u16 ppu_vram_addr;
    u16 ppu_temp_addr;
    u8 ppu_fine_x;
    u8 ppu_write_latch;
    u8 ppu_read_buffer;

    // APU state
    apu_snapshot_t apu;

    // Mapper metadata
    u8 mapper_id;
    u32 prg_ram_size;
    u32 chr_ram_size;

    // Variable-size mapper data follows after this structure
    // PRG RAM, CHR RAM (if applicable), and mapper-specific state
};

// Save state to file
bool save(const char* filename);

// Load state from file
bool load(const char* filename);

// Get the default save state filename based on ROM name
std::string get_default_filename();

}
