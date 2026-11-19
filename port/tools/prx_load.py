#!/usr/bin/env python3
"""Load the decrypted EBOOT the way the PSP kernel would: a relocatable module (PRX) at a fixed base
address, or a plain executable (ET_EXEC) at its own addresses.

Applies the PRX relocations (types R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16), then
writes:
  image.bin  - relocated file-backed bytes, starting at BASE (code + data, no .bss)
  meta.json  - entry point, gp, segment layout, .bss range, import stubs, and every code
               address the program stores somewhere (indirect call/jump target candidates)

  python3 prx_load.py EBOOT_decrypted.elf imports.json OUT_DIR
"""
import json
import struct
import sys
from pathlib import Path

BASE = 0x08804000
SHT_PRXRELOC = 0x700000A0
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def main():
    elf_path, imports_path, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    d = elf_path.read_bytes()
    e_entry, e_phoff, e_shoff = struct.unpack_from("<III", d, 0x18)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", d, 0x2A)

    if struct.unpack_from("<H", d, 0x10)[0] == 2:
        return load_exec(d, e_entry, e_phoff, e_phentsize, e_phnum, imports_path, out_dir)
    segs = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _pa, p_filesz, p_memsz = struct.unpack_from("<IIIIII", d, e_phoff + i * e_phentsize)
        if p_type == 1:
            segs.append({"off": p_off, "vaddr": p_vaddr, "filesz": p_filesz, "memsz": p_memsz})

    file_end = max(s["vaddr"] + s["filesz"] for s in segs)
    mem_end = max(s["vaddr"] + s["memsz"] for s in segs)
    img = bytearray(file_end)
    for s in segs:
        img[s["vaddr"]:s["vaddr"] + s["filesz"]] = d[s["off"]:s["off"] + s["filesz"]]

    sh = []
    for i in range(e_shnum):
        sh.append(struct.unpack_from("<IIIIIIIIII", d, e_shoff + i * e_shentsize))
    shstr_off = sh[e_shstrndx][4]

    def sec_name(s):
        n = s[0]
        return d[shstr_off + n:d.index(b"\0", shstr_off + n)].decode()

    sections = {sec_name(s): {"addr": s[3], "size": s[5]} for s in sh}
    text = sections[".text"]

    relocs = []
    for s in sh:
        if s[1] != SHT_PRXRELOC:
            continue
        for j in range(s[4], s[4] + s[5], 8):
            relocs.append(struct.unpack_from("<II", d, j))

    def seg_addr(i):
        return BASE + segs[i]["vaddr"]

    def rd(addr_rel):
        return struct.unpack_from("<I", img, addr_rel)[0]

    def wr(addr_rel, v):
        struct.pack_into("<I", img, addr_rel, v & 0xFFFFFFFF)

    code_ptrs = set()
    text_lo, text_hi = BASE + text["addr"], BASE + text["addr"] + text["size"]
    orig = bytes(img)  # HI16 pairing reads the unrelocated LO16 immediates

    def in_text(a):
        return text_lo <= a < text_hi

    for idx, (r_off, info) in enumerate(relocs):
        rtype, ofs_base, addr_base = info & 0xFF, (info >> 8) & 0xFF, (info >> 16) & 0xFF
        loc = r_off + segs[ofs_base]["vaddr"]  # relative to image start
        to = seg_addr(addr_base)
        op = rd(loc)
        if rtype == R_MIPS_32:
            v = (op + to) & 0xFFFFFFFF
            wr(loc, v)
            if in_text(v):
                code_ptrs.add(v)
        elif rtype == R_MIPS_26:
            target = ((op & 0x03FFFFFF) << 2) + to
            wr(loc, (op & 0xFC000000) | ((target >> 2) & 0x03FFFFFF))
        elif rtype == R_MIPS_HI16:
            lo = None
            for r2_off, info2 in relocs[idx + 1:]:
                t2 = info2 & 0xFF
                if t2 == R_MIPS_HI16:
                    continue
                if t2 == R_MIPS_LO16:
                    lo_loc = r2_off + segs[(info2 >> 8) & 0xFF]["vaddr"]
                    lo = struct.unpack_from("<I", orig, lo_loc)[0] & 0xFFFF
                break
            if lo is None:
                sys.exit(f"HI16 at {BASE + loc:08x} without a following LO16")
            slo = lo - 0x10000 if lo & 0x8000 else lo
            full = ((((orig_hi := struct.unpack_from('<I', orig, loc)[0] & 0xFFFF)) << 16) + slo + to) & 0xFFFFFFFF
            hi = ((full + 0x8000) >> 16) & 0xFFFF
            wr(loc, (op & 0xFFFF0000) | hi)
            if in_text(full):
                code_ptrs.add(full)
        elif rtype == R_MIPS_LO16:
            wr(loc, (op & 0xFFFF0000) | ((op + to) & 0xFFFF))
        else:
            sys.exit(f"unsupported relocation type {rtype}")

    # Module info: name, gp and library tables (already relocated).
    mi = sections[".rodata.sceModuleInfo"]["addr"]
    attr, ver = struct.unpack_from("<HH", img, mi)
    name = img[mi + 4:mi + 32].split(b"\0")[0].decode()
    gp, libent, libent_end, libstub, libstub_end = struct.unpack_from("<IIIII", img, mi + 32)

    imports = json.loads(imports_path.read_text())
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "image.bin").write_bytes(img)
    meta = {
        "base": f"{BASE:08x}",
        "entry": f"{BASE + e_entry:08x}",
        "gp": f"{gp:08x}",
        "module_name": name,
        "module_attr": attr,
        "text": [f"{text_lo:08x}", f"{text_hi:08x}"],
        "image_end": f"{BASE + file_end:08x}",
        "bss": [f"{BASE + segs[-1]['vaddr'] + segs[-1]['filesz']:08x}", f"{BASE + mem_end:08x}"],
        "sections": {k: [f"{BASE + v['addr']:08x}", v["size"]] for k, v in sections.items() if v["addr"] or k == ".text"},
        "imports": [{"addr": i["addr"], "name": i["name"], "lib": i["lib"], "nid": i["nid"]} for i in imports],
        "code_pointers": sorted(f"{a:08x}" for a in code_ptrs),
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=1))
    (out_dir / "meta.h").write_text(  # what the runtime needs to start the program (core.c)
        "// generated by tools/prx_load.py\n"
        f"#define META_IMAGE_END 0x{meta['bss'][0]}u  // .bss starts here\n"
        f"#define META_BSS_END 0x{meta['bss'][1]}u\n"
        f"#define META_ENTRY 0x{meta['entry']}u  // module_start\n")
    print(f"module {name!r} entry={meta['entry']} gp={meta['gp']} image={len(img)} bytes "
          f"bss={meta['bss']} relocs={len(relocs)} code_pointers={len(code_ptrs)}")


