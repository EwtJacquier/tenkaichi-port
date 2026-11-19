// High-resolution text (see text.h). The game's font engine writes every string into an RGBA
// strip 16 pixels tall; mods/font.c registers each string's layout (character, cell, color),
// and when the strip becomes a texture it is drawn again here with a vector font at the render
// scale: same cells, same colors, sharp at any resolution. Characters the font doesn't have
// (button icons, Korean) use the game's own glyph, scaled up.
#include "text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <libgen.h>
#include <unistd.h>
#endif

#include "cpu.h"
#include "ge.h"
#include "rt.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

#define FONT_FILE "fonts/texgyreadventor-bold.otf"
#define FONT_FILE_REGULAR "fonts/texgyreadventor-regular.otf"

// Which vector text is drawn (natively DBZ_MOD_TEXT=1 / DBZ_UITEXT=1; on the web the same as
// URL parameters), both off by default: every picture, words included, comes from the texture pack,
// and the font engine's lines from its HD font. The lines of the game's font engine and the words
// in the UI atlases (menu titles, tutorial pages, HUD digits: those atlases are drawn by this file
// then, not taken from the pack).
static bool vec_strings = false, vec_ui = false, vec_read;

static void vec_defaults(void) {
	if (vec_read) return;
	vec_read = true;
	if (getenv("DBZ_MOD_TEXT")) vec_strings = atoi(getenv("DBZ_MOD_TEXT")) != 0;
	if (getenv("DBZ_UITEXT")) vec_ui = atoi(getenv("DBZ_UITEXT")) != 0;
}

static stbtt_fontinfo font;          // bold: UI sprites with font 0
static stbtt_fontinfo font_regular;  // regular: the font engine's strings (its letters are thin), UI sprites with font 1
static unsigned char *font_data, *font_regular_data;
static int font_state;   // 0 not tried, 1 loaded, -1 unavailable
static float cap_units;  // height of 'H' in font units

static unsigned char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
	fclose(f);
	return d;
}

// assets/ is looked up in DBZ_ASSETS, the working directory, next to the executable
// (build/../assets) and at /assets (the web build's preloaded files).
static bool font_load(void) {
	if (font_state) return font_state > 0;
	font_state = -1;
	char path[1024];
	const char *dirs[4] = {getenv("DBZ_ASSETS"), "assets", NULL, "/assets"};
#ifndef __EMSCRIPTEN__
	static char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 32);
	if (n > 0) {
		exe[n] = 0;
		char *d = dirname(exe);
		memmove(exe, d, strlen(d) + 1);
		strcat(exe, "/../assets");
		dirs[2] = exe;
	}
#endif
	for (int i = 0; i < 4 && !font_data; i++) {
		if (!dirs[i]) continue;
		snprintf(path, sizeof path, "%s/%s", dirs[i], FONT_FILE);
		font_data = read_file(path);
		snprintf(path, sizeof path, "%s/%s", dirs[i], FONT_FILE_REGULAR);
		if (font_data) font_regular_data = read_file(path);
	}
	if (!font_data || !stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
		rt_log("text: %s not found, the game's bitmap font stays", FONT_FILE);
		return false;
	}
	if (!font_regular_data || !stbtt_InitFont(&font_regular, font_regular_data, stbtt_GetFontOffsetForIndex(font_regular_data, 0)))
		font_regular = font;  // regular-weight text falls back to bold
	int x0, y0, x1, y1;
	stbtt_GetCodepointBox(&font_regular, 'H', &x0, &y0, &x1, &y1);
	cap_units = (float)(y1 > 0 ? y1 : 700);
	rt_log("text: vector font %s", FONT_FILE);
	font_state = 1;
	return true;
}

// ---- registry of strings --------------------------------------------------------------------
typedef struct {
	uint32_t buf;
	int stride, tex_w, tex_h, cap_top, baseline, n;
	uint64_t hash, age;
	TextGlyph *g;
} TextReg;

#define MAX_TEXTS 1024
static TextReg texts[MAX_TEXTS];
static uint64_t text_clock;

