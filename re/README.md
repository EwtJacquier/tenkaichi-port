# Dragon Ball Z: Tenkaichi Tag Team (ULUS-10537) — reverse engineering

Everything the port needs from here is already in `export/`: you only need this folder to redo the
analysis, or to look into the game. Addresses are always in the PSP's runtime form (the program
loads at `0x08804040`): the same in Ghidra, in PPSSPP's debugger and in the port (`f_08815610` is
the function at `0x08815610`).

## Files

| Path | What it is |
|---|---|
| `../EBOOT_decrypted.elf` | the decrypted EBOOT.BIN (a plain MIPS/Allegrex executable), yours, never committed |
| `export/program.json` | where every function and jump table of the executable is: what the recompiler reads |
| `export/imports.json` | the PSP system functions the game imports (NID → name, from PPSSPP's tables) |
| `export/overlays/ovN.json` | the same for each code overlay (below), made by `analyze_overlays.sh` |
| `overlays/` | the code overlays taken from your disc by `tools/overlays.py` (not committed) |
| `ghidra_scripts/` | the scripts that made and use the analysis (export, signatures, decompile, cross-references) |
| `ghidra_project/`, `ghidra_project_ov/` | the Ghidra projects the scripts make (not committed) |
| `tools/packfile.py` | lists and extracts the files of `PACKFILE.BIN`, the game's archive |
| `tools/overlays.py` | takes the code overlays out of `PACKFILE.BIN` (and an executable + overlay ELF for Ghidra) |
| `tools/decomp.sh` | prints Ghidra's pseudo-C for the functions at some addresses |
| `tools/gen_imports.py` | makes `imports.json` from the EBOOT and PPSSPP's source tree |
| `tools/prxdec/` | decrypts EBOOT.BIN with PPSSPP's decrypter (`PPSSPP=path/to/ppsspp build.sh`) |

## Code overlays

Much of the game's code is not in the EBOOT: it comes in 11 overlays, ELF files inside
`PACKFILE.BIN` (type 0xffa8, one segment linked at `0x08a814a0`, the space right after the
executable's code and data). The game reads one in at a time and calls into it. The port recompiles
each one apart (`port/tools/recomp.py --overlay`), and the runtime calls the functions of whichever
overlay's bytes are at that address (`port/runtime/overlay.h`).

## Redoing the analysis

It needs [Ghidra](https://ghidra-sre.org) 12.2 with the
[ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex) extension (the PSP's CPU and its
ELF loader) and a JDK 21 or newer. The scripts look for them in `~/tools/` (`ghidra_12.2_PUBLIC`,
`jdk-25.0.5+8`); point them elsewhere with `GHIDRA=...` and `JAVA_HOME=...`.

```bash
re/analyze.sh                       # import + analyze EBOOT_decrypted.elf, export program.json (a few minutes)
python3 -I re/tools/overlays.py "path/to/the.iso" re/overlays
re/analyze_overlays.sh              # each overlay (a few minutes each)
re/tools/decomp.sh 08815610         # Ghidra's pseudo-C of a function (read-only)
```

Ghidra's analysis of this executable is rough (branches into other functions' bodies, calls into
the middle of functions, missed switch tables): the recompiler makes up for it by itself, see
`../docs/NOTES.md`.

## Findings

`../docs/NOTES.md` has the notes: the disc and `PACKFILE.BIN`, the overlays, the online battles'
lockstep protocol and what the netdelay mod changes in it, the renderer fixes.