def load_exec(d, e_entry, e_phoff, e_phentsize, e_phnum, imports_path, out_dir):
    """A plain executable (ET_EXEC): its segments already at their addresses (from BASE up), no
    relocations and no sections. The module info is at the first segment's p_paddr (a file offset);
    the code runs from the segment's start to the import stubs (.sceStub.text: calls to them are
    the system's functions, never code to translate). Code pointers: aligned words of the data that point into the code."""
    segs = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<IIIIII", d, e_phoff + i * e_phentsize)
        if p_type == 1:
            segs.append({"off": p_off, "vaddr": p_vaddr, "paddr": p_paddr, "filesz": p_filesz, "memsz": p_memsz})
    file_end = max(s["vaddr"] + s["filesz"] for s in segs) - BASE
    mem_end = max(s["vaddr"] + s["memsz"] for s in segs) - BASE
    img = bytearray(file_end)
    for s in segs:
        img[s["vaddr"] - BASE:s["vaddr"] - BASE + s["filesz"]] = d[s["off"]:s["off"] + s["filesz"]]
    s0 = segs[0]
    mi = s0["paddr"] - s0["off"] + s0["vaddr"] - BASE  # relative to the image
    attr, ver = struct.unpack_from("<HH", img, mi)
    name = img[mi + 4:mi + 32].split(b"\0")[0].decode()
    gp, libent, libent_end, libstub, libstub_end = struct.unpack_from("<IIIII", img, mi + 32)
    imports = json.loads(imports_path.read_text())
    text_lo = s0["vaddr"]
    text_hi = min(int(i["addr"], 16) for i in imports)  # the import stubs (.sceStub.text) end the code
    code_ptrs = set()
    for a in range(text_hi - BASE, file_end - 3, 4):
        v = struct.unpack_from("<I", img, a)[0]
        if text_lo <= v < text_hi and not v & 3:
            code_ptrs.add(v)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "image.bin").write_bytes(img)
    meta = {
        "base": f"{BASE:08x}",
        "entry": f"{e_entry:08x}",
        "gp": f"{gp:08x}",
        "module_name": name,
        "module_attr": attr,
        "text": [f"{text_lo:08x}", f"{text_hi:08x}"],
        "image_end": f"{BASE + file_end:08x}",
        "bss": [f"{BASE + file_end:08x}", f"{BASE + mem_end:08x}"],
        "sections": {".text": [f"{text_lo:08x}", text_hi - text_lo], ".rodata.sceModuleInfo": [f"{BASE + mi:08x}", 52],
                     ".lib.ent": [f"{libent:08x}", libent_end - libent], ".lib.stub": [f"{libstub:08x}", libstub_end - libstub]},
        "imports": [{"addr": i["addr"], "name": i["name"], "lib": i["lib"], "nid": i["nid"]} for i in imports],
        "code_pointers": sorted(f"{a:08x}" for a in code_ptrs),
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=1))
    (out_dir / "meta.h").write_text(
        "// generated by tools/prx_load.py\n"
        f"#define META_IMAGE_END 0x{meta['bss'][0]}u  // .bss starts here\n"
        f"#define META_BSS_END 0x{meta['bss'][1]}u\n"
        f"#define META_ENTRY 0x{meta['entry']}u  // the program's entry\n"
        f"#define META_GP 0x{meta['gp']}u  // the module's gp (every thread starts with it)\n")
    print(f"module {name!r} (executable) entry={meta['entry']} gp={meta['gp']} image={len(img)} bytes "
          f"bss={meta['bss']} text={meta['text']} code_pointers={len(code_ptrs)}")


if __name__ == "__main__":
    main()
