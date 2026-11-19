# FFmpeg (audio/video decoding only)

Used for ATRAC3/ATRAC3+ (music, voices) and H.264 / MPEG-PS (movies). LGPL.

Origin: FFmpeg 3.x static builds (libavcodec 57) with the PSP-relevant decoders enabled
(`atrac3p`, `atrac3`, `h264`, `mp3`, demuxers `mpegps`, `oma`), from PPSSPP's `ppsspp-ffmpeg`
repository (commit in SOURCE.txt) — only the FFmpeg libraries, no emulator code.
`tools/get_ffmpeg.sh` fetches it, copies the prebuilt x86_64 libraries and builds the wasm32 ones
with Emscripten (`tools/ffmpeg_wasm32.sh`); the libs are not committed.

To replace with an official FFmpeg build: configure with
`--disable-everything --enable-decoder=atrac3p,atrac3,h264 --enable-parser=h264
--enable-demuxer=mpegps,oma --enable-swresample --enable-static --disable-programs`
(and `emconfigure` for wasm32), then put the `.a` files in `x86_64/lib` / `wasm32/lib`.
