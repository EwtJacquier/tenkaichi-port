// Replacement textures (see texpack.h).
#include "texpack.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "rt.h"

typedef struct {
	uint64_t key;
	uint16_t iw, ih;
	uint8_t lw, lh;  // the texture it is in: log2 of its width and height
	uint8_t state;   // 0 not asked for yet, 1 being fetched (web), 2 failed
	char file[72];
} Entry;

static Entry *ents;
static int nents;
static char dir[512];
static bool remote;  // web: files are fetched from the page's server (textures/<file>)
static bool enabled = true;  // the page's "HD textures" switch

// Distinct picture sizes per texture size (log2 of width and height, up to 1024).
typedef struct {
	uint16_t iw, ih;
} Size;
static Size *sizes[11][11];
static int nsizes[11][11];

// ---- CRC-32 (zlib's, slicing by 8) and Adler-32 -------------------------------------------------
static uint32_t crc_tab[8][256];

static void crc_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_tab[0][i] = c;
	}
	for (int i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++) crc_tab[t][i] = (crc_tab[t - 1][i] >> 8) ^ crc_tab[0][crc_tab[t - 1][i] & 0xff];
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
	crc = ~crc;
	while (n >= 8) {
		uint32_t a, b;
		memcpy(&a, p, 4);
		memcpy(&b, p + 4, 4);
		a ^= crc;
		crc = crc_tab[7][a & 0xff] ^ crc_tab[6][(a >> 8) & 0xff] ^ crc_tab[5][(a >> 16) & 0xff] ^ crc_tab[4][a >> 24] ^
		      crc_tab[3][b & 0xff] ^ crc_tab[2][(b >> 8) & 0xff] ^ crc_tab[1][(b >> 16) & 0xff] ^ crc_tab[0][b >> 24];
		p += 8;
		n -= 8;
	}
	while (n--) crc = crc_tab[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t adler_update(uint32_t adler, const uint8_t *p, size_t n) {
	uint32_t a = adler & 0xffff, b = adler >> 16;
	while (n) {
		size_t k = n < 5552 ? n : 5552;
		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

uint64_t texpack_key(const uint32_t *px, int w, int iw, int ih) {
	uint32_t crc = 0, adler = 1;
	for (int y = 0; y < ih; y++) {
		const uint8_t *row = (const uint8_t *)(px + (size_t)y * w);
		crc = crc_update(crc, row, (size_t)iw * 4);
		adler = adler_update(adler, row, (size_t)iw * 4);
	}
	return (uint64_t)crc << 32 | adler;
}

uint64_t texpack_hash(const void *p, size_t n) {
	return (uint64_t)crc_update(0, p, n) << 32 | adler_update(1, p, n);
}

// CRC-32 and Adler-32 of two pieces put together from each one's (zlib's crc32_combine_op and
// adler32_combine): a picture's key for every one of its possible sizes from a single pass.
static uint32_t multmodp(uint32_t a, uint32_t b) {  // a * b modulo the CRC's polynomial (reflected)
	uint32_t m = 1u << 31, p = 0;
	for (;;) {
		if (a & m) {
			p ^= b;
			if ((a & (m - 1)) == 0) break;
		}
		m >>= 1;
		b = b & 1 ? (b >> 1) ^ 0xEDB88320u : b >> 1;
	}
	return p;
}

static uint32_t crc_shift_op(size_t len) {  // x^(8 len) modulo the polynomial: appending len bytes
	static uint32_t x2n[32];
	if (!x2n[0]) {
		uint32_t p = 1u << 30;  // x^1
		x2n[0] = p;
		for (int n = 1; n < 32; n++) x2n[n] = p = multmodp(p, p);
	}
	uint32_t p = 1u << 31;  // x^0
	unsigned k = 3;
	for (size_t n = len; n; n >>= 1, k++)
		if (n & 1) p = multmodp(x2n[k & 31], p);
	return p;
}

static uint32_t adler_combine(uint32_t a1, uint32_t a2, size_t len2) {
	const uint32_t BASE = 65521;
	uint32_t rem = (uint32_t)(len2 % BASE), sum1 = a1 & 0xffff, sum2 = (uint32_t)(((uint64_t)rem * sum1) % BASE);
	sum1 += (a2 & 0xffff) + BASE - 1;
	sum2 += ((a1 >> 16) & 0xffff) + ((a2 >> 16) & 0xffff) + BASE - rem;
	if (sum1 >= BASE) sum1 -= BASE;
	if (sum1 >= BASE) sum1 -= BASE;
	if (sum2 >= (BASE << 1)) sum2 -= BASE << 1;
	if (sum2 >= BASE) sum2 -= BASE;
	return sum1 | sum2 << 16;
}

// ---- index ------------------------------------------------------------------------------------------
static int log2i(int v) {
	int n = 0;
	while ((1 << n) < v) n++;
	return n;
}

static int cmp_entry(const void *a, const void *b) {
	const Entry *x = a, *y = b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	if (x->iw != y->iw) return x->iw - y->iw;
	return x->ih - y->ih;
}

static struct stat idx_st;  // index.txt as last read

static void index_path(char *path, size_t n) { snprintf(path, n, "%s/index.txt", dir); }

// (Re)reads index.txt; a missing file leaves the pack empty.
static void load_index(void) {
	free(ents);
	ents = NULL;
	nents = 0;
	for (int i = 0; i < 11; i++)
		for (int j = 0; j < 11; j++) free(sizes[i][j]), sizes[i][j] = NULL, nsizes[i][j] = 0;
	char path[600];
	index_path(path, sizeof path);
	FILE *f = fopen(path, "r");
	memset(&idx_st, 0, sizeof idx_st);
	if (!f) return;
	fstat(fileno(f), &idx_st);
	char line[256];
	int cap = 0;
	while (fgets(line, sizeof line, f)) {
		// "key w h file", the picture in the corner of a texture of the next powers of two, or
		// "key w h file tw th" (a picture much smaller than its texture: a portrait 272 texels wide
		// in rows of 272, in a texture 512 x 256)
		char file[72];
		unsigned long long key;
		int iw, ih, tw = 0, th = 0;
		int n = line[0] == '#' ? 0 : sscanf(line, "%16llx %d %d %71s %d %d", &key, &iw, &ih, file, &tw, &th);
		if (n != 4 && n != 6) continue;
		if (iw <= 0 || ih <= 0 || iw > 1024 || ih > 1024 || strchr(file, '/')) continue;
		if (n == 4 || tw < iw || th < ih || tw > 1024 || th > 1024) tw = iw, th = ih;
		if (nents == cap) ents = realloc(ents, sizeof(Entry) * (size_t)(cap = cap ? cap * 2 : 256));
		Entry *e = &ents[nents++];
		memset(e, 0, sizeof *e);
		e->key = key, e->iw = (uint16_t)iw, e->ih = (uint16_t)ih;
		e->lw = (uint8_t)log2i(tw), e->lh = (uint8_t)log2i(th);
		snprintf(e->file, sizeof e->file, "%s", file);
	}
	fclose(f);
	qsort(ents, (size_t)nents, sizeof(Entry), cmp_entry);
	for (int i = 0; i < nents; i++) {
		int lw = ents[i].lw, lh = ents[i].lh;
		Size *s = sizes[lw][lh];
		int n = nsizes[lw][lh], k = 0;
		while (k < n && (s[k].iw != ents[i].iw || s[k].ih != ents[i].ih)) k++;
		if (k < n) continue;
		sizes[lw][lh] = s = realloc(s, sizeof(Size) * (size_t)(n + 1));
		s[n].iw = ents[i].iw, s[n].ih = ents[i].ih;
		nsizes[lw][lh] = n + 1;
	}
	rt_log("texpack: %d replacement texture(s) in %s", nents, dir);
}

void texpack_init(void) {
	crc_init();
	const char *d = getenv("DBZ_TEXTURES");
#ifdef __EMSCRIPTEN__
	remote = true;
	if (!d) return;  // a page without the pack's folder (see web/shell.html)
#else
	if (!d) d = "build/textures";
#endif
	if (!*d || !strcmp(d, "0") || !strcmp(d, "off")) return;
	snprintf(dir, sizeof dir, "%s", d);
	load_index();
}

bool texpack_changed(void) {
	static unsigned n;
	if (!dir[0] || ++n % 60) return false;  // about once a second
	char path[600];
	index_path(path, sizeof path);
	struct stat st;
	if (stat(path, &st) != 0) memset(&st, 0, sizeof st);
	if (st.st_ino == idx_st.st_ino && st.st_size == idx_st.st_size && st.st_mtime == idx_st.st_mtime) return false;
	load_index();
	return true;
}

bool texpack_set_enabled(bool on) {
	if (on == enabled) return false;
	enabled = on;
	rt_log("texpack: HD textures %s", on ? "on" : "off");
	return true;
}

static int find_slow(const uint32_t *px, int w, int h) {  // a key for each possible size, one by one
	int lw = log2i(w), lh = log2i(h);
	for (int k = 0; k < nsizes[lw][lh]; k++) {
		Size s = sizes[lw][lh][k];
		if (s.iw > w || s.ih > h) continue;
		Entry probe = {.key = texpack_key(px, w, s.iw, s.ih), .iw = s.iw, .ih = s.ih};
		Entry *e = bsearch(&probe, ents, (size_t)nents, sizeof(Entry), cmp_entry);
		if (e) return (int)(e - ents);
	}
	return -1;
}

// The pictures a texture of this size may hold come in up to 78 sizes (a 512x512 texture), and the
// key of each one hashed its own corner: 50 MB hashed for a new texture, a fifth of a second (more
// in the browser), every texture a new screen brought. Now each row is hashed once: as the pass
// along it reaches each possible width, the row's CRC and Adler so far (crc_update and
// adler_update go on from where they stopped) join that width's key, combined (zlib's way), and
// at each possible height the key is looked up.
int texpack_find(const uint32_t *px, int w, int h) {
	if (!enabled || !nents) return -1;
	int lw = log2i(w), lh = log2i(h);
	if (lw > 10 || lh > 10 || !nsizes[lw][lh]) return -1;
	static int check = -1;  // DBZ_TEXPACK_CHECK=1 (tests): the old way too, and say when they differ
	if (check < 0) check = getenv("DBZ_TEXPACK_CHECK") != NULL;
	static int slow = -1;  // DBZ_TEXPACK_SLOWFIND=1 (tests): only the old way
	if (slow < 0) slow = getenv("DBZ_TEXPACK_SLOWFIND") != NULL;
	if (slow) return find_slow(px, w, h);
	const Size *sz = sizes[lw][lh];
	int n = nsizes[lw][lh];
	// the distinct widths (sorted) and, for each size, its width's place
	uint16_t widths[256];
	int nw = 0, maxh = 0;
	for (int k = 0; k < n; k++) {
		if (sz[k].iw > w || sz[k].ih > h) continue;
		int i = 0;
		while (i < nw && widths[i] < sz[k].iw) i++;
		if (i < nw && widths[i] == sz[k].iw) continue;
		if (nw == 256) return find_slow(px, w, h);
		memmove(widths + i + 1, widths + i, sizeof *widths * (size_t)(nw - i));
		widths[i] = sz[k].iw, nw++;
	}
	for (int k = 0; k < n; k++)
		if (sz[k].iw <= w && sz[k].ih <= h && sz[k].ih > maxh) maxh = sz[k].ih;
	uint32_t ops[256], kc[256], ka[256];
	for (int i = 0; i < nw; i++) ops[i] = crc_shift_op((size_t)widths[i] * 4), kc[i] = 0, ka[i] = 1;
	int found = -1;
	for (int y = 0; y < maxh && found < 0; y++) {
		const uint8_t *row = (const uint8_t *)(px + (size_t)y * w);
		uint32_t rc = 0, ra = 1;
		int at = 0;
		for (int i = 0; i < nw; i++) {
			size_t len = (size_t)(widths[i] - at) * 4;
			rc = crc_update(rc, row + (size_t)at * 4, len);
			ra = adler_update(ra, row + (size_t)at * 4, len);
			at = widths[i];
			kc[i] = multmodp(ops[i], kc[i]) ^ rc;
			ka[i] = adler_combine(ka[i], ra, (size_t)widths[i] * 4);
		}
		for (int k = 0; k < n && found < 0; k++) {  // the sizes this many rows tall
			if (sz[k].ih != y + 1 || sz[k].iw > w) continue;
			int i = 0;
			while (widths[i] != sz[k].iw) i++;
			Entry probe = {.key = (uint64_t)kc[i] << 32 | ka[i], .iw = sz[k].iw, .ih = sz[k].ih};
			Entry *e = bsearch(&probe, ents, (size_t)nents, sizeof(Entry), cmp_entry);
			if (e) found = (int)(e - ents);
		}
	}
	if (check) {
		int slow = find_slow(px, w, h);
		if (slow != found) rt_log("texpack: CHECK %dx%d texture: one pass found %d, one by one %d", w, h, found, slow);
		else {
			static int ok;
			if (++ok % 10 == 0) rt_log("texpack: CHECK %d lookups agree (last: %dx%d, %s)", ok, w, h, found >= 0 ? "found" : "none");
		}
	}
	return found;
}

// ---- QOI ---------------------------------------------------------------------------------------------
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t *qoi_decode(const uint8_t *d, size_t n, int *pw, int *ph) {
	if (n < 22 || memcmp(d, "qoif", 4)) return NULL;
	uint32_t w = be32(d + 4), h = be32(d + 8);
	if (!w || !h || w > 8192 || h > 8192) return NULL;
	uint32_t *out = malloc((size_t)w * h * 4);
	if (!out) return NULL;
	uint8_t index[64][4], px[4] = {0, 0, 0, 255};
	memset(index, 0, sizeof index);
	size_t p = 14, end = n - 8;
	int run = 0;
	for (size_t i = 0; i < (size_t)w * h; i++) {
		if (run > 0) {
			run--;
		}
		else if (p < end) {
			uint8_t b = d[p++];
			if (b == 0xfe) {
				if (p + 3 > end) break;
				px[0] = d[p], px[1] = d[p + 1], px[2] = d[p + 2];
				p += 3;
			}
			else if (b == 0xff) {
				if (p + 4 > end) break;
				memcpy(px, d + p, 4);
				p += 4;
			}
			else {
				switch (b >> 6) {
				case 0: memcpy(px, index[b], 4); break;
				case 1:
					px[0] += ((b >> 4) & 3) - 2;
					px[1] += ((b >> 2) & 3) - 2;
					px[2] += (b & 3) - 2;
					break;
				case 2: {
					if (p + 1 > end) goto done;
					uint8_t b2 = d[p++];
					int vg = (b & 0x3f) - 32;
					px[0] += vg - 8 + ((b2 >> 4) & 15);
					px[1] += vg;
					px[2] += vg - 8 + (b2 & 15);
					break;
				}
				default: run = b & 0x3f; break;
				}
			}
			memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
		}
		memcpy(out + i, px, 4);
	}
done:
	*pw = (int)w, *ph = (int)h;
	return out;
}

// ---- loading -------------------------------------------------------------------------------------------
static uint8_t *read_all(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = sz > 0 ? malloc((size_t)sz) : NULL;
	if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) {
		free(d);
		d = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return d;
}

bool texpack_active(void) { return dir[0] && enabled; }

uint32_t *texpack_qoi(const uint8_t *d, size_t n, int *w, int *h) { return qoi_decode(d, n, w, h); }

uint8_t *texpack_file(const char *name, size_t *n, int *state) {
	if (!dir[0] || *state == 2) return NULL;
	char path[600];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	uint8_t *d = read_all(path, n);
	if (d) return d;
	struct stat est;
	if (stat(path, &est) == 0) {  // there but empty: none (the page's server answers so for an HD movie it hasn't)
		*state = 2;
		return NULL;
	}
	if (remote) {
		if (*state == 0) {
			char url[200];
			snprintf(url, sizeof url, "textures/%s", name);
			char sub[600];  // a file in a folder of the pack (movies/): the folder made here first
			snprintf(sub, sizeof sub, "%s", path);
			char *slash = strrchr(sub, '/');
			if (slash && slash > sub + strlen(dir)) *slash = 0, mkdir(sub, 0755);
			plat_fetch(url, path);
			*state = 1;
			return NULL;
		}
		char mark[620];
		snprintf(mark, sizeof mark, "%s.failed", path);
		struct stat st;
		if (stat(mark, &st) != 0) return NULL;  // still on its way
	}
	*state = 2;
	return NULL;
}

uint32_t *texpack_load(int idx, int *pw, int *ph, int *iw, int *ih, bool *failed) {
	*failed = false;
	if (idx < 0 || idx >= nents) {
		*failed = true;
		return NULL;
	}
	Entry *e = &ents[idx];
	*iw = e->iw, *ih = e->ih;
	if (e->state == 2) {
		*failed = true;
		return NULL;
	}
	char path[600];
	snprintf(path, sizeof path, "%s/%s", dir, e->file);
	// On the web the page's worker decodes the picture off the game's thread and writes its pixels
	// ("RAW1", width, height, 4 bytes unused, then RGBA rows: web/shell.html); natively, or from a
	// page without the worker, the file is the pack's QOI.
	uint32_t *px = NULL;
	FILE *f = fopen(path, "rb");
	if (f) {
		uint8_t head[16];
		if (fread(head, 1, 16, f) == 16 && !memcmp(head, "RAW1", 4)) {
			uint32_t w, h;
			memcpy(&w, head + 4, 4);
			memcpy(&h, head + 8, 4);
			px = w && h && w <= 8192 && h <= 8192 ? malloc((size_t)w * h * 4) : NULL;
			if (px && fread(px, 4, (size_t)w * h, f) != (size_t)w * h) free(px), px = NULL;
			*pw = (int)w, *ph = (int)h;
			fclose(f);
		} else {
			fclose(f);
			size_t n;
			uint8_t *d = read_all(path, &n);
			px = d ? qoi_decode(d, n, pw, ph) : NULL;
			free(d);
		}
		// A page keeps no copy: the picture is in the GPU now, and in the browser's cache should it
		// be needed again (asked for once more then).
		if (remote) unlink(path), e->state = 0;
	} else {
		if (remote) {
			struct stat st;
			char mark[620];
			snprintf(mark, sizeof mark, "%s.failed", path);
			if (e->state == 0) {
				char url[200];
				snprintf(url, sizeof url, "textures/%s", e->file);
				plat_fetch(url, path);
				e->state = 1;
				return NULL;
			}
			if (stat(mark, &st) != 0) return NULL;  // still on its way
		}
		rt_log("texpack: cannot read %s", path);
		e->state = 2;
		*failed = true;
		return NULL;
	}
	if (!px || *pw % e->iw || *ph % e->ih || *pw / e->iw != *ph / e->ih) {
		rt_log("texpack: %s is not a %dx%d picture at an integer scale", e->file, e->iw, e->ih);
		free(px);
		e->state = 2;
		*failed = true;
		return NULL;
	}
	return px;
}