static uint64_t fnv(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

void text_register(uint32_t buf, int stride, int tex_w, int tex_h, int cap_top, int baseline, const TextGlyph *g, int n) {
	if (!font_load() || n <= 0 || stride <= 0 || tex_h <= 0 || baseline <= cap_top) return;
	size_t bytes = (size_t)stride * tex_h * 4;
	if (!rt_valid(buf, (uint32_t)bytes)) return;
	TextReg *t = NULL, *old = &texts[0];
	for (int i = 0; i < MAX_TEXTS && !t; i++) {
		if (texts[i].buf == buf) t = &texts[i];
		else if (texts[i].age < old->age) old = &texts[i];
	}
	if (!t) t = old;
	free(t->g);
	t->g = malloc(sizeof *g * (size_t)n);
	memcpy(t->g, g, sizeof *g * (size_t)n);
	t->buf = buf;
	t->stride = stride;
	t->tex_w = tex_w;
	t->tex_h = tex_h;
	t->cap_top = cap_top;
	t->baseline = baseline;
	t->n = n;
	t->hash = fnv(MP(buf), bytes);
	t->age = ++text_clock;
}

// ---- drawing ----------------------------------------------------------------------------------
static uint32_t *out;
static size_t out_cap;
static unsigned char *gbm;
static size_t gbm_cap;

static void put(int W, int H, int x, int y, unsigned a, uint32_t rgb) {
	if (x < 0 || y < 0 || x >= W || y >= H || !a) return;
	uint32_t *p = &out[(size_t)y * W + x];
	unsigned old = *p >> 24;
	*p = (rgb & 0xFFFFFFu) | (uint32_t)(a > old ? a : old) << 24;
}

static float text_weight(void) {  // DBZ_TEXT_WEIGHT: extra stroke, 1x px (default 0.15)
	static float w = -1;
	if (w < 0) {
		const char *e = getenv("DBZ_TEXT_WEIGHT");
		w = e ? (float)atof(e) : 0.15f;
		if (w < 0 || w > 2) w = 0.15f;
	}
	return w;
}

static bool is_vector(uint16_t code) { return code >= ' ' && code < 0x7F && stbtt_FindGlyphIndex(&font_regular, code); }

// A glyph of the font engine's strings as drawn: at this size and stretch, its origin q/8 pixel
// right of a pixel's edge, heavier by `weight` pixels (the union of the glyph drawn around a small
// circle: the game's letters are a little heavier than the regular weight). Kept, as strings
// repeat their letters and come back.
typedef struct {
	int code, q, x0, y0, w, h;
	float sxs, sc, weight;
	uint8_t *cov;
} GlyphBits;

static const GlyphBits *glyph_bits(int code, float sxs, float sc, int q, float weight) {
	static GlyphBits cache[2048];
	unsigned hsh = (unsigned)code * 2654435761u ^ (unsigned)(sxs * 1024.f) * 40503u ^ (unsigned)(sc * 1024.f) * 9973u ^ (unsigned)q * 131u;
	GlyphBits *e = &cache[hsh % 2048];
	if (e->cov && e->code == code && e->q == q && e->sxs == sxs && e->sc == sc && e->weight == weight) return e;
	float sub = (float)q / 8.f;
	int x0, y0, x1, y1;
	stbtt_GetCodepointBitmapBoxSubpixel(&font_regular, code, sxs, sc, sub, 0, &x0, &y0, &x1, &y1);
	int gw = x1 - x0, gh = y1 - y0;
	if (gw <= 0 || gh <= 0) return NULL;
	if ((size_t)(gw * gh) > gbm_cap) gbm_cap = (size_t)(gw * gh) * 2, gbm = realloc(gbm, gbm_cap);
	int taps = weight > 0.05f ? 9 : 1, pad = taps > 1 ? (int)ceilf(weight) + 1 : 0;
	int w = gw + 2 * pad, h = gh + 2 * pad;
	uint8_t *cov = calloc((size_t)w * h, 1);
	for (int tap = 0; tap < taps; tap++) {
		float dx = tap ? weight * cosf((float)tap * 0.785398f) : 0, dy = tap ? weight * sinf((float)tap * 0.785398f) : 0;
		float fx = sub + dx, fy = dy;
		int ox = (int)floorf(fx), oy = (int)floorf(fy);
		stbtt_MakeCodepointBitmapSubpixel(&font_regular, gbm, gw, gh, gw, sxs, sc, fx - (float)ox, fy - (float)oy, code);
		for (int y = 0; y < gh; y++)
			for (int x = 0; x < gw; x++) {
				int X = pad + ox + x, Y = pad + oy + y;
				if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
				uint8_t *c = &cov[Y * w + X], v = gbm[y * gw + x];
				if (v > *c) *c = v;
			}
	}
	free(e->cov);
	*e = (GlyphBits){code, q, x0 - pad, y0 - pad, w, h, sxs, sc, weight, cov};
	return e;
}

static void draw(const TextReg *t, int k) {
	int W = t->tex_w * k, H = t->tex_h * k;
	float scale = (float)((t->baseline - t->cap_top) * k) / cap_units;
	int base_y = t->baseline * k;
	// The vector font's own advances and kerning. The string must keep the game's width (it is
	// centered or aligned by it): the letters get up to 15% wider (the bitmap font is wide), and
	// what is left is spread evenly between them.
	static float adv_of[128];
	float vec_w = 0, other_w = 0;
	for (int i = 0; i < t->n; i++) {
		const TextGlyph *g = &t->g[i];
		if (is_vector(g->code)) {
			int adv, lsb;
			stbtt_GetCodepointHMetrics(&font_regular, g->code, &adv, &lsb);
			adv_of[i] = (float)adv * scale;
			if (i + 1 < t->n && is_vector(t->g[i + 1].code))
				adv_of[i] += (float)stbtt_GetCodepointKernAdvance(&font_regular, g->code, t->g[i + 1].code) * scale;
			vec_w += adv_of[i];
		}
		else {
			adv_of[i] = (float)(g->w * k);
			other_w += adv_of[i];
		}
	}
	const TextGlyph *last = &t->g[t->n - 1];
	float target = (float)((last->x + last->w) * k);
	float sx = vec_w > 0 ? (target - other_w) / vec_w : 1;
	sx = sx < 1 ? 1 : sx > 1.15f ? 1.15f : sx;
	static float pos[128];
	float pen = 0;
	for (int i = 0; i < t->n; i++) {
		pos[i] = pen;
		pen += is_vector(t->g[i].code) ? adv_of[i] * sx : adv_of[i];
	}
	float track = t->n > 1 ? (target - pen) / (float)(t->n - 1) : 0;
	for (int i = 0; i < t->n; i++) {
		const TextGlyph *g = &t->g[i];
		int cx = (int)lroundf(pos[i] + track * (float)i), cw = g->w * k;
		// Color under the whole cell, so filtering at the glyph's edges doesn't pull in black.
		for (int y = 0; y < H; y++)
			for (int x = cx; x < cx + cw && x < W; x++)
				if (x >= 0 && !(out[(size_t)y * W + x] >> 24)) out[(size_t)y * W + x] = g->rgb & 0xFFFFFFu;
		if (g->code == ' ') continue;
		if (is_vector(g->code)) {
			float origin = pos[i] + track * (float)i;
			int ix = (int)floorf(origin), q = (int)lroundf((origin - (float)ix) * 8.f);
			if (q == 8) ix++, q = 0;
			const GlyphBits *gb = glyph_bits(g->code, scale * sx, scale, q, text_weight() * (float)k);
			if (!gb) continue;
			for (int y = 0; y < gb->h; y++)
				for (int x = 0; x < gb->w; x++) put(W, H, ix + gb->x0 + x, base_y + gb->y0 + y, gb->cov[y * gb->w + x], g->rgb);
		}
		else if (g->bitmap && g->w && g->h && rt_valid(g->bitmap, (uint32_t)(g->w * g->h))) {
			// Icons and characters the font lacks: the game's glyph, bilinear-scaled.
			const uint8_t *src = MP(g->bitmap);
			for (int y = 0; y < g->h * k; y++) {
				float sy = ((float)y + 0.5f) / (float)k - 0.5f;
				int y0 = (int)floorf(sy);
				float fy = sy - (float)y0;
				for (int x = 0; x < g->w * k; x++) {
					float sx = ((float)x + 0.5f) / (float)k - 0.5f;
					int x0 = (int)floorf(sx);
					float fx = sx - (float)x0;
					float v = 0;
					for (int j = 0; j < 2; j++)
						for (int i2 = 0; i2 < 2; i2++) {
							int xx = x0 + i2, yy = y0 + j;
							if (xx < 0 || yy < 0 || xx >= g->w || yy >= g->h) continue;
							v += (float)src[yy * g->w + xx] * (i2 ? fx : 1 - fx) * (j ? fy : 1 - fy);
						}
					put(W, H, cx + x, y, (unsigned)(v + 0.5f), g->rgb);
				}
			}
		}
	}
}

bool text_set_vector(int strings, int ui) {
	vec_defaults();
	bool s = strings < 0 ? vec_strings : strings != 0, u = ui < 0 ? vec_ui : ui != 0;
	if (s == vec_strings && u == vec_ui) return false;
	vec_strings = s, vec_ui = u;
	rt_log("text: vector text %s for the game's lines, %s for the UI atlases", s ? "on" : "off", u ? "on" : "off");
	return true;
}

const uint32_t *text_hires(uint32_t addr, const uint8_t *src, size_t bytes, int w, int h, int stride, int k) {
	vec_defaults();
	if (!vec_strings || font_state <= 0 || k < 1) return NULL;
	for (int i = 0; i < MAX_TEXTS; i++) {
		TextReg *t = &texts[i];
		if (t->buf != addr || !t->n) continue;
		if (t->stride != stride || t->tex_w != w || t->tex_h != h) return NULL;
		if (bytes != (size_t)stride * h * 4 || fnv(src, bytes) != t->hash) return NULL;  // reused for something else
		size_t need = (size_t)w * k * h * k;
		if (need > out_cap) out_cap = need, out = realloc(out, need * 4);
		memset(out, 0, need * 4);
		uint64_t t0 = rt_now_us();
		draw(t, k);
		if (getenv("DBZ_UITEXT_DEBUG")) {
			static uint64_t total, calls;
			total += rt_now_us() - t0, calls++;
			if (calls % 50 == 0) rt_log("text: %llu strings drawn, %.1f ms in all", (unsigned long long)calls, total / 1e3);
		}
		t->age = ++text_clock;
		return out;
	}
	return NULL;
}

// =============================================================================================
// Text baked into UI atlases (see tools/uitext/build.py for how the description is made)
// =============================================================================================
typedef struct {
	int x, y, w, h;           // sprite rectangle in the atlas
	float ox, oy, em, sx;     // baseline origin (sprite coordinates), pixels per em, x stretch
	float bold;               // extra stroke weight, 1x px per side (matches the original letters)
	int font;                 // 0 bold, 1 regular
	uint32_t ftop, fbot;      // fill colors (RGBA as bytes R, G, B, A) at the top / bottom
	int nrings;
	float rw[4];              // ring widths (1x px), inside-out; negative: a glow that fades out
	uint32_t rc[4];
	float ext;                // the outlines reach this much deeper below the letters (1x px)
	bool keep;                // art shares the rectangle: clear only around the letters
	char text[192];
	// Text over a background (button, panel): the pixels to rebuild before the new text goes on
	// top, a w x h bit mask at (mx, my) in the sprite.
	bool panel;
	int mx, my, mw, mh;
	uint8_t *mask;
	// Lines after the first ("|" in text): baseline origin and x stretch of each.
	int nlines;
	float lox[4], loy[4], lsx[4];
} UiSprite;

typedef struct {
	char name[40];
	int w, h, n, cap;
	uint32_t crc;     // of all its pixels
	uint32_t sample;  // of 64 of them along the diagonal (a cheap first test), if has_sample
	bool has_sample;
	UiSprite *s;
	uint32_t *inpainted;  // its pixels with the panels' old text rebuilt, once computed
	bool inpainted_done;
	int nrects;          // every sprite of the atlas ("R" line), x y w h
	int (*rects)[4];
	uint16_t *rectmap;   // per texel, the sprite it belongs to (x0 y0 x1 y1; 0 0 0 0: none), once built
} UiAtlas;

static UiAtlas *atlases;
static int n_atlases, ui_state;

static uint32_t crc_table[256];
static uint32_t crc32_rows(const uint32_t *px, int stride, int w, int h) {
	if (!crc_table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	uint32_t c = 0xFFFFFFFFu;
	for (int y = 0; y < h; y++) {
		const uint8_t *p = (const uint8_t *)(px + (size_t)y * stride);
		for (int i = 0; i < w * 4; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	}
	return c ^ 0xFFFFFFFFu;
}

static uint32_t hex_rgba(const char *s) {  // "rrggbbaa" -> bytes R, G, B, A in memory order
	uint32_t v = (uint32_t)strtoul(s, NULL, 16);
	return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

static void ui_load(void) {
	if (ui_state) return;
	ui_state = -1;
	if (!font_load()) return;
	const char *paths[] = {getenv("DBZ_UI_TEXT"), "generated/ui_text.spec", "/generated/ui_text.spec", NULL};
	FILE *f = NULL;
	char exe_spec[1100];
#ifndef __EMSCRIPTEN__
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n > 0) {
		exe[n] = 0;
		snprintf(exe_spec, sizeof exe_spec, "%s/../generated/ui_text.spec", dirname(exe));
		paths[3] = exe_spec;
	}
#endif
	(void)exe_spec;
	for (int i = 0; i < 4 && !f; i++)
		if (paths[i]) f = fopen(paths[i], "r");
	if (!f) {
		rt_log("text: generated/ui_text.spec not found (tools/uitext/build.py makes it), UI atlases stay as they are");
		return;
	}
	char *line = NULL;
	size_t line_cap = 0;
	int sprites = 0;
	UiAtlas *a = NULL;
	while (getline(&line, &line_cap, f) > 0) {  // panel lines carry a mask and run to a few KB
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == 'A') {
			atlases = realloc(atlases, sizeof *atlases * (size_t)(n_atlases + 1));
			a = &atlases[n_atlases++];
			memset(a, 0, sizeof *a);
			char crc[16], smp[16] = "";
			if (sscanf(line, "A %39s %d %d %15s %d %15s", a->name, &a->w, &a->h, crc, &a->cap, smp) < 5 || a->cap <= 0) {
				n_atlases--, a = NULL;
				continue;
			}
			a->crc = (uint32_t)strtoul(crc, NULL, 16);
			a->has_sample = smp[0] != 0, a->sample = (uint32_t)strtoul(smp, NULL, 16);
			a->s = calloc((size_t)a->cap, sizeof *a->s);
			continue;
		}
		if (line[0] == 'R' && a) {  // R x,y,w,h ...: the atlas's sprites
			int cap = 0;
			for (const char *p = line + 1; *p; p++) cap += *p == ',';
			a->rects = calloc((size_t)(cap / 3 + 1), sizeof *a->rects);
			int used = 0;
			for (const char *p = line + 1; a->nrects <= cap / 3;) {
				int *r = a->rects[a->nrects];
				if (sscanf(p, " %d,%d,%d,%d%n", &r[0], &r[1], &r[2], &r[3], &used) != 4) break;
				a->nrects++, p += used;
			}
			continue;
		}
		if ((line[0] != 'S' && line[0] != 'P') || !a) continue;
		UiSprite s = {0};
		s.panel = line[0] == 'P';
		char ft[16], fb[16];
		int pos = 0;
		if (sscanf(line + 1, " %d %d %d %d %f %f %f %f %f %d %15s %15s %d%n", &s.x, &s.y, &s.w, &s.h, &s.ox, &s.oy, &s.em,
		           &s.sx, &s.bold, &s.font, ft, fb, &s.nrings, &pos) != 13 || s.nrings < 0 || s.nrings > 4)
			continue;
		pos += 1;
		s.ftop = hex_rgba(ft);
		s.fbot = hex_rgba(fb);
		const char *p = line + pos;
		bool ok = true;
		for (int i = 0; i < s.nrings && ok; i++) {
			char c[16];
			int used = 0;
			ok = sscanf(p, " %f %15s%n", &s.rw[i], c, &used) == 2;
			s.rc[i] = hex_rgba(c);
			p += used;
		}
		while (*p == ' ') p++;
		if (ok && !strncmp(p, "e:", 2)) {  // e:ext
			int used = 0;
			ok = sscanf(p, "e:%f%n", &s.ext, &used) == 1 && s.ext >= 0 && s.ext < 8;
			p += used;
			while (*p == ' ') p++;
		}
		if (ok && !strncmp(p, "k:1 ", 4)) {  // k:1 keep the art around the letters
			s.keep = true;
			p += 4;
			while (*p == ' ') p++;
		}
		if (s.panel && ok) {  // m:x,y,w,h,hexbits
			int used = 0;
			ok = sscanf(p, "m:%d,%d,%d,%d,%n", &s.mx, &s.my, &s.mw, &s.mh, &used) == 4 && used > 0 && s.mw > 0 && s.mh > 0 &&
			     s.mx + s.mw <= s.w && s.my + s.mh <= s.h;
			if (ok) {
				p += used;
				s.mask = calloc((size_t)s.mw * s.mh, 1);
				for (int i = 0; i < s.mw * s.mh && ok; i++) {
					char c = p[i / 4];
					int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
					if (v < 0) ok = false;
					else s.mask[i] = (uint8_t)((v >> (3 - i % 4)) & 1);
				}
				p += (s.mw * s.mh + 3) / 4;
				while (*p == ' ') p++;
			}
			if (!ok) free(s.mask), s.mask = NULL;
		}
		if (ok && !strncmp(p, "l:", 2)) {  // l:ox,oy,sx;ox,oy,sx
			p += 2;
			int used = 0;
			while (s.nlines < 4 && sscanf(p, "%f,%f,%f%n", &s.lox[s.nlines], &s.loy[s.nlines], &s.lsx[s.nlines], &used) == 3) {
				s.nlines++;
				p += used;
				if (*p == ';') p++;
			}
			while (*p && *p != ' ') p++;
			while (*p == ' ') p++;
		}
		if (!ok || !*p) continue;
		snprintf(s.text, sizeof s.text, "%s", p);
		if (a->n < a->cap) a->s[a->n++] = s, sprites++;
	}
	free(line);
	fclose(f);
	rt_log("text: %d text sprites in %d UI atlases", sprites, n_atlases);
	ui_state = 1;
}

// Squared Euclidean distance transform (Felzenszwalb & Huttenlocher), in place on rows of n.
static void edt1(float *f, int n, float *d, int *v, float *z) {
	int k = 0;
	v[0] = 0;
	z[0] = -1e20f, z[1] = 1e20f;
	for (int q = 1; q < n; q++) {
		float s;
		for (;;) {
			s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (float)(2 * q - 2 * v[k]);
			if (s > z[k] || k == 0) break;
			k--;
		}
		if (s <= z[k]) s = z[k];
		k++;
		v[k] = q, z[k] = s, z[k + 1] = 1e20f;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < (float)q) k++;
		d[q] = (float)(q - v[k]) * (float)(q - v[k]) + f[v[k]];
	}
}

static void edt(float *g, int w, int h) {
	int n = w > h ? w : h;
	float *f = malloc(sizeof *f * (size_t)n), *d = malloc(sizeof *d * (size_t)n), *z = malloc(sizeof *z * (size_t)(n + 1));
	int *v = malloc(sizeof *v * (size_t)n);
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) f[y] = g[(size_t)y * w + x];
		edt1(f, h, d, v, z);
		for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[y];
	}
	for (int y = 0; y < h; y++) {
		memcpy(f, g + (size_t)y * w, sizeof *f * (size_t)w);
		edt1(f, w, d, v, z);
		memcpy(g + (size_t)y * w, d, sizeof *f * (size_t)w);
	}
	free(f), free(d), free(z), free(v);
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// src over the RGBA pixel *d (bytes R, G, B, A), with coverage a (0..1) times src alpha.
static inline void over(uint32_t *d, uint32_t src, float a) {
	float sa = a * (float)(src >> 24) / 255.f;
	if (sa <= 0) return;
	float da = (float)(*d >> 24) / 255.f, oa = sa + da * (1 - sa);
	uint32_t o = 0;
	for (int c = 0; c < 3; c++) {
		float sc = (float)((src >> (8 * c)) & 0xFF), dc = (float)((*d >> (8 * c)) & 0xFF);
		float v = (sc * sa + dc * da * (1 - sa)) / (oa > 0 ? oa : 1);
		o |= (uint32_t)clampf(v + 0.5f, 0, 255) << (8 * c);
	}
	*d = o | (uint32_t)clampf(oa * 255.f + 0.5f, 0, 255) << 24;
}

