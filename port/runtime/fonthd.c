// HD font (see fonthd.h): the font engine's strings registered as the game writes them, drawn again
// at the render scale from the upscaled glyphs of the texture pack.
#include "fonthd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "texpack.h"

typedef struct {
	uint16_t code, x, y, w, h;  // in the atlas, at the font's scale
} HdGlyph;

typedef struct {
	uint32_t font;  // guest address of the font object
	uint64_t key;
	int files[2];   // texpack_file states of font-<key>.txt and .qoi
	int state;      // 0 not loaded yet, 1 ready, 2 none for this font
	int scale, aw, ah, n;
	uint8_t *alpha;  // the atlas's coverage, aw x ah
	HdGlyph *g;      // sorted by code
} HdFont;

#define MAX_FONTS 8
static HdFont fonts[MAX_FONTS];
static int nfonts;
static bool fresh;

static int font_index(uint32_t font) {
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].font == font) return i;
	return -1;
}

bool fonthd_knows(uint32_t font) { return font_index(font) >= 0; }

void fonthd_font(uint32_t font, const uint8_t *blob, size_t n) {
	int i = font_index(font);
	if (i < 0) {
		if (nfonts == MAX_FONTS) return;
		i = nfonts++;
	}
	HdFont *f = &fonts[i];
	free(f->alpha);
	free(f->g);
	memset(f, 0, sizeof *f);
	f->font = font;
	f->key = texpack_hash(blob, n);
	rt_log("fonthd: font at %08x: key %016llx", font, (unsigned long long)f->key);
	const char *dump = getenv("DBZ_FONT_DUMP");
	if (dump) {
		char path[512];
		snprintf(path, sizeof path, "%s/font-%016llx.bin", dump, (unsigned long long)f->key);
		FILE *out = fopen(path, "wb");
		if (out) {
			fwrite(blob, 1, n, out);
			fclose(out);
			rt_log("fonthd: glyph table written to %s", path);
		}
	}
}

static int cmp_glyph(const void *a, const void *b) {
	return (int)((const HdGlyph *)a)->code - (int)((const HdGlyph *)b)->code;
}

// The font's HD glyphs: font-<key>.txt ("scale S", then "code x y w h" per glyph) and the atlas
// font-<key>.qoi (white, the glyphs' coverage in its alpha).
static bool load(HdFont *f) {
	if (f->state) return f->state == 1;
	char tname[64], qname[64];
	snprintf(tname, sizeof tname, "font-%016llx.txt", (unsigned long long)f->key);
	snprintf(qname, sizeof qname, "font-%016llx.qoi", (unsigned long long)f->key);
	size_t tn = 0, qn = 0;
	uint8_t *txt = texpack_file(tname, &tn, &f->files[0]);
	uint8_t *img = texpack_file(qname, &qn, &f->files[1]);
	if (f->files[0] == 2 || f->files[1] == 2 || !txt || !img) {
		if (f->files[0] == 2 || f->files[1] == 2) f->state = 2;  // none for this font
		free(txt);
		free(img);
		return false;  // else: on its way (web)
	}
	int w, h;
	uint32_t *px = texpack_qoi(img, qn, &w, &h);
	free(img);
	char *t = malloc(tn + 1);
	memcpy(t, txt, tn);
	t[tn] = 0;
	free(txt);
	int cap = 0, scale = 0;
	for (char *p = t; *p; p++) cap += *p == '\n';
	HdGlyph *g = malloc(sizeof *g * (size_t)(cap + 1));
	int n = 0;
	for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
		unsigned code, x, y, gw, gh;
		if (sscanf(line, "scale %d", &scale) == 1) continue;
		if (sscanf(line, "%u %u %u %u %u", &code, &x, &y, &gw, &gh) == 5 && x + gw <= (unsigned)w && y + gh <= (unsigned)h)
			g[n++] = (HdGlyph){(uint16_t)code, (uint16_t)x, (uint16_t)y, (uint16_t)gw, (uint16_t)gh};
	}
	free(t);
	if (!px || scale < 1 || !n) {
		rt_log("fonthd: %s / %s unusable", tname, qname);
		free(px);
		free(g);
		f->state = 2;
		return false;
	}
	qsort(g, (size_t)n, sizeof *g, cmp_glyph);
	f->alpha = malloc((size_t)w * h);
	for (size_t i = 0; i < (size_t)w * h; i++) f->alpha[i] = (uint8_t)(px[i] >> 24);
	free(px);
	f->aw = w, f->ah = h, f->g = g, f->n = n, f->scale = scale;
	f->state = 1;
	fresh = true;
	rt_log("fonthd: %d HD glyphs at %dx for font %016llx", n, scale, (unsigned long long)f->key);
	return true;
}

static const HdGlyph *glyph(const HdFont *f, uint16_t code) {
	HdGlyph probe = {.code = code};
	return bsearch(&probe, f->g, (size_t)f->n, sizeof probe, cmp_glyph);
}

// ---- registry of strings (as runtime/text.c keeps them) ---------------------------------------
typedef struct {
	uint32_t buf;
	int stride, tex_w, tex_h, n, font;
	uint64_t hash, age;
	TextGlyph *g;
} Reg;

#define MAX_REGS 1024
static Reg regs[MAX_REGS];
static uint64_t clock_;

