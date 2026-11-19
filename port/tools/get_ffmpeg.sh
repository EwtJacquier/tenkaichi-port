#!/usr/bin/env bash
# FFmpeg's static libraries (audio and video decoding: ATRAC3/3+, MP3, H.264; see
# third_party/ffmpeg/README.md), for the native build (x86_64) and the web build (wasm32).
#
#   tools/get_ffmpeg.sh                    # from port/
#   FFMPEG_SRC=dir tools/get_ffmpeg.sh     # use an existing checkout of ppsspp-ffmpeg
#
# The x86_64 libraries come prebuilt in github.com/hrydgard/ppsspp-ffmpeg (pinned below); the
# wasm32 ones are built from the same sources with Emscripten (emcc on PATH, or EMSDK set).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=https://github.com/hrydgard/ppsspp-ffmpeg.git
COMMIT=1e3b4965632f60b1d85360261d1b9dd45444bc71
SRC=${FFMPEG_SRC:-build/ppsspp-ffmpeg}
if [ ! -d "$SRC/linux/x86_64" ]; then
	git clone --filter=blob:none "$REPO" "$SRC"
	git -C "$SRC" checkout -q "$COMMIT"
fi
OUT=third_party/ffmpeg
mkdir -p $OUT/x86_64/lib $OUT/wasm32/lib
cp "$SRC/linux/x86_64/lib/"{libavcodec,libavformat,libavutil,libswresample}.a $OUT/x86_64/lib/
[ -d $OUT/include ] || cp -r "$SRC/linux/x86_64/include" $OUT/include

if [ ! -f "$SRC/linux/wasm32/lib/libavcodec.a" ]; then
	if ! command -v emcc > /dev/null; then
		# shellcheck disable=SC1091
		source "${EMSDK:?emcc is not on PATH: set EMSDK to your emsdk folder}/emsdk_env.sh" > /dev/null 2>&1
	fi
	cp tools/ffmpeg_wasm32.sh "$SRC/"
	(cd "$SRC" && sh ./ffmpeg_wasm32.sh)
fi
cp "$SRC/linux/wasm32/lib/"{libavcodec,libavformat,libavutil,libswresample}.a $OUT/wasm32/lib/
echo "ok: $OUT/{x86_64,wasm32}/lib"