static uint32_t lerp_rgba(uint32_t a, uint32_t b, float t) {
	uint32_t o = 0;
	for (int c = 0; c < 4; c++) {
		float va = (float)((a >> (8 * c)) & 0xFF), vb = (float)((b >> (8 * c)) & 0xFF);
		o |= (uint32_t)clampf(va + (vb - va) * t + 0.5f, 0, 255) << (8 * c);
	}
	return o;
}

// The next character of a spec's text (UTF-8: labels.tsv has ™ and ®).
static int utf8_next(const char **pp) {
	const unsigned char *p = (const unsigned char *)*pp;
	int c = *p++;
	if (c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80)
		c = (c & 0x1F) << 6 | (p[0] & 0x3F), p += 1;
	else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
		c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F), p += 2;
	*pp = (const char *)p;
	return c;
}

// ---- other words on a sprite (mods: the title's single "Start Game" button) ----------------
typedef struct {
	char atlas[40], from[64], to[64];
} UiReplace;
static UiReplace ui_repl[8];
static int n_repl;

void uitext_replace(const char *atlas, const char *from, const char *to) {
	if (n_repl == (int)(sizeof ui_repl / sizeof *ui_repl)) return;
	UiReplace *r = &ui_repl[n_repl++];
	snprintf(r->atlas, sizeof r->atlas, "%s", atlas);
	snprintf(r->from, sizeof r->from, "%s", from);
	snprintf(r->to, sizeof r->to, "%s", to);
}

static float text_width(const stbtt_fontinfo *fi, const char *t, float scale_x) {
	float w = 0;
	for (const char *p = t; *p && *p != '|';) {
		int ch = utf8_next(&p), adv, lsb;
		stbtt_GetCodepointHMetrics(fi, ch, &adv, &lsb);
		w += (float)adv * scale_x;
		if (*p && *p != '|') {
			const char *q = p;
			w += (float)stbtt_GetCodepointKernAdvance(fi, ch, utf8_next(&q)) * scale_x;
		}
	}
	return w;
}

// The new words take the old ones' place: same look, centered where they were, narrower if they
// would leave the sprite.
static void ui_apply_replacements(void) {
	for (int r = 0; r < n_repl; r++)
		for (int i = 0; i < n_atlases; i++) {
			if (strcmp(atlases[i].name, ui_repl[r].atlas)) continue;
			for (int j = 0; j < atlases[i].n; j++) {
				UiSprite *s = &atlases[i].s[j];
				if (s->nlines || strcmp(s->text, ui_repl[r].from)) continue;
				const stbtt_fontinfo *fi = s->font == 1 ? &font_regular : &font;
				float sc = stbtt_ScaleForMappingEmToPixels(fi, s->em), reach = s->bold + 1.f;
				for (int k = 0; k < s->nrings; k++) reach += fabsf(s->rw[k]);
				float old_w = text_width(fi, s->text, sc * s->sx), new_w = text_width(fi, ui_repl[r].to, sc * s->sx);
				float room = (float)s->w - 2.f * reach;
				if (new_w > room && new_w > 0 && room > 0) s->sx *= room / new_w, new_w = room;
				s->ox += (old_w - new_w) / 2.f;
				snprintf(s->text, sizeof s->text, "%s", ui_repl[r].to);
				rt_log("text: %s \"%s\" drawn as \"%s\"", atlases[i].name, ui_repl[r].from, ui_repl[r].to);
			}
		}
}

// A word on its own over a background of its atlas (a bar, a picture): the sprite's rectangle has
// non-transparent pixels along its edge. Clearing the whole rectangle there opened a hole in that
// background (the bar went see-through around the word): only the old letters' surroundings are
// cleared then, to the background's own color (*fill: the median of the edge's non-transparent
// pixels). px: the atlas as the game has it (w x h).
static bool sprite_backdrop(const uint32_t *px, int w, int h, const UiSprite *s, uint32_t *fill) {
	if (!px || s->panel || s->keep || s->x + s->w > w || s->y + s->h > h) return false;
	int n = 0, solid = 0;
	static uint8_t ch[4][4096];
	for (int i = 0; i < 2 * (s->w + s->h); i++) {
		int x, y;
		if (i < s->w) x = s->x + i, y = s->y;
		else if (i < 2 * s->w) x = s->x + i - s->w, y = s->y + s->h - 1;
		else if (i < 2 * s->w + s->h) x = s->x, y = s->y + i - 2 * s->w;
		else x = s->x + s->w - 1, y = s->y + i - 2 * s->w - s->h;
		uint32_t p = px[(size_t)y * w + x];
		n++;
		if ((p >> 24) <= 24 || solid >= 4096) continue;
		for (int c = 0; c < 4; c++) ch[c][solid] = (uint8_t)(p >> (8 * c));
		solid++;
	}
	// A background runs along nearly all of the edge and fills the corners; a single letter or
	// digit (a narrow rectangle around its own outline) only touches parts of it.
	int corners = 0;
	const int cx[4] = {s->x, s->x + s->w - 1, s->x, s->x + s->w - 1}, cy[4] = {s->y, s->y, s->y + s->h - 1, s->y + s->h - 1};
	for (int i = 0; i < 4; i++) corners += (px[(size_t)cy[i] * w + cx[i]] >> 24) > 24;
	if (s->w < 16 || solid * 4 < n * 3 || corners < 2) return false;  // a word (or a letter) on its own
	uint32_t f = 0;
	for (int c = 0; c < 4; c++) {
		int hist[256] = {0}, m = 0, acc = 0;
		for (int i = 0; i < solid; i++) hist[ch[c][i]]++;
		while (m < 255 && (acc += hist[m]) * 2 < solid) m++;
		f |= (uint32_t)m << (8 * c);
	}
	*fill = f;
	return true;
}

// A panel's letters may reach below its rectangle (the font's descenders run longer than the
// game's: the "g" of "Loading..." lost its tail, and the old one's smear showed under it): they are
// drawn on into the panel below, without clearing anything there. H: dst's height.
static void draw_sprite(uint32_t *dst, int W, int H, const UiSprite *s, int k, const uint32_t *px, int pw, int ph) {
	uint32_t fill = 0;
	bool back = sprite_backdrop(px, pw, ph, s, &fill);
	int x0 = s->x * k, y0 = s->y * k, w = s->w * k, h = s->h * k, hr = h;
	float ring_k = 0;
	for (int i = 0; i < s->nrings; i++) ring_k += fabsf(s->rw[i]) * (float)k;
	if (s->panel) h = hr + hr / 2 < H - y0 ? hr + hr / 2 : H - y0;
	if (h < hr) h = hr;
	uint8_t *cov = calloc((size_t)w * h, 1);
	const stbtt_fontinfo *fi = s->font == 1 ? &font_regular : &font;
	float scale = stbtt_ScaleForMappingEmToPixels(fi, s->em * (float)k), sxs = scale * s->sx;
	float pen = s->ox * (float)k;
	int base = (int)lroundf(s->oy * (float)k);
	int line = 0, ink_end = hr;  // (the last row with ink below the rectangle)
	for (const char *p = s->text; *p;) {
		int ch = utf8_next(&p);
		if (ch == '|') {  // next line: its own origin and stretch
			if (line >= s->nlines) break;
			sxs = scale * s->lsx[line];
			pen = s->lox[line] * (float)k;
			base = (int)lroundf(s->loy[line] * (float)k);
			line++;
			continue;
		}
		int adv, lsb;
		stbtt_GetCodepointHMetrics(fi, ch, &adv, &lsb);
		int ix = (int)floorf(pen);
		float sub = pen - (float)ix;
		int bx0, by0, bx1, by1;
		stbtt_GetCodepointBitmapBoxSubpixel(fi, ch, sxs, scale, sub, 0, &bx0, &by0, &bx1, &by1);
		int gw = bx1 - bx0, gh = by1 - by0;
		if (gw > 0 && gh > 0) {
			if ((size_t)(gw * gh) > gbm_cap) gbm_cap = (size_t)(gw * gh) * 2, gbm = realloc(gbm, gbm_cap);
			stbtt_MakeCodepointBitmapSubpixel(fi, gbm, gw, gh, gw, sxs, scale, sub, 0, ch);
			for (int y = 0; y < gh; y++)
				for (int x = 0; x < gw; x++) {
					int px = ix + bx0 + x, py = base + by0 + y;
					if (px < 0 || py < 0 || px >= w || py >= h) continue;
					uint8_t *c = &cov[(size_t)py * w + px];
					if (gbm[y * gw + x] > *c) *c = gbm[y * gw + x];
					if (py >= hr && gbm[y * gw + x] && py + 1 > ink_end) ink_end = py + 1;
				}
		}
		pen += (float)adv * sxs;
		if (*p && *p != '|') {
			const char *q = p;
			pen += (float)stbtt_GetCodepointKernAdvance(fi, ch, utf8_next(&q)) * sxs;
		}
	}
	// Fill rows (for the gradient) and distance to the letters (for the outlines).
	int top = h, bot = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (cov[(size_t)y * w + x] >= 128) {
				if (y < top) top = y;
				bot = y;
			}
	// below the rectangle: as far as the letters and their outlines go
	h = ink_end > hr ? (ink_end + (int)ceilf(ring_k) + 2 < h ? ink_end + (int)ceilf(ring_k) + 2 : h) : hr;
	float *dist = NULL, *rdist = NULL;
	uint8_t *rcov = cov;  // what the outlines go around: the letters, extruded downward by ext
	float ring_total = 0;
	for (int i = 0; i < s->nrings; i++) ring_total += fabsf(s->rw[i]);
	if (s->nrings || s->bold > 0 || s->keep || back) {
		dist = malloc(sizeof *dist * (size_t)w * h);
		for (size_t i = 0; i < (size_t)w * h; i++) dist[i] = cov[i] >= 128 ? 0.f : 1e12f;
		edt(dist, w, h);
		rdist = dist;
		int e = (int)lroundf(s->ext * (float)k);
		if (s->nrings && e > 0) {
			rcov = malloc((size_t)w * h);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint8_t m = 0;
					for (int j = 0; j <= e && j <= y; j++) {
						uint8_t v = cov[(size_t)(y - j) * w + x];
						if (v > m) m = v;
					}
					rcov[(size_t)y * w + x] = m;
				}
			rdist = malloc(sizeof *rdist * (size_t)w * h);
			for (size_t i = 0; i < (size_t)w * h; i++) rdist[i] = rcov[i] >= 128 ? 0.f : 1e12f;
			edt(rdist, w, h);
		}
	}
	// Transparent, in the outer color, so filtering at the edges doesn't pull in black.
	uint32_t edge = (s->nrings ? s->rc[s->nrings - 1] : s->fbot) & 0xFFFFFFu;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t *d = &dst[(size_t)(y0 + y) * W + x0 + x];
			float c = (float)cov[(size_t)y * w + x] / 255.f;
			if (!s->panel && !s->keep && !back) *d = edge;  // a panel keeps its rebuilt background
			if (dist) {
				// Distance from the letters' edge, in 1x pixels (inside counts as 0); the letters
				// grow by `bold` to match the original stroke weight.
				float dd = c >= 0.5f ? 0.f : (sqrtf(dist[(size_t)y * w + x]) - 0.5f + (0.5f - c)) / (float)k;
				if (s->bold > 0) c = clampf((s->bold - dd) * (float)k + 0.5f, c, 1);
				if (rdist != dist) {
					float rc_ = (float)rcov[(size_t)y * w + x] / 255.f;
					dd = rc_ >= 0.5f ? 0.f : (sqrtf(rdist[(size_t)y * w + x]) - 0.5f + (0.5f - rc_)) / (float)k;
				}
				if (s->bold > 0) dd = dd > s->bold ? dd - s->bold : 0;
				if ((s->keep || back) && dd <= ring_total + 1.f) *d = back ? fill : edge;  // the old letters, not the art beside them
				float r = ring_total;
				for (int i = s->nrings - 1; i >= 0; i--) {  // outermost first
					float wdt = fabsf(s->rw[i]);
					float a = s->rw[i] < 0 ? clampf((r - dd) / wdt, 0, 1)  // glow: fades toward the outside
					                       : clampf((r - dd) * (float)k + 0.5f, 0, 1);
					over(d, s->rc[i], a);
					r -= wdt;
				}
			}
			float t = bot > top ? clampf((float)(y - top) / (float)(bot - top), 0, 1) : 0;
			if (!(s->ftop >> 24) && !(s->fbot >> 24))  // hollow letters: only their outlines show
				*d = (*d & 0xFFFFFFu) | (uint32_t)clampf((float)(*d >> 24) * (1 - c) + 0.5f, 0, 255) << 24;
			else
				over(d, lerp_rgba(s->ftop, s->fbot, t), c);
		}
	if (rdist != dist) free(rdist), free(rcov);
	free(dist);
	free(cov);
}

