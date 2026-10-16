#include "savestate.hpp"
#include "cpu.hpp"
#include "ppu.hpp"
#include "apu.hpp"
#include "cartridge.hpp"
#include "mapper.hpp"
#include <cstdio>
#include <cstring>
#include <iostream>

namespace SaveState {

std::string get_default_filename() {
    std::string rom_path = Cartridge::get_rom_path();
    if (rom_path.empty()) {
        return "";
    }

    // Replace .nes extension with .lns (LaiNES Save)
    size_t dot_pos = rom_path.rfind('.');
    if (dot_pos != std::string::npos) {
        return rom_path.substr(0, dot_pos) + ".lns";
    }
    return rom_path + ".lns";
}

bool save(const char* filename) {
    if (!Cartridge::loaded()) {
        std::cerr << "Cannot save state: no ROM loaded\n";
        return false;
    }

    Mapper* mapper = Cartridge::get_mapper();
    if (!mapper) {
        std::cerr << "Cannot save state: mapper not initialized\n";
        return false;
    }

    FILE* f = fopen(filename, "wb");
    if (!f) {
        std::cerr << "Failed to create save state file: " << filename << "\n";
        return false;
    }

    // Prepare the main state structure
    State state;
    memset(&state, 0, sizeof(State));

    state.magic = MAGIC;
    state.version = VERSION;

    // CPU state
    memcpy(state.cpu_ram, CPU::ram, sizeof(CPU::ram));
    state.cpu_a = CPU::A;
    state.cpu_x = CPU::X;
    state.cpu_y = CPU::Y;
    state.cpu_s = CPU::S;
    state.cpu_pc = CPU::PC;
    state.cpu_flags = CPU::P.get();

    // PPU state
    memcpy(state.ppu_ram, PPU::ciRam, sizeof(PPU::ciRam));
    memcpy(state.ppu_palette, PPU::cgRam, sizeof(PPU::cgRam));
    memcpy(state.ppu_oam, PPU::oamMem, sizeof(PPU::oamMem));

    // PPU internal registers
    PPU::PpuState ppu_state;
    PPU::get_state(ppu_state);
    state.ppu_scanline = PPU::scanline;
    state.ppu_dot = PPU::dot;
    state.ppu_ctrl = PPU::ctrl.r;
    state.ppu_mask = PPU::mask.r;
    state.ppu_status = PPU::status.r;
    state.ppu_vram_addr = ppu_state.vram_addr;
    state.ppu_temp_addr = ppu_state.temp_addr;
    state.ppu_fine_x = ppu_state.fine_x;
    state.ppu_write_latch = ppu_state.write_latch ? 1 : 0;
    state.ppu_read_buffer = ppu_state.read_buffer;

    // APU state
    APU::get_apu().save_snapshot(&state.apu);

    // Mapper metadata
    state.mapper_id = Cartridge::get_mapper_id();
    state.prg_ram_size = mapper->get_prg_ram_size();
    state.chr_ram_size = mapper->get_chr_ram_size();

    // Write main state structure
    if (fwrite(&state, sizeof(State), 1, f) != 1) {
        std::cerr << "Failed to write save state header\n";
        fclose(f);
        return false;
    }

    // Write PRG RAM
    if (state.prg_ram_size > 0) {
        u8* prg_ram = mapper->get_prg_ram();
        if (prg_ram && fwrite(prg_ram, state.prg_ram_size, 1, f) != 1) {
            std::cerr << "Failed to write PRG RAM\n";
            fclose(f);
            return false;
        }
    }

    // Write CHR RAM (if applicable)
    if (state.chr_ram_size > 0) {
        u8* chr_ram = mapper->get_chr_ram();
        if (chr_ram && fwrite(chr_ram, state.chr_ram_size, 1, f) != 1) {
            std::cerr << "Failed to write CHR RAM\n";
            fclose(f);
            return false;
        }
    }

    // Write mapper-specific state
    u32 mapper_state_size = mapper->get_state_size();
    if (fwrite(&mapper_state_size, sizeof(u32), 1, f) != 1) {
        std::cerr << "Failed to write mapper state size\n";
        fclose(f);
        return false;
    }

    if (mapper_state_size > 0) {
        u8* mapper_state_buffer = new u8[mapper_state_size];
        mapper->save_state(mapper_state_buffer);

        if (fwrite(mapper_state_buffer, mapper_state_size, 1, f) != 1) {
            std::cerr << "Failed to write mapper state\n";
            delete[] mapper_state_buffer;
            fclose(f);
            return false;
        }
        delete[] mapper_state_buffer;
    }

    fclose(f);
    std::cout << "Save state created: " << filename << "\n";
    return true;
}

bool load(const char* filename) {
    if (!Cartridge::loaded()) {
        std::cerr << "Cannot load state: no ROM loaded\n";
        return false;
    }

    Mapper* mapper = Cartridge::get_mapper();
    if (!mapper) {
        std::cerr << "Cannot load state: mapper not initialized\n";
        return false;
    }

    FILE* f = fopen(filename, "rb");
    if (!f) {
        std::cerr << "Failed to open save state file: " << filename << "\n";
        return false;
    }

    // Read main state structure
    State state;
    if (fread(&state, sizeof(State), 1, f) != 1) {
        std::cerr << "Failed to read save state header\n";
        fclose(f);
        return false;
    }

    // Verify magic number
    if (state.magic != MAGIC) {
        std::cerr << "Invalid save state file (bad magic number)\n";
        fclose(f);
        return false;
    }

    // Check version
    if (state.version != VERSION) {
        std::cerr << "Incompatible save state version\n";
        fclose(f);
        return false;
    }

    // Verify mapper ID matches
    if (state.mapper_id != Cartridge::get_mapper_id()) {
        std::cerr << "Save state mapper mismatch (expected "
                  << (int)Cartridge::get_mapper_id() << ", got "
                  << (int)state.mapper_id << ")\n";
        fclose(f);
        return false;
    }

    // Restore CPU state
    memcpy(CPU::ram, state.cpu_ram, sizeof(CPU::ram));
    CPU::A = state.cpu_a;
    CPU::X = state.cpu_x;
    CPU::Y = state.cpu_y;
    CPU::S = state.cpu_s;
    CPU::PC = state.cpu_pc;
    CPU::P.set(state.cpu_flags);

    // Restore PPU state
    memcpy(PPU::ciRam, state.ppu_ram, sizeof(PPU::ciRam));
    memcpy(PPU::cgRam, state.ppu_palette, sizeof(PPU::cgRam));
    memcpy(PPU::oamMem, state.ppu_oam, sizeof(PPU::oamMem));

    // Restore PPU internal registers
    PPU::scanline = state.ppu_scanline;
    PPU::dot = state.ppu_dot;
    PPU::ctrl.r = state.ppu_ctrl;
    PPU::mask.r = state.ppu_mask;
    PPU::status.r = state.ppu_status;

    PPU::PpuState ppu_state;
    ppu_state.vram_addr = state.ppu_vram_addr;
    ppu_state.temp_addr = state.ppu_temp_addr;
    ppu_state.fine_x = state.ppu_fine_x;
    ppu_state.write_latch = state.ppu_write_latch != 0;
    ppu_state.read_buffer = state.ppu_read_buffer;
    ppu_state.open_bus = 0;  // Reset open bus
    ppu_state.frame_odd = false;  // Will be set by next frame
    PPU::set_state(ppu_state);

    // Restore APU state
    APU::get_apu().load_snapshot(state.apu);

    // Read PRG RAM
    if (state.prg_ram_size > 0) {
        u8* prg_ram = mapper->get_prg_ram();
        if (prg_ram && fread(prg_ram, state.prg_ram_size, 1, f) != 1) {
            std::cerr << "Failed to read PRG RAM\n";
            fclose(f);
            return false;
        }
    }

    // Read CHR RAM (if applicable)
    if (state.chr_ram_size > 0) {
        u8* chr_ram = mapper->get_chr_ram();
        if (chr_ram && fread(chr_ram, state.chr_ram_size, 1, f) != 1) {
            std::cerr << "Failed to read CHR RAM\n";
            fclose(f);
            return false;
        }
    }

    // Read mapper-specific state
    u32 mapper_state_size;
    if (fread(&mapper_state_size, sizeof(u32), 1, f) != 1) {
        std::cerr << "Failed to read mapper state size\n";
        fclose(f);
        return false;
    }

    if (mapper_state_size > 0) {
        u8* mapper_state_buffer = new u8[mapper_state_size];

        if (fread(mapper_state_buffer, mapper_state_size, 1, f) != 1) {
            std::cerr << "Failed to read mapper state\n";
            delete[] mapper_state_buffer;
            fclose(f);
            return false;
        }

        mapper->load_state(mapper_state_buffer);
        delete[] mapper_state_buffer;
    }

    fclose(f);
    std::cout << "Save state loaded: " << filename << "\n";
    return true;
}

}
