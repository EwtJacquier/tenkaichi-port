#!/usr/bin/env bash
# Builds the browser version into build/web/ (index.html, .js, .wasm, .data).
# Needs Emscripten: emcc on PATH, or EMSDK set to your emsdk folder.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v emcc > /dev/null; then
	# shellcheck disable=SC1091
	source "${EMSDK:?emcc is not on PATH: set EMSDK to your emsdk folder}/emsdk_env.sh" > /dev/null 2>&1
fi
make -j"$(nproc)" web
ls -la build/web