// Rebuilds the masked pixels of a panel sprite from the background around them: inverse-distance
// interpolation between the nearest unmasked pixels along the row and the column, then smoothing.
static float color_diff(uint32_t a, uint32_t b) {  // RGBA distance, straight
	float d = 0;
	for (int i = 0; i < 32; i += 8) d += fabsf((float)((a >> i) & 0xFF) - (float)((b >> i) & 0xFF));
	return d;
}

static void inpaint(uint32_t *img, int W, int H, const UiSprite *s) {
	int bx = s->x + s->mx, by = s->y + s->my;
	int w = s->mw, h = s->mh;
	float *c = malloc(sizeof *c * (size_t)w * h * 4);
	uint8_t *got = calloc((size_t)w * h, 1);  // the pixels with a value (the unmasked, the rebuilt)
	#define INSPRITE(xx, yy) ((xx) >= s->x && (yy) >= s->y && (xx) < s->x + s->w && (yy) < s->y + s->h)
	// A source pixel: in the sprite, or around it if not transparent (a panel whose letters fill
	// its whole rectangle, a name over a picture: the picture just outside; before, nothing was
	// left to rebuild from and the rectangle came out see-through).
	#define USABLE(xx, yy) (INSPRITE(xx, yy) || ((xx) >= 0 && (yy) >= 0 && (xx) < W && (yy) < H && \
	                                             (img[(size_t)(yy) * W + (xx)] >> 24) > 16))
	// The nearest unmasked pixel in each direction (mask coordinates; -1 or w / h: just outside
	// the mask's box), by one sweep per row and per column.
	int *near = malloc(sizeof *near * (size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		int last = -1;
		for (int x = 0; x < w; x++) {
			if (!s->mask[y * w + x]) last = x;
			near[((size_t)y * w + x) * 4 + 0] = last;
		}
		last = w;
		for (int x = w - 1; x >= 0; x--) {
			if (!s->mask[y * w + x]) last = x;
			near[((size_t)y * w + x) * 4 + 1] = last;
		}
	}
	for (int x = 0; x < w; x++) {
		int last = -1;
		for (int y = 0; y < h; y++) {
			if (!s->mask[y * w + x]) last = y;
			near[((size_t)y * w + x) * 4 + 2] = last;
		}
		last = h;
		for (int y = h - 1; y >= 0; y--) {
			if (!s->mask[y * w + x]) last = y;
			near[((size_t)y * w + x) * 4 + 3] = last;
		}
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float *o = &c[((size_t)y * w + x) * 4];
			int gx = bx + x, gy = by + y;
			if (!s->mask[y * w + x]) {
				uint32_t p = img[(size_t)gy * W + gx];
				for (int i = 0; i < 4; i++) o[i] = (float)((p >> (8 * i)) & 0xFF);
				got[y * w + x] = 1;
				continue;
			}
			// inverse-distance weights of those four, when inside the sprite
			const int *nr = &near[((size_t)y * w + x) * 4];
			int tx[4] = {bx + nr[0], bx + nr[1], gx, gx}, ty[4] = {gy, gy, by + nr[2], by + nr[3]};
			int dist[4] = {x - nr[0], nr[1] - x, y - nr[2], nr[3] - y};
			float acc[4] = {0}, wsum = 0;
			// Along the direction whose two ends agree: a border crossing the old letters (the
			// panel's white rim under the "g" of "Loading...") is carried on along itself instead of
			// being mixed with the inside of the panel into a grey smear.
			bool ok[4];
			uint32_t pv[4];
			for (int d = 0; d < 4; d++) {
				ok[d] = USABLE(tx[d], ty[d]);
				pv[d] = ok[d] ? img[(size_t)ty[d] * W + tx[d]] : 0;
			}
			float dh = ok[0] && ok[1] ? color_diff(pv[0], pv[1]) : -1, dv = ok[2] && ok[3] ? color_diff(pv[2], pv[3]) : -1;
			bool use[4] = {ok[0], ok[1], ok[2], ok[3]};
			if (dh >= 0 && dv >= 0) {
				if (dv > 2 * dh + 48) use[2] = use[3] = false;
				else if (dh > 2 * dv + 48) use[0] = use[1] = false;
			}
			for (int d = 0; d < 4; d++) {
				if (!use[d]) continue;
				uint32_t p = pv[d];
				float wt = 1.f / (float)dist[d];
				for (int i = 0; i < 4; i++) acc[i] += wt * (float)((p >> (8 * i)) & 0xFF);
				wsum += wt;
			}
			for (int i = 0; i < 4; i++) o[i] = wsum > 0 ? acc[i] / wsum : 0;
			got[y * w + x] = wsum > 0;
		}
	free(near);
	for (int it = 0; it < 24; it++)
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				if (!s->mask[y * w + x]) continue;
				float acc[4] = {0}, n = 0;
				static const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
				for (int d = 0; d < 4; d++) {
					int xx = x + nb[d][0], yy = y + nb[d][1];
					const float *q;
					float tmp[4];
					if (xx >= 0 && yy >= 0 && xx < w && yy < h) {
						if (!got[yy * w + xx]) continue;  // still empty: it fills in from its neighbors
						q = &c[((size_t)yy * w + xx) * 4];
					} else {
						int gx = bx + xx, gy = by + yy;
						if (!USABLE(gx, gy)) continue;
						uint32_t p = img[(size_t)gy * W + gx];
						for (int i = 0; i < 4; i++) tmp[i] = (float)((p >> (8 * i)) & 0xFF);
						q = tmp;
					}
					// (not across an edge: a neighbor far from this pixel's color stays out)
					const float *me = &c[((size_t)y * w + x) * 4];
					if (got[y * w + x] && fabsf(q[0] - me[0]) + fabsf(q[1] - me[1]) + fabsf(q[2] - me[2]) + fabsf(q[3] - me[3]) > 96) continue;
					for (int i = 0; i < 4; i++) acc[i] += q[i];
					n++;
				}
				if (n > 0) {
					for (int i = 0; i < 4; i++) c[((size_t)y * w + x) * 4 + i] = acc[i] / n;
					got[y * w + x] = 1;
				}
			}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			if (!s->mask[y * w + x]) continue;
			const float *o = &c[((size_t)y * w + x) * 4];
			uint32_t p = 0;
			for (int i = 0; i < 4; i++) p |= (uint32_t)clampf(o[i] + 0.5f, 0, 255) << (8 * i);
			img[(size_t)(by + y) * W + bx + x] = p;
		}
	#undef INSPRITE
	#undef USABLE
	free(got);
	free(c);
}

// Per texel of the atlas (w x h), the sprite it belongs to: x0 y0 x1 y1 (the smallest when sprites
// overlap; all 0 outside them). Upscaling stays within it: an edge doesn't take in the next
// sprite's pixels.
static const uint16_t *ui_rectmap(UiAtlas *a, int w, int h) {
	if (a->rectmap || !a->nrects) return a->rectmap;
	uint16_t *m = calloc((size_t)w * h * 4, sizeof *m);
	int *area = malloc(sizeof *area * (size_t)w * h);
	for (size_t i = 0; i < (size_t)w * h; i++) area[i] = 1 << 30;
	for (int r = 0; r < a->nrects; r++) {
		int x0 = a->rects[r][0], y0 = a->rects[r][1], x1 = x0 + a->rects[r][2], y1 = y0 + a->rects[r][3];
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > w) x1 = w;
		if (y1 > h) y1 = h;
		int ar = (x1 - x0) * (y1 - y0);
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++) {
				size_t i = (size_t)y * w + x;
				if (ar >= area[i]) continue;
				area[i] = ar;
				uint16_t *o = &m[i * 4];
				o[0] = (uint16_t)x0, o[1] = (uint16_t)y0, o[2] = (uint16_t)x1, o[3] = (uint16_t)y1;
			}
	}
	free(area);
	return a->rectmap = m;
}

// Catmull-Rom upscale by k, on premultiplied colors; each output pixel samples only the sprite of
// the texel under it (rects: from ui_rectmap, or NULL). Only texels rx0..rx1-1, ry0..ry1-1 of it
// (upscale: all of them).
static bool upscale_smooth;  // upscale_part always Catmull-Rom (the panels over the HD pictures)
static void upscale_part(const uint32_t *src, int w, int h, int k, const uint16_t *rects, uint32_t *dst, int rx0, int ry0,
                         int rx1, int ry1) {
	int W = w * k;
	if (ge_tex_filter() && !upscale_smooth) {  // the game's textures drawn sharp (ge.c): its own pixels, k x k each
		for (int Y = ry0 * k; Y < ry1 * k; Y++)
			for (int X = rx0 * k; X < rx1 * k; X++) dst[(size_t)Y * W + X] = src[(size_t)(Y / k) * w + X / k];
		return;
	}
	float wts[16][4];
	for (int i = 0; i < k && i < 16; i++) {
		float f = ((float)i + 0.5f) / (float)k - 0.5f;
		f -= floorf(f);
		wts[i][0] = f * (-0.5f + f * (1.0f - 0.5f * f));
		wts[i][1] = 1.0f + f * f * (-2.5f + 1.5f * f);
		wts[i][2] = f * (0.5f + f * (2.0f - 1.5f * f));
		wts[i][3] = f * f * (-0.5f + 0.5f * f);
	}
	for (int Y = ry0 * k; Y < ry1 * k; Y++) {
		int iy = Y % k, y0 = (int)floorf(((float)Y + 0.5f) / (float)k - 0.5f);
		for (int X = rx0 * k; X < rx1 * k; X++) {
			int ix = X % k, x0 = (int)floorf(((float)X + 0.5f) / (float)k - 0.5f);
			int lo_x = 0, hi_x = w - 1, lo_y = 0, hi_y = h - 1;
			if (rects) {
				const uint16_t *r = &rects[((size_t)(Y / k) * w + X / k) * 4];
				if (r[2] > r[0]) lo_x = r[0], hi_x = r[2] - 1, lo_y = r[1], hi_y = r[3] - 1;
			}
			float acc[4] = {0};
			for (int ty = 0; ty < 4; ty++) {
				int yy = y0 - 1 + ty;
				yy = yy < lo_y ? lo_y : yy > hi_y ? hi_y : yy;
				for (int tx = 0; tx < 4; tx++) {
					int xx = x0 - 1 + tx;
					xx = xx < lo_x ? lo_x : xx > hi_x ? hi_x : xx;
					uint32_t p = src[(size_t)yy * w + xx];
					float wgt = wts[ix][tx] * wts[iy][ty], a = (float)(p >> 24) / 255.f;
					for (int c = 0; c < 3; c++) acc[c] += wgt * (float)((p >> (8 * c)) & 0xFF) * a;
					acc[3] += wgt * a;
				}
			}
			float a = clampf(acc[3], 0, 1);
			uint32_t o = (uint32_t)(a * 255.f + 0.5f) << 24;
			for (int c = 0; c < 3; c++) o |= (uint32_t)clampf(a > 0 ? acc[c] / a + 0.5f : 0, 0, 255) << (8 * c);
			dst[(size_t)Y * W + X] = o;
		}
	}
}

static void upscale(const uint32_t *src, int w, int h, int k, const uint16_t *rects, uint32_t *dst) {
	upscale_part(src, w, h, k, rects, dst, 0, 0, w, h);
}

static int pow2_at_least(int v) {
	int p = 1;
	while (p < v) p <<= 1;
	return p;
}

