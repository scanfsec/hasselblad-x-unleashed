#pragma once
#include "common.hpp"
#include <string>

class Mapper;  // Forward declaration

namespace Cartridge {


template <bool wr> u8     access(u16 addr, u8 v = 0);
template <bool wr> u8 chr_access(u16 addr, u8 v = 0);
void signal_scanline(int scanline = -1);  // Default parameter for compatibility
void load(const char* fileName);
void reset();
bool loaded();

// Stubs for mapper features - will be implemented in mapper PRs
bool check_mapper_irq(int elapsed);
bool handles_expansion_addr(u16 addr);
void run_mapper_audio(int elapsed);
void end_mapper_audio_frame(int elapsed);
void ppu_write_hook(u16 addr, u8 v);  // Mapper PPU write observer

// Save state support - get ROM path, mapper ID, and mapper instance
std::string get_rom_path();
u8 get_mapper_id();
Mapper* get_mapper();


}
