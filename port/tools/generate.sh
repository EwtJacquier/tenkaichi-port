#!/usr/bin/env bash
# From your decrypted EBOOT to C (generated/): loads the program the way the PSP does, then
# translates every function with the analysis in ../re/export (program.json: where the functions
# and jump tables are; no game code). Run it again after changing hooks.txt.
#
#   tools/generate.sh [path/to/EBOOT_decrypted.elf]      (default: ../EBOOT_decrypted.elf)
set -euo pipefail
cd "$(dirname "$0")/.."
ELF=${1:-../EBOOT_decrypted.elf}
WANT=eb7bffa629a689df3a35b19f14b508b005642735  # Dragon Ball Z: Tenkaichi Tag Team (USA), ULUS-10537
[ -f "$ELF" ] || { echo "no $ELF: see README.md (Getting the decrypted EBOOT)" >&2; exit 1; }
HAVE=$(sha1sum "$ELF" | cut -d' ' -f1)
[ "$HAVE" = "$WANT" ] || echo "warning: $ELF is not the EBOOT this port was made from (SHA-1 $HAVE, expected $WANT)" >&2
python3 -I tools/prx_load.py "$ELF" ../re/export/imports.json generated
python3 -I tools/recomp.py . ../re/export/program.json $(ls ../re/overlays/ov*.bin 2>/dev/null)  # overlays call into it
# The code overlays (re/tools/overlays.py, re/analyze_overlays.sh): each one apart, then their list.
ovs=()
for b in ../re/overlays/ov*.bin; do
	[ -e "$b" ] || continue
	n=$(basename "$b" .bin); n=${n#ov}
	j=../re/export/overlays/ov$n.json  # Ghidra's analysis; without it, the recompiler's own discovery
	[ -s "$j" ] || { j=generated/no_analysis.json; echo '{"functions": [], "jumptables": []}' > "$j"; }
	python3 -I tools/recomp.py --overlay . "$n" "$j" "$b" "../re/overlays/ov$n.json"
	ovs+=("$n")
done
{
	echo '#include "overlay.h"'
	for n in "${ovs[@]}"; do echo "extern const Overlay rt_overlay_$n;"; done
	printf 'const Overlay *const rt_overlays[] = {'
	for n in "${ovs[@]}"; do printf '&rt_overlay_%s, ' "$n"; done
	echo 'NULL};'
} > generated/overlays.c
tail -3 generated/recomp_report.txt 2>/dev/null || true