// The atlas these decoded pixels are, if any (by the CRC of its rectangle; each rectangle size is
// hashed once).
static UiAtlas *ui_match(const uint32_t *px, int w, int h) {
	vec_defaults();
	if (!vec_ui) return NULL;  // the texture pack's upscaled atlases instead
	ui_load();
	if (ui_state <= 0) return NULL;
	static bool replaced;
	if (!replaced) replaced = true, ui_apply_replacements();
	struct { int w, h; uint32_t crc; } seen[16];
	int n_seen = 0;
	for (int i = 0; i < n_atlases; i++) {
		UiAtlas *a = &atlases[i];
		if (a->w > w || a->h > h || pow2_at_least(a->w) != w || pow2_at_least(a->h) != h) continue;
		if (a->has_sample) {  // 64 pixels before the whole rectangle
			uint32_t smp[64];
			for (int i = 0; i < 64; i++) smp[i] = px[(size_t)(i * a->h / 64) * w + i * a->w / 64];
			if (crc32_rows(smp, 64, 64, 1) != a->sample) continue;
		}
		uint32_t crc = 0;
		int j = 0;
		while (j < n_seen && (seen[j].w != a->w || seen[j].h != a->h)) j++;
		if (j < n_seen) crc = seen[j].crc;
		else {
			crc = crc32_rows(px, w, a->w, a->h);
			if (n_seen < 16) seen[n_seen].w = a->w, seen[n_seen].h = a->h, seen[n_seen++].crc = crc;
		}
		if (getenv("DBZ_UITEXT_DEBUG")) rt_log("text: %dx%d texture vs %s: crc %08x (want %08x)", w, h, a->name, crc, a->crc);
		if (crc == a->crc) return a;
	}
	return NULL;
}

// Panels lose their old text first: a copy of px with their masked pixels rebuilt (NULL: no
// panels). Kept for the next time the game loads the atlas.
static const uint32_t *ui_inpainted(UiAtlas *a, const uint32_t *px, int w, int h) {
	if (a->inpainted_done) return a->inpainted;
	uint32_t *fixed = NULL;
	for (int j = 0; j < a->n; j++) {
		const UiSprite *sp = &a->s[j];
		if (!sp->panel || !sp->mask || sp->x + sp->w > w || sp->y + sp->h > h) continue;
		if (!fixed) {
			fixed = malloc(sizeof *fixed * (size_t)w * h);
			memcpy(fixed, px, sizeof *fixed * (size_t)w * h);
		}
		inpaint(fixed, w, h, sp);
	}
	a->inpainted = fixed, a->inpainted_done = true;
	return fixed;
}

static void ui_dump(const UiAtlas *a, const uint32_t *px, const uint32_t *hi, int w, int h, int k, const char *tag) {
	const char *dump = getenv("DBZ_UITEXT_DUMP");  // review: the atlas before and after
	if (!dump) return;
	char path[600];
	snprintf(path, sizeof path, "%s/%s%s.ppm", dump, a->name, tag);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	int W = w * k, H = h * k;
	fprintf(f, "P6\n%d %d\n255\n", W * 2, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W * 2; x++) {
			uint32_t p = x < W ? px[(size_t)(y / k) * w + x / k] : hi[(size_t)y * W + x - W];
			unsigned al = p >> 24, bg[3] = {90, 90, 140};
			uint8_t c[3];
			for (int i = 0; i < 3; i++) c[i] = (uint8_t)((((p >> (8 * i)) & 0xFF) * al + bg[i] * (255 - al)) / 255);
			fwrite(c, 1, 3, f);
		}
	fclose(f);
}

bool uitext_claims(const uint32_t *px, int w, int h) { return ui_match(px, w, h) != NULL; }

const uint32_t *uitext_hires(const uint32_t *px, int w, int h, int k) {
	UiAtlas *a = k >= 2 ? ui_match(px, w, h) : NULL;
	if (!a) return NULL;
	size_t need = (size_t)w * k * h * k;
	if (need > out_cap) out_cap = need, out = realloc(out, need * 4);
	uint64_t t0 = rt_now_us();
	const uint32_t *fixed = ui_inpainted(a, px, w, h);
	upscale(fixed ? fixed : px, w, h, k, ui_rectmap(a, w, h), out);
	uint64_t t1 = rt_now_us();
	for (int j = 0; j < a->n; j++)
		if (a->s[j].x + a->s[j].w <= w && a->s[j].y + a->s[j].h <= h) draw_sprite(out, w * k, h * k, &a->s[j], k, px, w, h);
	rt_log("text: %s drawn at %dx with %d vector text sprites (%.0f + %.0f ms)", a->name, k, a->n, (t1 - t0) / 1e3,
	       (rt_now_us() - t1) / 1e3);
	ui_dump(a, px, out, w, h, k, "");
	return out;
}

// Both switches on: the texture pack's picture of the atlas (its art upscaled) at the render scale,
// and each word drawn there as over the redrawn atlas: a word on its own clears its rectangle, a
// word over art clears only around its letters (the art beside them stays the pack's); a panel
// (a button: its background rebuilt without the old letters) comes from the redrawn atlas whole.
static uint32_t *over_buf;
static size_t over_cap;

// Where a panel's old letters were (its mask, k times larger and a little wider: the upscaled
// letters' edges), softened: 1 inside, falling to 0 over about k pixels (rw x rh = the panel at k).
static float *panel_mask(const UiSprite *sp, int k) {
	int rw = sp->w * k, rh = sp->h * k, grow = k / 2 > 0 ? k / 2 : 1, rad = k;
	float *m = calloc((size_t)rw * rh, sizeof *m), *t = malloc(sizeof *t * (size_t)rw * rh);
	for (int y = 0; y < rh; y++)
		for (int x = 0; x < rw; x++) {
			int mx = x / k - sp->mx, my = y / k - sp->my;
			if (mx >= 0 && my >= 0 && mx < sp->mw && my < sp->mh && sp->mask[my * sp->mw + mx])
				for (int dy = -grow; dy <= grow; dy++)
					for (int dx = -grow; dx <= grow; dx++)
						if (x + dx >= 0 && y + dy >= 0 && x + dx < rw && y + dy < rh) m[(size_t)(y + dy) * rw + x + dx] = 1;
		}
	for (int pass = 0; pass < 2; pass++) {  // a box blur, rows then columns: the soft edge
		for (int y = 0; y < rh; y++)
			for (int x = 0; x < rw; x++) {
				float acc = 0;
				int n = 0;
				for (int d = -rad; d <= rad; d++) {
					int xx = pass ? x : x + d, yy = pass ? y + d : y;
					if (xx < 0 || yy < 0 || xx >= rw || yy >= rh) continue;
					acc += (pass ? t : m)[(size_t)yy * rw + xx], n++;
				}
				(pass ? m : t)[(size_t)y * rw + x] = acc / (float)n;
			}
	}
	free(t);
	return m;
}

const uint32_t *uitext_over(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih) {
	UiAtlas *a = k >= 2 && hd && iw > 0 && ih > 0 ? ui_match(px, w, h) : NULL;
	if (!a) return NULL;
	uint64_t t0 = rt_now_us();
	int W = w * k, H = h * k;
	size_t need = (size_t)W * H;
	if (need > over_cap) over_cap = need, over_buf = realloc(over_buf, need * 4);
	// The pack's picture at k (as it is when its scale is k, else resampled, bilinear); past the
	// picture (a texture bigger than its art) its last column and row repeat.
	if (pw == iw * k && ph == ih * k) {
		for (int Y = 0; Y < H; Y++) {
			const uint32_t *row = hd + (size_t)(Y < ph ? Y : ph - 1) * pw;
			uint32_t *d = over_buf + (size_t)Y * W;
			int n = pw < W ? pw : W;
			memcpy(d, row, sizeof *d * (size_t)n);
			for (int X = n; X < W; X++) d[X] = row[pw - 1];
		}
	} else {
		float sx = (float)pw / (float)iw, sy = (float)ph / (float)ih;
		for (int Y = 0; Y < H; Y++) {
			float fy = ((float)Y + 0.5f) / (float)k * sy - 0.5f;
			fy = fy > (float)ph - 1 ? (float)ph - 1 : fy < 0 ? 0 : fy;
			int y0 = (int)fy, y1 = y0 + 1 < ph ? y0 + 1 : y0;
			float ty = fy - (float)y0;
			for (int X = 0; X < W; X++) {
				float fx = ((float)X + 0.5f) / (float)k * sx - 0.5f;
				fx = fx > (float)pw - 1 ? (float)pw - 1 : fx < 0 ? 0 : fx;
				int x0 = (int)fx, x1 = x0 + 1 < pw ? x0 + 1 : x0;
				float tx = fx - (float)x0;
				uint32_t c00 = hd[(size_t)y0 * pw + x0], c01 = hd[(size_t)y0 * pw + x1], c10 = hd[(size_t)y1 * pw + x0],
				         c11 = hd[(size_t)y1 * pw + x1], o = 0;
				for (int i = 0; i < 32; i += 8) {
					float v = ((float)((c00 >> i) & 0xFF) * (1 - tx) + (float)((c01 >> i) & 0xFF) * tx) * (1 - ty) +
					          ((float)((c10 >> i) & 0xFF) * (1 - tx) + (float)((c11 >> i) & 0xFF) * tx) * ty;
					o |= (uint32_t)(v + 0.5f) << i;
				}
				over_buf[(size_t)Y * W + X] = o;
			}
		}
	}
	// A panel loses its old letters: where they were (its mask, k times larger and a little wider:
	// the upscaled letters' edges), the background rebuilt without them in the atlas as the game has
	// it, upscaled smoothly, blends into the pack's picture through a soft-edged mask (a hard one
	// showed as blocks); the rest of the panel stays the pack's.
	const uint32_t *fixed = ui_inpainted(a, px, w, h);
	static uint32_t *panel_buf;
	static size_t panel_cap;
	if (fixed && need > panel_cap) panel_cap = need, panel_buf = realloc(panel_buf, need * 4);
	for (int j = 0; fixed && j < a->n; j++) {
		const UiSprite *sp = &a->s[j];
		if (!sp->panel || !sp->mask || sp->x + sp->w > w || sp->y + sp->h > h) continue;
		upscale_smooth = true;
		upscale_part(fixed, w, h, k, ui_rectmap(a, w, h), panel_buf, sp->x, sp->y, sp->x + sp->w, sp->y + sp->h);
		upscale_smooth = false;
		int rw = sp->w * k, rh = sp->h * k;
		float *m = panel_mask(sp, k);
		for (int y = 0; y < rh; y++)
			for (int x = 0; x < rw; x++) {
				float f = m[(size_t)y * rw + x] * 2.f;  // fully the rebuilt background inside the letters' area
				if (f <= 0) continue;
				if (f > 1) f = 1;
				size_t i = (size_t)(sp->y * k + y) * W + sp->x * k + x;
				uint32_t aa = over_buf[i], bb = panel_buf[i], o = 0;
				for (int c = 0; c < 32; c += 8) {
					float v = (float)((aa >> c) & 0xFF) * (1 - f) + (float)((bb >> c) & 0xFF) * f;
					o |= (uint32_t)(v + 0.5f) << c;
				}
				over_buf[i] = o;
			}
		free(m);
	}
	for (int j = 0; j < a->n; j++)
		if (a->s[j].x + a->s[j].w <= w && a->s[j].y + a->s[j].h <= h) draw_sprite(over_buf, W, H, &a->s[j], k, px, w, h);
	rt_log("text: %s over the pack's picture at %dx (%.0f ms)", a->name, k, (rt_now_us() - t0) / 1e3);
	ui_dump(a, px, over_buf, w, h, k, "-hd");
	return over_buf;
}

#ifndef UITEXT_CPU_ONLY
// =============================================================================================
// The same atlases drawn by the GPU. Each glyph's signed distance field (from the font, once)
// lets one shader pass per sprite draw the letters with their outlines, glow and gradient at
// any scale, over the atlas upscaled by the same pass: the CPU only lays the glyphs out
// (a frame stall per atlas becomes a millisecond). The CPU drawing above stays the reference
// (tools/uitext/preview.c renders with it; DBZ_UITEXT_CPU=1 uses it in the game).
// =============================================================================================
#include <GLES3/gl3.h>

#define SDF_EM 64    // glyph distance fields: pixels per em
#define SDF_PAD 50   // their margin: the widest outline in the spec reaches 0.73 em
#define DATA_W 1024  // glyph records texture width (2 texels a glyph)

