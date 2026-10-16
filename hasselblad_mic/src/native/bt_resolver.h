/* Version-independent discovery of the audited ABI pattern family. This is
 * deliberately conservative: missing, changed or ambiguous patterns fail.
 * Never use a known-version offset as a fallback after discovery fails. */
#ifndef MIC_BT_RESOLVER_H
#define MIC_BT_RESOLVER_H
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bt_signatures.h"
struct mic_bt_abi { uintptr_t offset[MIC_ABI_COUNT]; uintptr_t inited, dg_table, clock_latch; };
static uint32_t mic_code_mask(uint32_t w)
{
    return (w & 0x9f000000u) == 0x90000000u ? 0x9f00001fu : 0xffffffffu;
}
static uintptr_t mic_adrp_target(uint32_t instruction, uintptr_t pc)
{
    int64_t delta = (((instruction >> 5) & 0x7ffff) << 2) | ((instruction >> 29) & 3);
    if (delta & (1 << 20)) delta -= 1 << 21;
    return (uintptr_t)((int64_t)(pc & ~(uintptr_t)4095) + delta * 4096);
}
static int mic_abi_load(const char *path, struct mic_bt_abi *out)
{
    int result = -1, fd = open(path, O_RDONLY | O_CLOEXEC); struct stat st;
    unsigned char *bytes = NULL; Elf64_Ehdr eh; Elf64_Phdr ph[32];
    memset(out, 0, sizeof *out);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof eh || st.st_size > 16 * 1024 * 1024) goto done;
    bytes = malloc((size_t)st.st_size); if (!bytes) goto done;
    if (pread(fd, bytes, (size_t)st.st_size, 0) != st.st_size) goto done;
    memcpy(&eh, bytes, sizeof eh);
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 || eh.e_type != ET_DYN ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || !eh.e_phnum || eh.e_phnum > 32 ||
        eh.e_phoff > (uint64_t)st.st_size || eh.e_phnum * sizeof(Elf64_Phdr) > (uint64_t)st.st_size - eh.e_phoff) goto done;
    memcpy(ph, bytes + eh.e_phoff, eh.e_phnum * sizeof(Elf64_Phdr));
    for (int j = 0; j < eh.e_phnum; ++j)
        if (ph[j].p_offset > (uint64_t)st.st_size || ph[j].p_filesz > (uint64_t)st.st_size - ph[j].p_offset ||
            ph[j].p_vaddr > 0x100000000ull || ph[j].p_memsz > 64 * 1024 * 1024 ||
            ph[j].p_vaddr > UINTPTR_MAX - ph[j].p_memsz) goto done;
    uintptr_t file_offsets[MIC_ABI_COUNT] = {0};
    for (unsigned a = 0; a < MIC_ABI_COUNT; ++a) {
        const struct mic_abi_signature *sig = &mic_signatures[a]; unsigned hits = 0;
        for (int j = 0; j < eh.e_phnum; ++j) {
            if (ph[j].p_type != PT_LOAD || !(ph[j].p_flags & PF_X)) continue;
            for (uint64_t p = 0; p + sig->count * 4 <= ph[j].p_filesz; p += 4) {
                unsigned k;
                for (k = 0; k < sig->count; ++k) {
                    uint32_t w; memcpy(&w, bytes + ph[j].p_offset + p + 4 * k, 4);
                    uint32_t mask = mic_code_mask(sig->words[k]);
                    if ((w & mask) != (sig->words[k] & mask)) break;
                }
                if (k != sig->count) continue;
                ++hits; out->offset[a] = ph[j].p_vaddr + p; file_offsets[a] = ph[j].p_offset + p;
            }
        }
        if (hits != 1) { result = -(int)a - 10; goto done; }
    }
    uint32_t adrp, use, call;
    memcpy(&call, bytes + file_offsets[MIC_ABI_INIT_FLAG], 4);
    int64_t branch_delta = (int64_t)(call & 0x03ffffff) * 4;
    if (branch_delta & (1 << 27)) branch_delta -= 1 << 28;
    if ((uintptr_t)((int64_t)out->offset[MIC_ABI_INIT_FLAG] + branch_delta) != out->offset[MIC_ABI_APP_INIT]) goto done;
    memcpy(&adrp, bytes + file_offsets[MIC_ABI_INIT_FLAG] + 4, 4);
    memcpy(&use, bytes + file_offsets[MIC_ABI_INIT_FLAG] + 16, 4);
    out->inited = mic_adrp_target(adrp, out->offset[MIC_ABI_INIT_FLAG] + 4) + ((use >> 10) & 4095);
    memcpy(&adrp, bytes + file_offsets[MIC_ABI_DG_CLOSE] + 0x38, 4);
    memcpy(&use, bytes + file_offsets[MIC_ABI_DG_CLOSE] + 0x44, 4);
    out->dg_table = mic_adrp_target(adrp, out->offset[MIC_ABI_DG_CLOSE] + 0x38) + ((use >> 10) & 4095);
    memcpy(&adrp, bytes + file_offsets[MIC_ABI_CLOCK_RECV] + 0x38c, 4);
    memcpy(&use, bytes + file_offsets[MIC_ABI_CLOCK_RECV] + 0x390, 4);
    out->clock_latch = mic_adrp_target(adrp, out->offset[MIC_ABI_CLOCK_RECV] + 0x38c) + (((use >> 10) & 4095) * 8);
    /* The request writer must address the same object as the response reader. */
    memcpy(&adrp, bytes + file_offsets[MIC_ABI_CLOCK_RECV] + 0x34c, 4);
    memcpy(&use, bytes + file_offsets[MIC_ABI_CLOCK_RECV] + 0x350, 4);
    if (out->clock_latch != mic_adrp_target(adrp, out->offset[MIC_ABI_CLOCK_RECV] + 0x34c) + ((use >> 10) & 4095)) goto done;
    int flag_ok = 0, table_ok = 0, clock_ok = 0;
    for (int j = 0; j < eh.e_phnum; ++j) {
        if (ph[j].p_type != PT_LOAD || !(ph[j].p_flags & PF_W)) continue;
        uintptr_t lo = ph[j].p_vaddr, hi = lo + ph[j].p_memsz;
        if (out->inited >= lo && out->inited < hi) flag_ok = 1;
        if (out->dg_table >= lo && out->dg_table <= hi && hi - out->dg_table >= 20 * 32) table_ok = 1;
        if (!(out->clock_latch & 7) && out->clock_latch >= lo && out->clock_latch <= hi && hi - out->clock_latch >= 8) clock_ok = 1;
    }
    if (!flag_ok || !table_ok || !clock_ok) goto done;
    result = 0;
done:
    if (result) memset(out, 0, sizeof *out);
    free(bytes); close(fd); return result;
}
#endif
