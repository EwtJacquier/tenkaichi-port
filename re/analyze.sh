#!/usr/bin/env bash
# Import EBOOT_decrypted.elf into a new Ghidra project (PSP loader at 0x08804000, Allegrex), analyze it
# and export the program (functions, jump tables) for the recompiler. Once: later runs use
# port/tools/rebuild.sh.
set -euo pipefail
cd "$(dirname "$0")"
# GHIDRA (Ghidra 12.2 folder) and JAVA_HOME (a JDK 21+) from the environment, else ~/tools/
export JAVA_HOME=${JAVA_HOME:-$HOME/tools/jdk-25.0.5+8}
export PATH=$JAVA_HOME/bin:$PATH GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G}
H=${GHIDRA:-$HOME/tools/ghidra_12.2_PUBLIC}/support/analyzeHeadless
"$H" "$PWD/ghidra_project" dbztag -import "$PWD/../EBOOT_decrypted.elf" -loader PspElfLoader -loader-imagebase 08804000 \
	-scriptPath "$PWD/ghidra_scripts" -postScript ExportProgram.java "$PWD/export/program.json" > export/analyze.log 2>&1
grep -E "Using Loader|Language|ExportProgram.java>|ERROR|Analysis succeeded|Total Time" export/analyze.log | awk 'NR <= 20'