typedef struct {
	int font, code;
	int x, y, w, h;  // texels in the SDF texture, margin included
	int bx0, by0;    // the glyph's bitmap box at SDF_EM, from the pen at the baseline (y down)
	int ink0, ink1;  // its first and last rows with ink, from by0
} SdfGlyph;

static SdfGlyph *sdf_g;
static int sdf_n, sdf_index[2][256];  // index + 1 for codes < 256
static GLuint gl_prog, gl_vao, gl_fbo, gl_sdf, gl_src, gl_data, gl_rect, gl_hd, gl_pmask;
static int gl_state;  // 0 not tried, 1 ready, -1 unavailable
static GLint u_base, u_k, u_srcSize, u_mode, u_first, u_count, u_bold, u_ringTotal, u_ext, u_nrings, u_rw, u_rc, u_ftop, u_fbot,
	u_edge, u_fill, u_rectBot, u_rows, u_my, u_quad, u_view, u_useRect, u_hdScale, u_useMask;

static const SdfGlyph *sdf_find(int font, int code) {
	if (code < 256) return sdf_index[font][code] ? &sdf_g[sdf_index[font][code] - 1] : NULL;
	for (int i = 0; i < sdf_n; i++)
		if (sdf_g[i].font == font && sdf_g[i].code == code) return &sdf_g[i];
	return NULL;
}

// Distance fields of every character the spec uses, packed in rows into one R16F texture.
static bool sdf_build(void) {
	uint64_t t0 = rt_now_us();
	int cap = 256;
	sdf_g = calloc((size_t)cap, sizeof *sdf_g);
	for (int i = 0; i < n_atlases; i++)
		for (int j = 0; j < atlases[i].n; j++) {
			const UiSprite *s = &atlases[i].s[j];
			const stbtt_fontinfo *fi = s->font == 1 ? &font_regular : &font;
			for (const char *p = s->text; *p;) {
				int ch = utf8_next(&p);
				if (ch == '|' || sdf_find(s->font, ch)) continue;
				float sc = stbtt_ScaleForMappingEmToPixels(fi, SDF_EM);
				int x0, y0, x1, y1;
				stbtt_GetCodepointBitmapBox(fi, ch, sc, sc, &x0, &y0, &x1, &y1);
				if (x1 <= x0 || y1 <= y0) continue;  // a space
				if (sdf_n == cap) cap *= 2, sdf_g = realloc(sdf_g, sizeof *sdf_g * (size_t)cap);
				SdfGlyph *g = &sdf_g[sdf_n++];
				*g = (SdfGlyph){.font = s->font, .code = ch, .w = x1 - x0 + 2 * SDF_PAD, .h = y1 - y0 + 2 * SDF_PAD, .bx0 = x0, .by0 = y0};
				if (ch < 256) sdf_index[s->font][ch] = sdf_n;
			}
		}
	// Rows of glyphs, a texel apart.
	int W = 2048, x = 1, y = 1, row = 0;
	for (int i = 0; i < sdf_n; i++) {
		SdfGlyph *g = &sdf_g[i];
		if (x + g->w + 1 > W) x = 1, y += row + 1, row = 0;
		g->x = x, g->y = y;
		x += g->w + 1;
		if (g->h > row) row = g->h;
	}
	int H = y + row + 1;
	GLint maxs = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxs);
	if (H > maxs) return false;
	float *tex = malloc(sizeof *tex * (size_t)W * H);
	for (size_t i = 0; i < (size_t)W * H; i++) tex[i] = 1000.f;  // far from every glyph
	for (int i = 0; i < sdf_n; i++) {
		SdfGlyph *g = &sdf_g[i];
		const stbtt_fontinfo *fi = g->font == 1 ? &font_regular : &font;
		float sc = stbtt_ScaleForMappingEmToPixels(fi, SDF_EM);
		int gw = g->w, gh = g->h;
		uint8_t *cov = calloc((size_t)gw * gh, 1);
		stbtt_MakeCodepointBitmap(fi, cov + SDF_PAD * gw + SDF_PAD, gw - 2 * SDF_PAD, gh - 2 * SDF_PAD, gw, sc, sc, g->code);
		float *dout = malloc(sizeof *dout * (size_t)gw * gh), *din = malloc(sizeof *din * (size_t)gw * gh);
		g->ink0 = gh, g->ink1 = -1;
		for (int yy = 0; yy < gh; yy++)
			for (int xx = 0; xx < gw; xx++) {
				size_t k = (size_t)yy * gw + xx;
				bool in = cov[k] >= 128;
				dout[k] = in ? 0.f : 1e12f;
				din[k] = in ? 1e12f : 0.f;
				if (in) {
					if (yy - SDF_PAD < g->ink0) g->ink0 = yy - SDF_PAD;
					g->ink1 = yy - SDF_PAD;
				}
			}
		edt(dout, gw, gh);
		edt(din, gw, gh);
		// Signed distance from the outline (texels; inside negative), each pixel's coverage
		// placing the edge within it.
		for (int yy = 0; yy < gh; yy++)
			for (int xx = 0; xx < gw; xx++) {
				size_t k = (size_t)yy * gw + xx;
				float c = (float)cov[k] / 255.f;
				float d = c < 0.5f ? sqrtf(dout[k]) - c - 0.5f : -(sqrtf(din[k]) + c - 1.5f);
				tex[(size_t)(g->y + yy) * W + g->x + xx] = d;
			}
		free(cov), free(dout), free(din);
	}
	glGenTextures(1, &gl_sdf);
	glBindTexture(GL_TEXTURE_2D, gl_sdf);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, W, H, 0, GL_RED, GL_FLOAT, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(tex);
	rt_log("text: %d glyph distance fields (%dx%d, %.0f ms)", sdf_n, W, H, (rt_now_us() - t0) / 1e3);
	return true;
}

static const char *UI_VS =
	"#version 300 es\n"
	"uniform vec4 u_quad;\n"  // x0 y0 x1 y1, pixels of the target
	"uniform vec2 u_view;\n"
	"void main() {\n"
	"  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
	"  gl_Position = vec4(mix(u_quad.xy, u_quad.zw, c) / u_view * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

