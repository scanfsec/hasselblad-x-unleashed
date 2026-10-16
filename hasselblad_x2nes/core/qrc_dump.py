#!/usr/bin/env python3
"""Dump ALL embedded qrc (Qt resource) trees from camera-gui into core/qrc_dump/.

Format (empirically verified against all 346 files of the main qml tree; node
stride and dir semantics per rcc2.py / REPORT_O1):
  - Registration: _GLOBAL__sub_I_qrc_*.cpp ctors call qRegisterResourceData(3, TREE, NAMES, DATA)
    (three absolute .rodata addresses) -- enumerated by scanning .text for BL to
    qRegisterResourceData and decoding the ADRP/ADD argument setup.
  - TREE region: 22-byte nodes, all big-endian:
      @0  u32 name_offset
      @4  u16 flags (bit1=Directory 0x2, bit0=zlib 0x1)  [NOTE: differs from
          upstream Qt where Directory=0x01 -- verify per build]
      @6  u32 Dir:childcount | File:(constant 1, unused)
      @10 u32 Dir:first_child | File:payload_offset_into_DATA   <-- verified
      @14 u32 timestamp  @18 u32 checksum
  - NAMES region: entry = u16 len | u32 hash | UTF-16BE name
  - DATA payload entry at DATA+payload_off (verified all 346 files of main tree):
      plain:    [u32 size][data]
      zlib:     [u32 comp_size][u32 unc_size][zlib stream]

Path note: node 0's name duplicates the first path component (rcc mount root);
the live URL is  qrc:/<path minus first component>  -- we strip it on output.
"""
import io, os, re, struct, sys, zlib, json
from elftools.elf.elffile import ELFFile

BIN = r"C:\Users\tom\Documents\test\system_decrypted\bin\camera-gui"
OUT = r"C:\Users\tom\Documents\test\x2nes\core\qrc_dump"
QREG = 0xE90980  # qRegisterResourceData

data = open(BIN, "rb").read()
elf = ELFFile(io.BytesIO(data))

def v2o(addr):
    for s in elf.iter_sections():
        if s["sh_type"] != "SHT_NOBITS" and s["sh_addr"] <= addr < s["sh_addr"] + s["sh_size"]:
            return s["sh_offset"] + addr - s["sh_addr"]
    return None

text = elf.get_section_by_name(".text")
taddr, toff, tsize = text["sh_addr"], text["sh_offset"], text["sh_size"]
tcode = data[toff:toff + tsize]

def simm(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m

# ---- 1. enumerate qRegisterResourceData call sites + argument pointers ----
calls = []
for i in range(0, tsize - 4, 4):
    op = struct.unpack_from("<I", tcode, i)[0]
    if (op & 0xFC000000) == 0x94000000:
        tgt = taddr + i + (simm(op & 0x3FFFFFF, 26) << 2)
        if tgt == QREG:
            calls.append(i)

def decode_callsite(i, window=48):
    regs = {}
    for j in range(max(0, i - window * 4), i, 4):
        op = struct.unpack_from("<I", tcode, j)[0]
        pc = taddr + j
        if (op & 0x9F000000) == 0x90000000:          # ADRP Xd, imm
            rd = op & 31
            immlo = (op >> 29) & 3
            immhi = (op >> 5) & 0x7FFFF
            imm = simm((immhi << 2) | immlo, 21) << 12
            regs[rd] = (pc & ~0xFFF) + imm
        elif (op & 0xFFC00000) == 0x91000000:        # ADD Xd, Xn, #imm(,LSL#12)
            rd, rn = op & 31, (op >> 5) & 31
            imm12 = (op >> 10) & 0xFFF
            sh = (op >> 22) & 1
            if rn in regs and rd != rn:
                regs[rd] = regs[rn] + (imm12 << (12 if sh else 0))
            elif rn in regs and rd == rn:
                regs[rd] = regs[rn] + (imm12 << (12 if sh else 0))
    return regs

trees = {}
for c in calls:
    r = decode_callsite(c)
    if all(k in r for k in (1, 2, 3)):
        trees[(r[1], r[2], r[3])] = None

# ---- 2. walk each tree, extract payloads ----
manifest = {}
n_files = n_err = 0

for (TREE, NAMES, DATA) in sorted(trees):
    tree_off = v2o(TREE)
    names_blob = data[v2o(NAMES):v2o(DATA)]
    data_off = v2o(DATA)

    def nm(o):
        ln, = struct.unpack_from(">H", names_blob, o)
        return names_blob[o + 6:o + 6 + ln * 2].decode("utf-16-be", errors="replace")

    results = []

    def walk(i, path):
        if i * 22 + 22 > 0x400000:  # sanity cap
            return
        b = data[tree_off + 22 * i: tree_off + 22 * i + 22]
        if len(b) < 22:
            return
        name_o, flags = struct.unpack_from(">IH", b, 0)
        childcount, f2 = struct.unpack_from(">II", b, 6)  # f2@10: dir first_child / file payload_off
        try:
            n = nm(name_o)
        except Exception:
            return
        if flags & 2:  # directory (bit1)
            for k in range(childcount):
                walk(f2 + k, path + [n])
        else:
            results.append((path + [n], flags, f2))

    try:
        walk(0, [])
    except Exception as e:
        print(f"tree {TREE:#x} walk failed: {e}")
        continue

    tdir = os.path.join(OUT, f"t{TREE:08x}")
    for path, flags, poff in results:
        # strip duplicated first component (rcc mount root), see docstring
        comps = path[1:] if len(path) > 1 and path[0] == path[1] else path
        rel = "/".join(c for c in comps if c)
        if not rel:
            continue
        entry = data[data_off + poff: data_off + poff + 8]
        if len(entry) < 8:
            n_err += 1
            continue
        try:
            if flags & 1:  # zlib (bit0)
                comp, unc = struct.unpack_from(">II", entry, 0)
                raw = zlib.decompress(data[data_off + poff + 8: data_off + poff + 8 + comp])
                if len(raw) != unc:
                    n_err += 1
                    continue
                comp_flag = 1
            else:
                sz, = struct.unpack_from(">I", entry, 0)
                raw = data[data_off + poff + 4: data_off + poff + 4 + sz]
                comp_flag = 0
        except Exception:
            n_err += 1
            continue
        fp = os.path.join(OUT, f"t{TREE:08x}", *rel.split("/"))
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "wb") as fh:
            fh.write(raw)
        manifest[rel] = {
            "tree": f"t{TREE:08x}",
            "payload_off": poff,
            "size": len(raw),
            "zlib": comp_flag,
        }
        n_files += 1

with open(os.path.join(OUT, "manifest.json"), "w") as fh:
    json.dump(manifest, fh, indent=1, sort_keys=True)

print(f"trees={len(trees)} files_written={n_files} errors={n_err}")
print(f"manifest: {os.path.join(OUT, 'manifest.json')}")
