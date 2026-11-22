// Replacement textures: a texture pack (the HD textures; tools/textures.py installs one).
//
// The pack is a folder: index.txt lists one replacement per line, "<key> <iw> <ih> <file>", and
// each file is a QOI image. <key> is CRC-32 then Adler-32 (16 hex digits) of the RGBA bytes of the
// picture's own iw x ih texels, the top-left corner of the texture the game uploads (of the next
// powers of two, else named after the file: "<key> <iw> <ih> <file> <tw> <th>"); the image is
// that picture at an integer scale. Native: DBZ_TEXTURES (default build/textures). Web: the page
// writes index.txt to /textures and the images are fetched when first needed. A new index.txt is
// read again while the game runs. Nothing of the game's own files changes: without the pack (or
// with the page's "HD textures" switch off) the game shows its own textures.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void texpack_init(void);
// Once a frame: true when index.txt changed and was read again (textures must be looked up again).
bool texpack_changed(void);
// The key of the iw x ih texels at the top left of px (rows of w texels).
uint64_t texpack_key(const uint32_t *px, int w, int iw, int ih);
// The pack on or off while the game runs (the page's switch): true if that changed (textures must
// be looked up again: ge_forget_textures).
bool texpack_set_enabled(bool on);
// The replacement for a texture decoded to px (w x h RGBA texels, rows of w), or -1.
int texpack_find(const uint32_t *px, int w, int h);
// The key and picture size of the pack's entry idx (0 if there is none).
uint64_t texpack_entry_key(int idx, int *iw, int *ih);
// Its pixels (malloc'ed; the caller frees them), *pw x *ph for the *iw x *ih texels it replaces.
// NULL while the file is still being fetched (web), or for good when *failed is set.
uint32_t *texpack_load(int idx, int *pw, int *ph, int *iw, int *ih, bool *failed);

// ---- for the pack's other files (the HD font, runtime/fonthd.c) ----
// A pack is there and switched on.
bool texpack_active(void);
// CRC-32 then Adler-32 of n bytes (the pack's kind of key).
uint64_t texpack_hash(const void *p, size_t n);
// The bytes of a file in the pack's folder (malloc'ed), or NULL. *state starts at 0; on the web the
// first call starts fetching it (*state 1) and a later one returns it once it is there; *state 2:
// it can't be had.
uint8_t *texpack_file(const char *name, size_t *n, int *state);
// A QOI picture's RGBA pixels (malloc'ed), or NULL.
uint32_t *texpack_qoi(const uint8_t *d, size_t n, int *w, int *h);
