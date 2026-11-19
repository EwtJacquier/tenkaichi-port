// Text drawn again at high resolution: the strings the game renders into textures with its own
// bitmap font, and the words baked into its UI atlases, are drawn again with a vector font at the
// render scale (see mods/font.c and runtime/text.c). Without runtime/text.c, runtime/text_none.c
// leaves the game's own text, which the texture pack may have upscaled.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint16_t code;    // character (ASCII, or a two-byte code for icons and Korean)
	int16_t x;        // left edge of its cell in the strip, in PSP pixels
	uint8_t w, h;     // cell size of the game's glyph
	uint32_t rgb;     // 0xBBGGRR
	uint32_t bitmap;  // guest address of the game's glyph (w * h alpha bytes), 0 if none
} TextGlyph;

// A string the game just rendered into the RGBA strip at guest address buf (stride pixels per
// row, tex_w x tex_h texture). cap_top / baseline: rows of the bitmap font's capital letters.
void text_register(uint32_t buf, int stride, int tex_w, int tex_h, int cap_top, int baseline, const TextGlyph *g, int n);

// The strip at addr drawn k times larger, or NULL. Valid until the next call.
// (if the game registered it and its pixels are still the ones it rendered, and this vector text
// is on)
const uint32_t *text_hires(uint32_t addr, const uint8_t *src, size_t bytes, int w, int h, int stride, int k);

// A UI atlas with text (px, w x h texels) drawn k times larger, or NULL. Valid until the next call.
// UI atlases with baked text (generated/ui_text.spec, from tools/uitext/build.py): if the decoded
// texture px is one of them, the atlas with its text sprites drawn again as vector text with their
// outlines; NULL otherwise (and always while the UI vector text is off, the default: the texture
// pack has the atlases upscaled).
const uint32_t *uitext_hires(const uint32_t *px, int w, int h, int k);

// The same atlas over its upscaled picture from the texture pack (hd: pw x ph pixels for the
// iw x ih texels of the w x h texture), k times larger, or NULL. Valid until the next call.
// The pack's art with the words drawn over it, when the HD textures and the UI vector text are
// both on.
const uint32_t *uitext_over(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih);

// The same drawn by the GPU straight into the GL texture tex (allocated w*k x h*k; left bound to
// texture unit 0); false if it isn't drawn. Changes GL state (the caller re-applies its own).
bool uitext_hires_gl(const uint32_t *px, int w, int h, int k, unsigned tex);

// uitext_over drawn by the GPU straight into the GL texture tex (as uitext_hires_gl); false if it
// isn't (the CPU's uitext_over then).
bool uitext_over_gl(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih, unsigned tex);

// px is an atlas drawn by uitext_hires: that wins over the texture pack.
bool uitext_claims(const uint32_t *px, int w, int h);

// The text redrawing on (1) or off (0) for the game's lines and for the UI atlases (-1: as it is);
// true if that changed (textures drawn before must be made again: ge_forget_textures).
bool text_set_vector(int strings, int ui);

// Mods: the UI sprites of atlas whose text is `from` show the words `to` instead, when the UI
// atlases are redrawn. Call before the atlas is first drawn.
void uitext_replace(const char *atlas, const char *from, const char *to);