// Mirrors upscale() and draw_sprite() above.
static const char *UI_FS =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform highp sampler2D u_src;\n"   // the atlas, 1x
	"uniform highp sampler2D u_sdf;\n"   // glyph distance fields
	"uniform highp usampler2D u_rect;\n"  // per texel of the atlas, its sprite: x0 y0 x1 y1 (u_useRect)
	"uniform int u_useRect;\n"
	"uniform highp sampler2D u_data;\n"  // glyph records: texel 0 the glyph's box in u_sdf, texel 1 the map from target pixels to it
	"uniform int u_base;\n"  // the background: 0 Catmull-Rom, 1 xBR, 2 its own pixels (ge.c's filter for the game's textures), 3 the pack's picture
	"uniform highp sampler2D u_hd;\n"  // the pack's picture (u_base 3), u_hdScale of its pixels per texel of the atlas
	"uniform vec2 u_hdScale;\n"
	"uniform highp sampler2D u_pmask;\n"  // over it (u_useMask): 1 where a panel's old letters were, per texel of the atlas
	"uniform int u_useMask;\n"
	"uniform int u_k, u_mode, u_first, u_count, u_ext, u_nrings;\n"  // mode: 0 upscale, 1 text replacing the rectangle, 2 over a panel, 3 clearing only near the letters, 4 the same to the background's color (sprite_backdrop)
	"uniform ivec2 u_srcSize;\n"
	"uniform float u_bold, u_ringTotal, u_my;\n"
	"uniform float u_rw[4];\n"
	"uniform vec4 u_rc[4];\n"
	"uniform vec4 u_ftop, u_fbot, u_edge, u_fill;\n"
	"uniform float u_rectBot;\n"  // the sprite's last row + 1: below it (a panel's long letters) nothing is cleared
	"uniform vec2 u_rows;\n"
	"out vec4 o;\n"
	"vec4 cr(float f) {\n"
	"  return vec4(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2.0 - 1.5 * f)), f * f * (-0.5 + 0.5 * f));\n"
	"}\n"
	// xBR (as ge.c draws the game's textures with it), within the texel's sprite
	"ivec2 xe, xlo, xhi;\n"
	"vec4 X(int dx, int dy) { return texelFetch(u_src, clamp(xe + ivec2(dx, dy), xlo, xhi), 0); }\n"
	"float Y(vec4 c) { return dot(c.rgb * c.a, vec3(14.352, 28.176, 5.472)) + 32.0 * c.a; }\n"
	"vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) { return abs(a - b) + abs(a - c) + abs(d - e) + abs(d - f) + 4.0 * abs(g - h); }\n"
	"bvec4 ne(vec4 a, vec4 b) { return greaterThanEqual(abs(a - b), vec4(15.0)); }\n"
	"bvec4 eq(vec4 a, vec4 b) { return lessThan(abs(a - b), vec4(15.0)); }\n"
	"bvec4 and4(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }\n"
	"bvec4 or4(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }\n"
	"vec4 xbrUp(vec2 tc) {\n"
	"  xe = ivec2(floor(tc)); vec2 fp = tc - floor(tc);\n"
	"  vec4 A1 = X(-1,-2), B1 = X(0,-2), C1 = X(1,-2);\n"
	"  vec4 A0 = X(-2,-1), A = X(-1,-1), B = X(0,-1), C = X(1,-1), C4 = X(2,-1);\n"
	"  vec4 D0 = X(-2, 0), D = X(-1, 0), E = X(0, 0), F = X(1, 0), F4 = X(2, 0);\n"
	"  vec4 G0 = X(-2, 1), G = X(-1, 1), H = X(0, 1), I = X(1, 1), I4 = X(2, 1);\n"
	"  vec4 G5 = X(-1, 2), H5 = X(0, 2), I5 = X(1, 2);\n"
	"  vec4 b = vec4(Y(B), Y(D), Y(H), Y(F)), c = vec4(Y(C), Y(A), Y(G), Y(I));\n"
	"  vec4 e = vec4(Y(E)), d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;\n"
	"  vec4 i4 = vec4(Y(I4), Y(C1), Y(A0), Y(G5)), i5 = vec4(Y(I5), Y(C4), Y(A1), Y(G0));\n"
	"  vec4 h5 = vec4(Y(H5), Y(F4), Y(B1), Y(D0)), f4 = h5.yzwx;\n"
	"  vec4 fx = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(1.0, 1.0, -1.0, -1.0) * fp.x;\n"
	"  vec4 fxl = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(0.5, 2.0, -0.5, -2.0) * fp.x;\n"
	"  vec4 fxu = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(2.0, 0.5, -2.0, -0.5) * fp.x;\n"
	"  bvec4 r0 = and4(ne(e, f), ne(e, h));\n"
	"  bvec4 r1 = and4(r0, or4(or4(and4(ne(f, b), ne(h, d)), and4(and4(eq(e, i), ne(f, i4)), ne(h, i5))), or4(eq(e, g), eq(e, c))));\n"
	"  bvec4 r2l = and4(ne(e, g), ne(d, g)), r2u = and4(ne(e, c), ne(b, c));\n"
	"  float dl = clamp(1.0 / float(u_k), 0.05, 1.0);\n"
	"  vec4 delta = vec4(dl), deltaL = vec4(0.5, 1.0, 0.5, 1.0) * dl, deltaU = deltaL.yxwz;\n"
	"  vec4 fx45i = clamp((fx + delta - vec4(1.5, 0.5, -0.5, 0.5) - 0.25) / (2.0 * delta), 0.0, 1.0);\n"
	"  vec4 fx45 = clamp((fx + delta - vec4(1.5, 0.5, -0.5, 0.5)) / (2.0 * delta), 0.0, 1.0);\n"
	"  vec4 fx30 = clamp((fxl + deltaL - vec4(1.0, 1.0, -0.5, 0.0)) / (2.0 * deltaL), 0.0, 1.0);\n"
	"  vec4 fx60 = clamp((fxu + deltaU - vec4(2.0, 0.0, -1.0, 0.5)) / (2.0 * deltaU), 0.0, 1.0);\n"
	"  vec4 wd1 = wd(e, c, g, i, h5, f4, h, f), wd2 = wd(h, d, i5, f, i4, b, e, i);\n"
	"  bvec4 edri = and4(lessThanEqual(wd1, wd2), r0), edr = and4(lessThan(wd1, wd2), r1);\n"
	"  bvec4 edrl = and4(and4(lessThanEqual(2.0 * abs(f - g), abs(h - c)), r2l), edr);\n"
	"  bvec4 edru = and4(and4(greaterThanEqual(abs(f - g), 2.0 * abs(h - c)), r2u), edr);\n"
	"  fx45 *= vec4(edr); fx30 *= vec4(edrl); fx60 *= vec4(edru); fx45i *= vec4(edri);\n"
	"  bvec4 px = lessThanEqual(abs(e - f), abs(e - h));\n"
	"  vec4 m = max(max(fx30, fx60), max(fx45, fx45i));\n"
	"  vec4 r1c = mix(E, px.x ? F : H, m.x); r1c = mix(r1c, px.z ? D : B, m.z);\n"
	"  vec4 r2c = mix(E, px.y ? B : F, m.y); r2c = mix(r2c, px.w ? H : D, m.w);\n"
	"  vec4 d1 = abs(E - r1c), d2 = abs(E - r2c);\n"
	"  return dot(d1, vec4(1.0)) >= dot(d2, vec4(1.0)) ? r1c : r2c;\n"
	"}\n"
	"vec4 crUp(ivec2 P, ivec2 lo, ivec2 hi) {\n"  // Catmull-Rom on premultiplied colors, within lo..hi
	"  vec2 f = (vec2(P) + 0.5) / float(u_k) - 0.5;\n"
	"  vec2 b = floor(f);\n"
	"  f -= b;\n"
	"  vec4 wx = cr(f.x), wy = cr(f.y), acc = vec4(0.0);\n"
	"  for (int j = 0; j < 4; j++) {\n"
	"    int yy = clamp(int(b.y) - 1 + j, lo.y, hi.y);\n"
	"    vec4 row = vec4(0.0);\n"
	"    for (int i = 0; i < 4; i++) {\n"
	"      vec4 t = texelFetch(u_src, ivec2(clamp(int(b.x) - 1 + i, lo.x, hi.x), yy), 0);\n"
	"      row += wx[i] * vec4(t.rgb * t.a, t.a);\n"
	"    }\n"
	"    acc += wy[j] * row;\n"
	"  }\n"
	"  float a = clamp(acc.a, 0.0, 1.0);\n"
	"  return vec4(a > 0.0 ? clamp(acc.rgb / a, 0.0, 1.0) : vec3(0.0), a);\n"
	"}\n"
	"vec4 upscaled(ivec2 P) {\n"  // the background at P, within the texel's sprite
	"  ivec2 lo = ivec2(0), hi = u_srcSize - 1;\n"
	"  if (u_useRect == 1) {\n"
	"    uvec4 r = texelFetch(u_rect, P / u_k, 0);\n"
	"    if (r.z > r.x) { lo = ivec2(r.xy); hi = ivec2(r.zw) - 1; }\n"
	"  }\n"
	"  if (u_base == 3) {\n"  // the pack's picture; where a panel's old letters were, the atlas rebuilt without them
	"    vec2 t = (vec2(P) + 0.5) / float(u_k);\n"  // in the atlas's texels
	"    vec4 c = texture(u_hd, t * u_hdScale / vec2(textureSize(u_hd, 0)));\n"
	"    if (u_useMask == 1) {\n"  // the letters' mask, softened over a texel or so (uitext_over's panel_mask)
	"      vec2 ms = vec2(textureSize(u_pmask, 0));\n"
	"      float v = 0.25 * (texture(u_pmask, (t + vec2(-0.5, -0.5)) / ms).r + texture(u_pmask, (t + vec2(0.5, -0.5)) / ms).r +\n"
	"                        texture(u_pmask, (t + vec2(-0.5, 0.5)) / ms).r + texture(u_pmask, (t + vec2(0.5, 0.5)) / ms).r);\n"
	"      float f = min(1.0, 2.0 * v);\n"
	"      if (f > 0.0) c = mix(c, crUp(P, lo, hi), f);\n"
	"    }\n"
	"    return c;\n"
	"  }\n"
	"  if (u_base == 2) return texelFetch(u_src, clamp(P / u_k, lo, hi), 0);\n"
	"  if (u_base == 1) { xlo = lo; xhi = hi; return xbrUp((vec2(P) + 0.5) / float(u_k)); }\n"
	"  return crUp(P, lo, hi);\n"
	"}\n"
	"vec4 over(vec4 d, vec4 s, float a) {\n"
	"  float sa = a * s.a;\n"
	"  if (sa <= 0.0) return d;\n"
	"  float oa = sa + d.a * (1.0 - sa);\n"
	"  return vec4((s.rgb * sa + d.rgb * d.a * (1.0 - sa)) / (oa > 0.0 ? oa : 1.0), oa);\n"
	"}\n"
	"float sdf(vec2 uv) { return texture(u_sdf, uv / vec2(textureSize(u_sdf, 0))).r; }\n"
	"void main() {\n"
	"  ivec2 P = ivec2(gl_FragCoord.xy);\n"
	"  vec4 bg = upscaled(P);\n"
	"  if (u_mode == 0) { o = bg; return; }\n"
	"  vec2 p = gl_FragCoord.xy;\n"
	// The nearest glyph (and, for outlines reaching deeper below, the nearest of the letters
	// moved down by up to u_ext pixels).
	"  float best = 1e4, bestExt = 1e4;\n"
	"  vec2 buv = vec2(0.0), bm = vec2(1.0);\n"
	"  bool found = false;\n"
	"  for (int i = 0; i < u_count; i++) {\n"
	"    int t = 2 * (u_first + i);\n"
	"    vec4 box = texelFetch(u_data, ivec2(t % DATA_W, t / DATA_W), 0);\n"
	"    vec4 m = texelFetch(u_data, ivec2((t + 1) % DATA_W, (t + 1) / DATA_W), 0);\n"
	"    vec2 uv = m.xy + p * m.zw;\n"
	"    if (uv.x < box.x || uv.x > box.z || uv.y < box.y || uv.y - float(u_ext) * m.w > box.w) continue;\n"
	"    float d = 1e4;\n"
	"    if (uv.y <= box.w) {\n"
	"      d = sdf(uv);\n"
	"      if (d < best) { best = d; buv = uv; bm = m.zw; found = true; }\n"
	"    }\n"
	"    for (int j = 1; j <= u_ext; j++) {\n"
	"      float v = uv.y - float(j) * m.w;\n"
	"      if (v < box.y) break;\n"
	"      if (v <= box.w) d = min(d, sdf(vec2(uv.x, v)));\n"
	"    }\n"
	"    bestExt = min(bestExt, d);\n"
	"  }\n"
	// To target pixels; a stretched glyph's distances along x scale with it (first order: the
	// field's gradient where the nearest glyph is).
	"  float corr = 1.0;\n"
	"  if (found && abs(bm.x - bm.y) > 1e-4 * bm.y) {\n"
	"    vec2 g = vec2(sdf(buv + vec2(1.0, 0.0)) - sdf(buv - vec2(1.0, 0.0)), sdf(buv + vec2(0.0, 1.0)) - sdf(buv - vec2(0.0, 1.0)));\n"
	"    float gl = length(g);\n"
	"    if (gl > 1e-3) corr = 1.0 / length(vec2(g.x / gl * bm.x / bm.y, g.y / gl));\n"
	"  }\n"
	"  float d = best / u_my * corr, de = bestExt / u_my * corr, kf = float(u_k);\n"
	"  float c = clamp(0.5 - d, 0.0, 1.0);\n"
	"  bool below = gl_FragCoord.y >= u_rectBot;\n"
	"  vec4 dst = u_mode == 1 && !below ? u_edge : bg;\n"
	"  if (u_nrings > 0 || u_bold > 0.0 || u_mode >= 3) {\n"
	"    float dd = d > 0.0 ? (d + 0.5) / kf : 0.0;\n"  // 1x pixels from the letters, as draw_sprite measures
	"    if (u_bold > 0.0) c = clamp((u_bold - dd) * kf + 0.5, c, 1.0);\n"
	"    if (u_ext > 0 && u_nrings > 0) dd = de > 0.0 ? (de + 0.5) / kf : 0.0;\n"
	"    if (u_bold > 0.0) dd = dd > u_bold ? dd - u_bold : 0.0;\n"
	"    if (u_mode >= 3 && !below && dd <= u_ringTotal + 1.0) dst = u_mode == 4 ? u_fill : u_edge;\n"
	"    float r = u_ringTotal;\n"
	"    for (int i = 3; i >= 0; i--) {\n"
	"      if (i >= u_nrings) continue;\n"
	"      float w = abs(u_rw[i]);\n"
	"      float a = u_rw[i] < 0.0 ? clamp((r - dd) / w, 0.0, 1.0) : clamp((r - dd) * kf + 0.5, 0.0, 1.0);\n"
	"      dst = over(dst, u_rc[i], a);\n"
	"      r -= w;\n"
	"    }\n"
	"  }\n"
	"  float t = u_rows.y > u_rows.x ? clamp((p.y - 0.5 - u_rows.x) / (u_rows.y - u_rows.x), 0.0, 1.0) : 0.0;\n"
	"  o = u_ftop.a == 0.0 && u_fbot.a == 0.0 ? vec4(dst.rgb, dst.a * (1.0 - c))\n"  // hollow letters
	"                                         : over(dst, mix(u_ftop, u_fbot, t), c);\n"
	"}\n";

