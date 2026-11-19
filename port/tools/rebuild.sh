#!/usr/bin/env bash
# Full pipeline: apply analysis fixups in Ghidra, export, recompile to C, build.
#   tools/rebuild.sh          (from port/)
#   SKIP_GHIDRA=1 tools/rebuild.sh   (only recomp + build)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(cd .. && pwd)

if [ -z "${SKIP_GHIDRA:-}" ]; then
	export JAVA_HOME=${JAVA_HOME:-$HOME/tools/jdk-25.0.5+8}  # GHIDRA / JAVA_HOME from the environment, else ~/tools/
	export PATH=$JAVA_HOME/bin:$PATH GHIDRA_HEADLESS_MAXMEM=6G
	"${GHIDRA:-$HOME/tools/ghidra_12.2_PUBLIC}/support/analyzeHeadless" "$ROOT/re/ghidra_project" dbztag \
		-process EBOOT_decrypted.elf -scriptPath "$ROOT/re/ghidra_scripts" \
		-postScript FixupCode.java "$ROOT/re/export/fixups.json" \
		-postScript ExportProgram.java "$ROOT/re/export/program.json" > build/ghidra.log 2>&1
	grep -E "FixupCode.java>|ExportProgram.java>|ERROR" build/ghidra.log || true
fi

python3 -I tools/prx_load.py "$ROOT/EBOOT_decrypted.elf" "$ROOT/re/export/imports.json" generated
python3 -I tools/recomp.py . "$ROOT/re/export/program.json"
cat generated/recomp_report.txt
make -j"$(nproc)" 2>&1 | grep -E "error|warning" || true
ls -la build/dbztag