static uint64_t fnv(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

void fonthd_register(uint32_t font, uint32_t buf, int stride, int tex_w, int tex_h, const TextGlyph *g, int n) {
	int fi = font_index(font);
	if (fi < 0 || n <= 0 || stride <= 0 || tex_h <= 0) return;
	size_t bytes = (size_t)stride * tex_h * 4;
	if (!rt_valid(buf, (uint32_t)bytes)) return;
	Reg *r = NULL, *old = &regs[0];
	for (int i = 0; i < MAX_REGS && !r; i++) {
		if (regs[i].buf == buf) r = &regs[i];
		else if (regs[i].age < old->age) old = &regs[i];
	}
	if (!r) r = old;
	free(r->g);
	r->g = malloc(sizeof *g * (size_t)n);
	memcpy(r->g, g, sizeof *g * (size_t)n);
	r->buf = buf, r->stride = stride, r->tex_w = tex_w, r->tex_h = tex_h, r->n = n, r->font = fi;
	r->hash = fnv(MP(buf), bytes);
	r->age = ++clock_;
	static int log = -1;
	if (log < 0) log = getenv("DBZ_FONTHD_LOG") != NULL;
	if (log) {
		char s[129];
		int k = 0;
		for (int i = 0; i < n && k < 128; i++) s[k++] = g[i].code >= 32 && g[i].code < 127 ? (char)g[i].code : '?';
		s[k] = 0;
		rt_log("fonthd: string at %08x %dx%d: \"%s\"", buf, tex_w, tex_h, s);
	}
}

// ---- drawing ------------------------------------------------------------------------------------------
static uint32_t *out;
static size_t out_cap;

static void put(int W, int H, int x, int y, unsigned a, uint32_t rgb) {
	if (x < 0 || y < 0 || x >= W || y >= H || !a) return;
	uint32_t *p = &out[(size_t)y * W + x];
	unsigned old = *p >> 24;
	*p = (rgb & 0xFFFFFFu) | (uint32_t)(a > old ? a : old) << 24;
}

// Coverage at (x, y) of a w x h picture of 8-bit alpha, sampled at scale k from scale s (bilinear;
// exact when k == s).
static unsigned sample(const uint8_t *src, int pitch, int w, int h, int x, int y, int s, int k) {
	if (k == s) return src[(size_t)y * pitch + x];
	float sx = ((float)x + 0.5f) * (float)s / (float)k - 0.5f, sy = ((float)y + 0.5f) * (float)s / (float)k - 0.5f;
	int x0 = (int)(sx < 0 ? sx - 1 : sx), y0 = (int)(sy < 0 ? sy - 1 : sy);
	float fx = sx - (float)x0, fy = sy - (float)y0, v = 0;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			int xx = x0 + i, yy = y0 + j;
			if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
			v += (float)src[(size_t)yy * pitch + xx] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
		}
	return (unsigned)(v + 0.5f);
}

const uint32_t *fonthd_hires(uint32_t addr, const uint8_t *src, size_t bytes, int w, int h, int stride, int k) {
	if (k < 2 || !texpack_active()) return NULL;
	Reg *r = NULL;
	for (int i = 0; i < MAX_REGS && !r; i++)
		if (regs[i].buf == addr && regs[i].n) r = &regs[i];
	if (!r || r->stride != stride || r->tex_w != w || r->tex_h != h) return NULL;
	if (bytes != (size_t)stride * h * 4 || fnv(src, bytes) != r->hash) return NULL;  // reused for something else
	HdFont *f = &fonts[r->font];
	if (!load(f)) return NULL;
	int W = w * k, H = h * k;
	size_t need = (size_t)W * H;
	if (need > out_cap) out_cap = need, out = realloc(out, need * 4);
	memset(out, 0, need * 4);
	for (int i = 0; i < r->n; i++) {
		const TextGlyph *g = &r->g[i];
		int cx = g->x * k, cw = g->w * k, ch = g->h * k;
		// color under the whole cell, so filtering at the glyph's edges doesn't pull in black
		for (int y = 0; y < H; y++)
			for (int x = cx; x < cx + cw && x < W; x++)
				if (x >= 0 && !(out[(size_t)y * W + x] >> 24)) out[(size_t)y * W + x] = g->rgb & 0xFFFFFFu;
		const HdGlyph *hg = glyph(f, g->code);
		if (hg && hg->w == g->w * f->scale && hg->h == g->h * f->scale) {
			const uint8_t *a = f->alpha + (size_t)hg->y * f->aw + hg->x;
			for (int y = 0; y < ch; y++)
				for (int x = 0; x < cw; x++) put(W, H, cx + x, y, sample(a, f->aw, hg->w, hg->h, x, y, f->scale, k), g->rgb);
		}
		else if (g->bitmap && g->w && g->h && rt_valid(g->bitmap, (uint32_t)(g->w * g->h))) {
			const uint8_t *a = MP(g->bitmap);  // a glyph the HD font lacks: the game's, scaled
			for (int y = 0; y < ch; y++)
				for (int x = 0; x < cw; x++) put(W, H, cx + x, y, sample(a, g->w, g->w, g->h, x, y, 1, k), g->rgb);
		}
	}
	r->age = ++clock_;
	return out;
}

bool fonthd_changed(void) {
	bool r = fresh;
	fresh = false;
	return r;
}
