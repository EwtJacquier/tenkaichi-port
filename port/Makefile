# Native (Linux) build of the recompiled game.
#   make -j16            build build/dbztag
#   make run ISO=...     run it
CC ?= gcc
GEN_OPT ?= -O2
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -fwrapv -Iruntime -Igenerated
GEN_CFLAGS = $(CFLAGS_COMMON) $(GEN_OPT) -w
FFMPEG = third_party/ffmpeg
RT_CFLAGS = $(CFLAGS_COMMON) -I$(FFMPEG)/include -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers $(shell pkg-config --cflags sdl2)
LDLIBS = $(FFMPEG)/x86_64/lib/libavcodec.a $(FFMPEG)/x86_64/lib/libswresample.a $(FFMPEG)/x86_64/lib/libavutil.a \
	$(shell pkg-config --libs sdl2) -lGLESv2 -lpthread -lm -rdynamic

GEN_SRC = $(wildcard generated/code_*.c) $(wildcard generated/ov_*.c) generated/overlays.c generated/func_table.c generated/hle_stubs.c
RT_SRC = $(wildcard runtime/*.c)
MOD_SRC = $(wildcard mods/*.c)
MOD_OBJ = $(patsubst mods/%.c,build/mods/%.o,$(MOD_SRC))
GEN_OBJ = $(patsubst generated/%.c,build/gen/%.o,$(GEN_SRC))
RT_OBJ = $(patsubst runtime/%.c,build/rt/%.o,$(RT_SRC))

ISO ?= ../Dragon\ Ball\ Z\ -\ Tenkaichi\ Tag\ Team\ (USA)\ (En,Fr,Es).iso

all: build/dbztag

build/dbztag: $(GEN_OBJ) $(RT_OBJ) $(MOD_OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

build/gen/%.o: generated/%.c generated/funcs.h runtime/cpu.h runtime/overlay.h | build/gen
	$(CC) $(GEN_CFLAGS) -c $< -o $@

build/rt/%.o: runtime/%.c runtime/rt.h runtime/cpu.h runtime/ge.h runtime/texpack.h runtime/platform.h runtime/text.h runtime/fonthd.h generated/hle.h | build/rt
	$(CC) $(RT_CFLAGS) -c $< -o $@

build/mods/%.o: mods/%.c runtime/rt.h runtime/cpu.h runtime/text.h runtime/fonthd.h generated/funcs.h | build/mods
	$(CC) $(RT_CFLAGS) -c $< -o $@

build/gen build/rt build/mods:
	mkdir -p $@

run: build/dbztag
	./build/dbztag $(ISO)

clean:
	rm -rf build/gen build/rt build/mods build/dbztag

.PHONY: all run clean

# ---- WebAssembly build (tools/build_web.sh sets up the Emscripten environment) -------------
EMCC ?= emcc
WEB_CFLAGS = -std=gnu11 -O2 -msimd128 -fno-strict-aliasing -fwrapv -Iruntime -Igenerated -I$(FFMPEG)/include -w
WGEN_OBJ = $(patsubst generated/%.c,build/wgen/%.o,$(GEN_SRC))
WRT_OBJ = $(patsubst runtime/%.c,build/wrt/%.o,$(RT_SRC))
WMOD_OBJ = $(patsubst mods/%.c,build/wmods/%.o,$(MOD_SRC))
WEB_LIBS = $(FFMPEG)/wasm32/lib/libavcodec.a $(FFMPEG)/wasm32/lib/libswresample.a $(FFMPEG)/wasm32/lib/libavutil.a
WEB_LDFLAGS = --emit-symbol-map -O2 -msimd128 -sASYNCIFY -sINITIAL_MEMORY=512MB -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=1MB \
	-sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3 -lidbfs.js -sFORCE_FILESYSTEM -sINVOKE_RUN=0 \
	-sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV,IDBFS,addRunDependency,removeRunDependency \
	-sEXPORTED_FUNCTIONS=_main --preload-file generated/image.bin@/image.bin $(WEB_EXTRA_FILES) \
	--pre-js web/audio.js --pre-js web/net.js $(WEB_PROF)
# make web WEB_PROF=--profiling-funcs: function names in the wasm (for the browser's profiler)
WEB_EXTRA_FILES = $(if $(wildcard assets/fonts/*),--preload-file assets/fonts@/assets/fonts) \
	$(if $(wildcard generated/ui_text.spec),--preload-file generated/ui_text.spec@/generated/ui_text.spec)
WEB_EXTRA_DEPS = $(wildcard assets/fonts/*) $(wildcard generated/ui_text.spec)

web: build/web/index.html build/web/index.js build/web/livekit-client.umd.js $(patsubst web/%,build/web/%,$(wildcard web/*.png web/*.webp))

build/web/livekit-client.umd.js: web/livekit-client.umd.js | build/web
	cp $< $@
build/web/%.png: web/%.png | build/web
	cp $< $@
build/web/%.webp: web/%.webp | build/web
	cp $< $@

# The page: web/shell.html with the game's script where its {{{ SCRIPT }}} marker is (what emcc's
# --shell-file did), made apart from the link: a change to the page alone takes no relink.
build/web/index.html: web/shell.html | build/web
	sed 's|{{{ SCRIPT }}}|<script async src="index.js"></script>|' $< > $@

build/web/index.js: $(WGEN_OBJ) $(WRT_OBJ) $(WMOD_OBJ) web/audio.js web/net.js $(WEB_EXTRA_DEPS) | build/web
	$(EMCC) -o $@ $(WGEN_OBJ) $(WRT_OBJ) $(WMOD_OBJ) $(WEB_LIBS) $(WEB_LDFLAGS)

build/wgen/%.o: generated/%.c generated/funcs.h runtime/cpu.h | build/wgen
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@
build/wrt/%.o: runtime/%.c runtime/rt.h runtime/cpu.h runtime/ge.h runtime/platform.h runtime/coro.h runtime/text.h runtime/texpack.h runtime/fonthd.h generated/hle.h | build/wrt
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@
build/wmods/%.o: mods/%.c runtime/rt.h runtime/cpu.h runtime/text.h runtime/fonthd.h generated/funcs.h | build/wmods
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@
build/wgen build/wrt build/wmods build/web:
	mkdir -p $@

.PHONY: web
