// HD font: the lines the game's font engine writes into textures (menus, descriptions, messages)
// drawn again at the render scale with the game's own glyphs, upscaled. The upscaled glyphs come
// with the texture pack (font-<key>.qoi + font-<key>.txt, key: CRC-32 + Adler-32 of the font's
// glyph table), so the font looks as it always did, only sharp; without them, or with the HD
// textures switched off, the game's own lines show.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text.h"

// The font at guest address font, as its glyph table (blob: "PGF1", u32 glyph count, the sorted
// u16 codes, the 8-byte glyph records as in memory, u32 bitmap bytes, the 8-bit alpha bitmaps).
// The blob's key names its HD glyphs; DBZ_FONT_DUMP=dir writes it as dir/font-<key>.bin (the
// glyphs to upscale).
bool fonthd_knows(uint32_t font);
void fonthd_font(uint32_t font, const uint8_t *blob, size_t n);

// A string the font engine just wrote into the RGBA strip at guest address buf (stride pixels a
// row, tex_w x tex_h), with the font object at guest address font.
void fonthd_register(uint32_t font, uint32_t buf, int stride, int tex_w, int tex_h, const TextGlyph *g, int n);

// The strip at addr drawn k times larger with the HD glyphs, if it was registered and still holds
// what the game wrote; NULL otherwise. Valid until the next call.
const uint32_t *fonthd_hires(uint32_t addr, const uint8_t *src, size_t bytes, int w, int h, int stride, int k);

// Once a frame: true when an HD font just became ready (strips drawn before it are made again).
bool fonthd_changed(void);
