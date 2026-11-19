#!/usr/bin/env bash
# prxdec: decrypts a PSP module (~PSP: EBOOT.BIN, PRX) with PPSSPP's PrxDecrypter + libkirk and
# gunzips it when the module is compressed. Needs a PPSSPP source tree (PPSSPP=...).
#   re/tools/prxdec/build.sh && re/tools/prxdec/prxdec EBOOT.BIN EBOOT_decrypted.elf
set -euo pipefail
cd "$(dirname "$0")"
P=${PPSSPP:?set PPSSPP to a PPSSPP source tree (git clone https://github.com/hrydgard/ppsspp)}
mkdir -p build
for c in AES amctrl bn ec kirk_engine SHA1; do gcc -O1 -w -I"$P/ext/libkirk" -c "$P/ext/libkirk/$c.c" -o build/k_$c.o; done
g++ -std=c++17 -O1 -w -I"$P" -I"$P/ext" -c "$P/Core/ELF/PrxDecrypter.cpp" -o build/prxdec_lib.o
g++ -std=c++17 -O1 -w -I"$P" -I"$P/ext" -c main.cpp -o build/main.o
g++ -std=c++17 -O1 -w -I"$P" -I"$P/ext" -c logstub.cpp -o build/logstub.o
g++ build/*.o -lz -o prxdec
echo "built $(pwd)/prxdec"