static GLuint ui_shader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	char def[64];
	snprintf(def, sizeof def, "#define DATA_W %d\n", DATA_W);
	const char *nl = strchr(src, '\n') + 1;  // after #version
	const char *parts[3] = {src, def, nl};
	GLint lens[3] = {(GLint)(nl - src), -1, -1};
	glShaderSource(s, 3, parts, lens);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		rt_log("text: shader: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool gl_ready(void) {
	if (gl_state) return gl_state > 0;
	gl_state = -1;
	if (getenv("DBZ_UITEXT_CPU")) return false;
	GLuint vs = ui_shader(GL_VERTEX_SHADER, UI_VS), fs = ui_shader(GL_FRAGMENT_SHADER, UI_FS);
	if (!vs || !fs) return false;
	gl_prog = glCreateProgram();
	glAttachShader(gl_prog, vs);
	glAttachShader(gl_prog, fs);
	glLinkProgram(gl_prog);
	GLint ok;
	glGetProgramiv(gl_prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		rt_log("text: shader link failed");
		return false;
	}
	if (!sdf_build()) return false;
#define U(n) n = glGetUniformLocation(gl_prog, #n)
	U(u_base); U(u_k); U(u_srcSize); U(u_mode); U(u_first); U(u_count); U(u_bold); U(u_ringTotal); U(u_ext); U(u_nrings);
	U(u_rw); U(u_rc); U(u_ftop); U(u_fbot); U(u_edge); U(u_fill); U(u_rectBot); U(u_rows); U(u_my); U(u_quad); U(u_view); U(u_useRect); U(u_hdScale); U(u_useMask);
#undef U
	glUseProgram(gl_prog);
	glUniform1i(glGetUniformLocation(gl_prog, "u_src"), 1);
	glUniform1i(glGetUniformLocation(gl_prog, "u_sdf"), 2);
	glUniform1i(glGetUniformLocation(gl_prog, "u_data"), 3);
	glUniform1i(glGetUniformLocation(gl_prog, "u_rect"), 4);
	glUniform1i(glGetUniformLocation(gl_prog, "u_hd"), 5);
	glUniform1i(glGetUniformLocation(gl_prog, "u_pmask"), 6);
	glGenVertexArrays(1, &gl_vao);
	glGenFramebuffers(1, &gl_fbo);
	glGenTextures(1, &gl_src);
	glGenTextures(1, &gl_data);
	glGenTextures(1, &gl_rect);
	GLuint nearest[3] = {gl_src, gl_data, gl_rect};
	for (int i = 0; i < 3; i++) {
		glBindTexture(GL_TEXTURE_2D, nearest[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	GLuint smooth[2] = {gl_hd, gl_pmask};  // the pack's picture, the panels' mask: sampled smoothly, edges repeated
	glGenTextures(2, smooth);
	gl_hd = smooth[0], gl_pmask = smooth[1];
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, smooth[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	gl_state = 1;
	return true;
}

static void rgba4(uint32_t c, float *o) {
	for (int i = 0; i < 4; i++) o[i] = (float)((c >> (8 * i)) & 0xFF) / 255.f;
}

typedef struct {
	int first, count;
	float top, bot, my;
} SpriteRun;

// Glyph records of one sprite (as draw_sprite places its glyphs), appended to *rec.
static SpriteRun layout_sprite(const UiSprite *s, int k, float **rec, int *nrec, int *caprec) {
	SpriteRun r = {*nrec, 0, 1e9f, -1e9f, 1};
	const stbtt_fontinfo *fi = s->font == 1 ? &font_regular : &font;
	float scale = stbtt_ScaleForMappingEmToPixels(fi, s->em * (float)k), sxs = scale * s->sx;
	float se = stbtt_ScaleForMappingEmToPixels(fi, SDF_EM);
	float x0 = (float)(s->x * k), y0 = (float)(s->y * k);
	float pen = s->ox * (float)k;
	int base = (int)lroundf(s->oy * (float)k);
	int line = 0;
	r.my = se / scale;
	for (const char *p = s->text; *p;) {
		int ch = utf8_next(&p);
		if (ch == '|') {
			if (line >= s->nlines) break;
			sxs = scale * s->lsx[line];
			pen = s->lox[line] * (float)k;
			base = (int)lroundf(s->loy[line] * (float)k);
			line++;
			continue;
		}
		const SdfGlyph *g = sdf_find(s->font, ch);
		if (g) {
			if (*nrec == *caprec) *caprec = *caprec ? *caprec * 2 : 1024, *rec = realloc(*rec, sizeof **rec * 8 * (size_t)*caprec);
			float *o = *rec + 8 * (size_t)(*nrec)++;
			// u = (X - (x0 + pen)) * se / sxs + (g->x + SDF_PAD - bx0), v likewise down from the
			// baseline: X, Y target pixels (centers at +0.5).
			float mx = se / sxs, my = se / scale;
			o[0] = (float)g->x, o[1] = (float)g->y, o[2] = (float)(g->x + g->w), o[3] = (float)(g->y + g->h);
			o[4] = (float)(g->x + SDF_PAD - g->bx0) - (x0 + pen) * mx;
			o[5] = (float)(g->y + SDF_PAD - g->by0) - (y0 + (float)base) * my;
			o[6] = mx, o[7] = my;
			r.count++;
			// rows with ink (sprite pixels), for the gradient
			float t = (float)base + (float)(g->by0 + g->ink0) / my, b = (float)base + (float)(g->by0 + g->ink1 + 1) / my - 1.f;
			if (t < r.top) r.top = t;
			if (b > r.bot) r.bot = b;
		}
		int adv, lsb;
		stbtt_GetCodepointHMetrics(fi, ch, &adv, &lsb);
		pen += (float)adv * sxs;
		if (*p && *p != '|') {
			const char *q = p;
			pen += (float)stbtt_GetCodepointKernAdvance(fi, ch, utf8_next(&q)) * sxs;
		}
	}
	return r;
}

static bool atlas_gl(UiAtlas *a, const uint32_t *px, int w, int h, int k, unsigned tex, int hd_scale);
static bool hd_mask;  // uitext_over_gl: the panels' mask is in gl_pmask

bool uitext_hires_gl(const uint32_t *px, int w, int h, int k, unsigned tex) {
	UiAtlas *a = k >= 2 ? ui_match(px, w, h) : NULL;
	if (!a || !gl_ready()) return false;
	return atlas_gl(a, px, w, h, k, tex, 0);
}

// The same over the pack's picture (hd: pw x ph pixels for the iw x ih texels of the w x h atlas,
// hd_scale = pw / iw of them per texel): what uitext_over makes on the CPU, here on the GPU. The
// picture goes up as it is, with a mask of the panels' old letters (a byte a texel); the shader
// takes the picture as the background (u_base 3), the atlas rebuilt without those letters where
// the mask says (softened as uitext_over's panel_mask), and draws every sprite over it. A new
// screen's atlases took a fifth of a second each on the CPU (the words drawn at 4x): the screen
// froze at every change.
bool uitext_over_gl(const uint32_t *px, int w, int h, int k, const uint32_t *hd, int pw, int ph, int iw, int ih, unsigned tex) {
	static int cpu = -1;  // DBZ_UITEXT_OVER_CPU=1 (tests): uitext_over on the CPU, as before
	if (cpu < 0) cpu = getenv("DBZ_UITEXT_OVER_CPU") != NULL;
	UiAtlas *a = !cpu && k >= 2 && hd && iw > 0 && ih > 0 ? ui_match(px, w, h) : NULL;
	if (!a || !gl_ready()) return false;
	int sc = pw / iw;
	if (sc < 1 || sc > 8 || pw != iw * sc || ph != ih * sc) return false;
	uint64_t t0 = rt_now_us();
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, gl_hd);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, hd);
	// The panels' letters, one byte a texel of the atlas (the shader rebuilds the background there).
	static uint8_t *mask;
	static size_t mask_cap;
	if ((size_t)w * h > mask_cap) mask_cap = (size_t)w * h, mask = realloc(mask, mask_cap);
	memset(mask, 0, (size_t)w * h);
	bool any = false;
	for (int j = 0; j < a->n; j++) {
		const UiSprite *sp = &a->s[j];
		if (!sp->panel || !sp->mask || sp->x + sp->w > w || sp->y + sp->h > h) continue;
		for (int my = 0; my < sp->mh; my++)
			for (int mx = 0; mx < sp->mw; mx++) {
				int x = sp->x + sp->mx + mx, y = sp->y + sp->my + my;
				if (sp->mask[my * sp->mw + mx] && x >= sp->x && y >= sp->y && x < sp->x + sp->w && y < sp->y + sp->h)
					mask[(size_t)y * w + x] = 255, any = true;
			}
	}
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, gl_pmask);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, any ? w : 1, any ? h : 1, 0, GL_RED, GL_UNSIGNED_BYTE, mask);
	hd_mask = any;
	uint64_t t_base = rt_now_us();
	bool ok = atlas_gl(a, px, w, h, k, tex, sc);
	if (ok) {
		static int logged;
		if (logged++ < 40 || getenv("DBZ_UITEXT_DEBUG"))
			rt_log("text: %s over the pack's picture by the GPU at %dx (%.1f ms: picture and mask %.1f)", a->name, k,
			       (rt_now_us() - t0) / 1e3, (t_base - t0) / 1e3);
	}
	return ok;
}

static bool atlas_gl(UiAtlas *a, const uint32_t *px, int w, int h, int k, unsigned tex, int hd_scale) {
	static int clears = 4;
	if (clears > 0)
		for (clears--; glGetError() != GL_NO_ERROR;) {}  // not ours
	uint64_t t0 = rt_now_us();
	const uint32_t *fixed = ui_inpainted(a, px, w, h);
	uint64_t t_inpaint = rt_now_us();
	int W = w * k, H = h * k;
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gl_src);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fixed ? fixed : px);
	// Glyph records, sprite by sprite.
	static float *rec;
	static int caprec;
	int nrec = 0;
	SpriteRun *runs = malloc(sizeof *runs * (size_t)(a->n ? a->n : 1));
	for (int j = 0; j < a->n; j++) runs[j] = layout_sprite(&a->s[j], k, &rec, &nrec, &caprec);
	uint64_t t_layout = rt_now_us();
	int rows = (2 * nrec + DATA_W - 1) / DATA_W;
	if (rows < 1) rows = 1;
	if ((size_t)rows * DATA_W > (size_t)caprec * 2) caprec = rows * DATA_W / 2, rec = realloc(rec, sizeof *rec * 8 * (size_t)caprec);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, gl_data);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, DATA_W, rows, 0, GL_RGBA, GL_FLOAT, rec);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, gl_sdf);
	const uint16_t *rmap = ui_rectmap(a, w, h);
	if (rmap) {
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, gl_rect);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, rmap);
	}
	// The target: the slot's texture at the render scale.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if (ok) {
		glViewport(0, 0, W, H);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glUseProgram(gl_prog);
		glBindVertexArray(gl_vao);
		glUniform1i(u_k, k);
		glUniform1i(u_base, hd_scale ? 3 : ge_tex_filter());
		glUniform2f(u_hdScale, (float)hd_scale, (float)hd_scale);
		glUniform1i(u_useMask, hd_scale && hd_mask);
		glUniform1i(u_useRect, rmap != NULL);
		glUniform2i(u_srcSize, w, h);
		glUniform2f(u_view, (float)W, (float)H);
		glUniform1i(u_mode, 0);
		glUniform4f(u_quad, 0, 0, (float)W, (float)H);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		for (int j = 0; j < a->n; j++) {
			const UiSprite *s = &a->s[j];
			if (s->x + s->w > w || s->y + s->h > h) continue;
			float rw[4] = {0}, rc[16] = {0}, ft[4], fb[4], edge[4], tot = 0;
			for (int i = 0; i < s->nrings; i++) rw[i] = s->rw[i], rgba4(s->rc[i], rc + 4 * i), tot += fabsf(s->rw[i]);
			rgba4(s->ftop, ft), rgba4(s->fbot, fb);
			rgba4((s->nrings ? s->rc[s->nrings - 1] : s->fbot) & 0xFFFFFFu, edge);
			const SpriteRun *r = &runs[j];
			uint32_t fillc = 0;
			bool back = sprite_backdrop(px, w, h, s, &fillc);  // mode 4: over a background (draw_sprite)
			float fill[4];
			rgba4(fillc, fill);
			glUniform4fv(u_fill, 1, fill);
			glUniform1i(u_mode, s->panel ? 2 : s->keep ? 3 : back ? 4 : 1);
			glUniform1i(u_first, r->first);
			glUniform1i(u_count, r->count);
			glUniform1i(u_ext, s->nrings ? (int)lroundf(s->ext * (float)k) : 0);
			glUniform1i(u_nrings, s->nrings);
			glUniform1f(u_bold, s->bold);
			glUniform1f(u_ringTotal, tot);
			glUniform1f(u_my, r->my);
			glUniform1fv(u_rw, 4, rw);
			glUniform4fv(u_rc, 4, rc);
			glUniform4fv(u_ftop, 1, ft);
			glUniform4fv(u_fbot, 1, fb);
			glUniform4fv(u_edge, 1, edge);
			// the sprite's rows with ink: from the glyph boxes, relative to the sprite
			glUniform2f(u_rows, r->count ? r->top + (float)(s->y * k) : 0, r->count ? r->bot + (float)(s->y * k) : 0);
			// a panel's letters reaching below its rectangle are drawn on there (draw_sprite)
			int hr = s->h * k, below = 0;
			if (s->panel && r->count && r->bot + tot * (float)k + 2 > (float)hr) {
				below = (int)ceilf(r->bot + tot * (float)k + 2) - hr;
				if (below > hr / 2) below = hr / 2;
				if ((s->y + s->h) * k + below > H) below = H - (s->y + s->h) * k;
			}
			glUniform1f(u_rectBot, (float)((s->y + s->h) * k));
			glUniform4f(u_quad, (float)(s->x * k), (float)(s->y * k), (float)((s->x + s->w) * k), (float)((s->y + s->h) * k + below));
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		if (getenv("DBZ_UITEXT_DUMP")) {
			uint32_t *hi = malloc(sizeof *hi * (size_t)W * H);
			glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, hi);
			ui_dump(a, px, hi, w, h, k, "_gpu");
			free(hi);
		}
	}
	static int checks = 4;  // the first atlases tell whether this GL can (asking waits for the GPU)
	GLenum err = checks > 0 ? (checks--, glGetError()) : GL_NO_ERROR;
	if (err != GL_NO_ERROR) {  // the CPU draws it, from now on
		rt_log("text: GL error 0x%x drawing %s: back to the CPU", err, a->name);
		ok = false, gl_state = -1;
	}
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	free(runs);
	if (ok && !hd_scale) {
		static int logged;
		if (logged++ < 40 || getenv("DBZ_UITEXT_DEBUG"))
			rt_log("text: %s drawn by the GPU at %dx with %d sprites (%.1f ms: inpainting %.1f, layout %.1f)", a->name, k,
			       a->n, (rt_now_us() - t0) / 1e3, (t_inpaint - t0) / 1e3, (t_layout - t_inpaint) / 1e3);
	}
	return ok;
}
#endif
