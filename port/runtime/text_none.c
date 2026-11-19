// No text redrawing (see text.h): the game's own text shows, as the texture pack has it. These
// are weak: a build with runtime/text.c uses its own.
#include "text.h"

#define WEAK __attribute__((weak))

WEAK void text_register(uint32_t buf, int stride, int tex_w, int tex_h, int cap_top, int baseline, const TextGlyph *g, int n) {
	(void)buf, (void)stride, (void)tex_w, (void)tex_h, (void)cap_top, (void)baseline, (void)g, (void)n;
}
WEAK const uint32_t *text_hires(uint32_t addr, const uint8_t *src, size_t bytes, int w, int h, int stride, int k) {
	(void)addr, (void)src, (void)bytes, (void)w, (void)h, (void)stride, (void)k;
	return NULL;
}
WEAK const uint32_t *uitext_hires(const uint32_t *px, int w, int h, int k) {
	(void)px, (void)w, (void)h, (void)k;
	return NULL;
}
WEAK const uint32_t *uitext_over(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih) {
	(void)px, (void)w, (void)h, (void)k, (void)hd, (void)pw, (void)ph, (void)iw, (void)ih;
	return NULL;
}
WEAK bool uitext_over_gl(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih, unsigned tex) {
	(void)px, (void)w, (void)h, (void)k, (void)hd, (void)pw, (void)ph, (void)iw, (void)ih, (void)tex;
	return false;
}

WEAK bool uitext_hires_gl(const uint32_t *px, int w, int h, int k, unsigned tex) {
	(void)px, (void)w, (void)h, (void)k, (void)tex;
	return false;
}
WEAK bool uitext_claims(const uint32_t *px, int w, int h) {
	(void)px, (void)w, (void)h;
	return false;
}
WEAK bool text_set_vector(int strings, int ui) {
	(void)strings, (void)ui;
	return false;
}
WEAK void uitext_replace(const char *atlas, const char *from, const char *to) { (void)atlas, (void)from, (void)to; }
