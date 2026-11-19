#!/usr/bin/env bash
# Ghidra's analysis of each code overlay (re/overlays/ovN.elf: the executable plus that overlay,
# made by tools/overlays.py), exported as export/overlays/ovN.json (functions, jump tables) for
# the recompiler. A few minutes per overlay; already exported ones are skipped.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=${JAVA_HOME:-$HOME/tools/jdk-25.0.5+8}
export PATH=$JAVA_HOME/bin:$PATH GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G}
H=${GHIDRA:-$HOME/tools/ghidra_12.2_PUBLIC}/support/analyzeHeadless
mkdir -p ghidra_project_ov export/overlays
for elf in overlays/ov*.elf; do
	n=$(basename "$elf" .elf)
	[ -s "export/overlays/$n.json" ] && continue
	"$H" "$PWD/ghidra_project_ov" "$n" -import "$PWD/$elf" -loader PspElfLoader -loader-imagebase 08804000 -deleteProject \
		-scriptPath "$PWD/ghidra_scripts" -postScript ExportProgram.java "$PWD/export/overlays/$n.json" > "export/overlays/$n.log" 2>&1 || true
	echo "$n: $(grep -cE '"entry"' "export/overlays/$n.json" 2>/dev/null || echo 0) entries"
done
