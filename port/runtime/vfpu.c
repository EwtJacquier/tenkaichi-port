// VFPU (Allegrex vector unit) executor for the ~40 instruction forms the game uses.
// Register storage is column-major per matrix: v[mtx*16 + col*4 + row]. Instruction register
// fields use the hardware numbering mtx*4 + col + row*32 (+ transpose bit for vectors/matrices).
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "rt.h"

enum { PFX_S = 0, PFX_T = 1, PFX_D = 2, CC = 3 };

// The VFPU's random number generator, as PPSSPP has worked it out from the PSP (MIPSVFPUUtils.cpp:
// vrnd_init, vrnd_generate): an LCG, a xorshift and a Pell-like sequence kept in RCX0-7. The same
// seed gives the same numbers on every copy (netplay).
// The VFPU's dot product as the hardware forms it (vmmul): each product of the two 24-bit
// mantissas kept to 25 bits, aligned to the largest product's exponent by dropping the low bits,
// summed as integers, the extra bits dropped again and the sum normalized, rounding to nearest
// even. A float sum rounds at every step and comes out a little apart: a game builds its legs'
// matrices with vmmul and the feet shook between two places frame after frame (the same exact
// product PPSSPP turns on for this game); measured on a match's bone matrices: 44 parts flipping
// pose 3+ frames in a row with float sums, none with this. Four terms, as the hardware always takes.
static float psp_dot4(const float *a, const float *b) {
	enum { EXTRA = 2 };
	int32_t ex[4], mant[4], neg[4], emax = 0, inf_neg = -1;
	uint32_t r;
	for (int i = 0; i < 4; i++) {
		uint32_t x, y;
		memcpy(&x, &a[i], 4);
		memcpy(&y, &b[i], 4);
		int32_t xe = (int32_t)((x >> 23) & 0xFF), ye = (int32_t)((y >> 23) & 0xFF);
		neg[i] = (int32_t)((x ^ y) >> 31);
		if (xe == 255 || ye == 255) {
			if ((xe == 255 && ((x & 0x7FFFFF) || ye == 0)) || (ye == 255 && ((y & 0x7FFFFF) || xe == 0))) goto nan;
			ex[i] = 255;
			mant[i] = 0x800000 << EXTRA;
		}
		else {
			uint64_t p = (uint64_t)(((x & 0x7FFFFFu) | 0x800000u) << EXTRA) * (uint64_t)(((y & 0x7FFFFFu) | 0x800000u) << EXTRA);
			mant[i] = (int32_t)((p >> (23 + EXTRA)) & 0x7FFFFFFF);
			ex[i] = xe + ye - 127;  // (zeros count as their tiny powers of two: they fall away when aligned)
		}
		if (ex[i] > emax) emax = ex[i];
		if (ex[i] >= 255) {  // infinity minus infinity
			if (inf_neg != -1 && neg[i] != inf_neg) goto nan;
			inf_neg = neg[i];
		}
	}
	int32_t sum = 0;
	for (int i = 0; i < 4; i++) {
		int32_t sh = emax - ex[i], m = sh >= 32 ? 0 : mant[i] >> sh;
		sum += neg[i] ? -m : m;
	}
	uint32_t sign = 0;
	if (sum < 0) sign = 0x80000000u, sum = -sum;
	sum >>= EXTRA;
	if (sum == 0 || emax <= 0) return 0.f;
	uint32_t m = (uint32_t)sum;
	int lead = 31 - __builtin_clz(m);
	if (lead > 23) {  // round to nearest even what is shifted away
		uint32_t half = 1u << (lead - 24);
		if ((m & half) && ((m & (half << 1)) || (m & (half - 1)))) m += half, lead = 31 - __builtin_clz(m);
		m >>= lead - 23;
		emax += lead - 23;
	}
	else {
		m <<= 23 - lead;
		emax -= 23 - lead;
	}
	if (emax >= 255) emax = 255, m = 0;
	else if (emax <= 0) return 0.f;
	r = sign | (uint32_t)emax << 23 | (m & 0x7FFFFFu);
	float f;
	memcpy(&f, &r, 4);
	return f;
nan:
	r = 0x7F800001u;
	memcpy(&f, &r, 4);
	return f;
}

// The same for the code tools/recomp.py writes for vmmul.
float rt_vdot4(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
	const float a[4] = {a0, a1, a2, a3}, b[4] = {b0, b1, b2, b3};
	return psp_dot4(a, b);
}

void vrnd_init(uint32_t seed, uint32_t *rcx) {
	for (int i = 0; i < 8; ++i)
		rcx[i] = 0x3F800000u | ((seed >> ((i / 4) * 16)) & 0xFFFFu) | (((seed >> (4 * i)) & 0xF) << 16);
}

uint32_t vrnd_generate(uint32_t *rcx) {
	uint32_t A = (rcx[0] & 0xFFFFu) | (rcx[4] << 16), B = (rcx[1] & 0xFFFFu) | (rcx[5] << 16);
	uint32_t C = (rcx[2] & 0xFFFFu) | (rcx[6] << 16), D = (rcx[3] & 0xFFFFu) | (rcx[7] << 16);
	uint32_t E = 0;
	for (int i = 0; i < 8; i++) E |= ((rcx[i] >> 16) & 0xF) << (4 * i);
	A = 69069u * A + 1u;
	B ^= B << 13;
	B ^= B >> 17;
	B ^= B << 5;
	uint32_t t = 2u * D + C + E;
	E = (uint32_t)(((uint64_t)C + (uint64_t)(D >> 1) + (uint64_t)E) >> 32);
	C = D;
	D = t;
	uint32_t parts[8] = {A & 0xFFFFu, B & 0xFFFFu, C & 0xFFFFu, D & 0xFFFFu, A >> 16, B >> 16, C >> 16, D >> 16};
	for (int i = 0; i < 8; i++) rcx[i] = 0x3F800000u | (((E >> (4 * i)) & 0xF) << 16) | parts[i];
	return A + B + D;
}

static inline int vsize(uint32_t op) { return (int)(((op >> 7) & 1) | ((op >> 14) & 2)) + 1; }
static inline int store_index(int mtx, int col, int row) { return mtx * 16 + (col & 3) * 4 + (row & 3); }

// Storage indices of an n-element vector register.
static void vregs(int reg, int n, int out[4]) {
	int mtx = (reg >> 2) & 7, col = reg & 3, row, transpose = (reg >> 5) & 1;
	switch (n) {
	case 1: transpose = 0; row = (reg >> 5) & 3; break;
	case 2: row = (reg >> 5) & 2; break;
	case 3: row = (reg >> 6) & 1; break;
	default: row = (reg >> 5) & 2; break;
	}
	for (int i = 0; i < n; i++) {
		out[i] = transpose ? store_index(mtx, row + i, col) : store_index(mtx, col, row + i);
	}
}

static void read_vec(CPU *c, int reg, int n, float *v) {
	int r[4];
	vregs(reg, n, r);
	for (int i = 0; i < n; i++) v[i] = c->v[r[i]];
}

// Matrix as m[j*4 + i] (column j, row i), side n.
static void mat_index(int reg, int n, int idx[16]) {
	int row, transpose = (reg >> 5) & 1;
	switch (n) {
	case 1: transpose = 0; row = (reg >> 5) & 3; break;
	case 2: row = (reg >> 5) & 2; break;
	case 3: row = (reg >> 6) & 1; break;
	default: row = (reg >> 5) & 2; break;
	}
	int mtx = (reg >> 2) & 7, col = reg & 3;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			idx[j * 4 + i] = transpose ? store_index(mtx, row + i, col + j) : store_index(mtx, col + j, row + i);
		}
	}
}
static void read_mat(CPU *c, int reg, int n, float *m) {
	int idx[16];
	mat_index(reg, n, idx);
	memset(m, 0, 16 * sizeof(float));
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) m[j * 4 + i] = c->v[idx[j * 4 + i]];
}
static void write_mat(CPU *c, int reg, int n, const float *m) {
	int idx[16];
	mat_index(reg, n, idx);
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) c->v[idx[j * 4 + i]] = m[j * 4 + i];
}

static void apply_st(float *r, uint32_t pfx, int n) {
	if (pfx == 0xE4) return;
	static const float consts[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};
	float orig[4] = {0, 0, 0, 0};
	memcpy(orig, r, (size_t)n * sizeof(float));
	for (int i = 0; i < n; i++) {
		int sel = (pfx >> (i * 2)) & 3, ab = (pfx >> (8 + i)) & 1, cst = (pfx >> (12 + i)) & 1, neg = (pfx >> (16 + i)) & 1;
		r[i] = cst ? consts[sel + (ab << 2)] : (ab ? fabsf(orig[sel]) : orig[sel]);
		if (neg) r[i] = -r[i];
	}
}

// Writes with D-prefix saturation and write mask, then resets all prefixes.
static void write_vec(CPU *c, int reg, int n, float *v) {
	uint32_t d = c->vfpu_ctrl[PFX_D];
	int r[4];
	vregs(reg, n, r);
	for (int i = 0; i < n; i++) {
		int sat = (d >> (i * 2)) & 3, mask = (d >> (8 + i)) & 1;
		float x = v[i];
		if (sat == 1) x = x < 0 ? 0 : (x > 1 ? 1 : x);
		else if (sat == 3) x = x < -1 ? -1 : (x > 1 ? 1 : x);
		if (!mask) c->v[r[i]] = x;
	}
}

static void eat_prefixes(CPU *c) {
	c->vfpu_ctrl[PFX_S] = 0xE4;
	c->vfpu_ctrl[PFX_T] = 0xE4;
	c->vfpu_ctrl[PFX_D] = 0;
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// Floats to half floats as the PSP converts them (PPSSPP's float_to_half_fast3; the other way: half_to_float).
static uint32_t float_to_half(float x) {
	uint32_t f;
	memcpy(&f, &x, 4);
	uint32_t sign = f & 0x80000000u, o;
	f ^= sign;
	if (f >= (255u << 23)) o = f > (255u << 23) ? 0x7E00 | (f & 0x3FF) : 0x7C00;
	else {
		f &= ~0xFFFu;
		float g = u2f(f) * u2f(15u << 23);
		memcpy(&f, &g, 4);
		f -= ~0xFFFu;
		if (f > (31u << 23)) f = 31u << 23;
		o = f >> 13;
	}
	return o | sign >> 16;
}
static float clamp01(float v) { return v <= 0.f ? 0.f : v > 1.f ? 1.f : v; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float half_to_float(uint16_t h) {
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
	if (e == 0) return m ? (s ? -1.f : 1.f) * ldexpf((float)m, -24) : u2f(s);
	if (e == 31) return u2f(s | 0x7F800000u | (m << 13));
	return u2f(s | ((e + 112) << 23) | (m << 13));
}

static _Noreturn void unknown(uint32_t pc, uint32_t op) { rt_fatal("VFPU instruction %08x at %08x not implemented", op, pc); }

// DBZ_VFPU_STATS=1 (diagnostics): at exit, the instruction forms run most (opcode, sub-op, size).
static uint64_t vstat[1 << 11];
static uint32_t vstat_op[1 << 11];
static void vstat_report(void) {
	int idx[1 << 11];
	for (int i = 0; i < (1 << 11); i++) idx[i] = i;
	for (int i = 0; i < 40; i++)
		for (int j = i + 1; j < (1 << 11); j++)
			if (vstat[idx[j]] > vstat[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
	uint64_t total = 0;
	for (int i = 0; i < (1 << 11); i++) total += vstat[i];
	fprintf(stderr, "VFPU: %llu instructions\n", (unsigned long long)total);
	for (int i = 0; i < 40 && vstat[idx[i]]; i++)
		fprintf(stderr, "  %5.1f%%  op=%08x (form %03x)\n", 100.0 * vstat[idx[i]] / total, vstat_op[idx[i]], idx[i]);
}

void rt_vfpu_mismatch(uint32_t pc, uint32_t op, const float *a, const float *b) {
	static int n;
	if (n++ > 50) return;
	for (int i = 0; i < 128; i++) {
		uint32_t x, y;
		memcpy(&x, &a[i], 4), memcpy(&y, &b[i], 4);
		if (x != y) rt_log("VFPU self-check: %08x at %08x: v[%d] inline %08x (%g), runtime %08x (%g)", op, pc, i, x, a[i], y, b[i]);
	}
}

void rt_vfpu(CPU *c, uint32_t pc, uint32_t op) {
	static int stats = -1;
	if (stats < 0 && (stats = getenv("DBZ_VFPU_STATS") != NULL)) atexit(vstat_report);
	if (stats) {
		uint32_t k = ((op >> 23) << 2 | ((op >> 7) & 1) | ((op >> 14) & 2)) & ((1 << 11) - 1);
		vstat[k]++;
		vstat_op[k] = op;
	}
	uint32_t o = op >> 26;
	int vd = op & 0x7F, vs = (op >> 8) & 0x7F, vt = (op >> 16) & 0x7F, n = vsize(op);
	float s[4] = {0}, t[4] = {0}, d[4] = {0};
	int rs = (op >> 21) & 31;

	switch (o) {
	case 0x32: case 0x3A: {  // lv.s / sv.s
		int reg = (int)(((op >> 16) & 0x1F) | ((op & 3) << 5)), r[4];
		vregs(reg, 1, r);
		uint32_t addr = c->r[rs] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
		if (o == 0x32) c->v[r[0]] = u2f(M32(addr));
		else M32(addr) = f2u(c->v[r[0]]);
		return;
	}
	case 0x36: case 0x3E: {  // lv.q / sv.q
		int reg = (int)(((op >> 16) & 0x1F) | ((op & 1) << 5)), r[4];
		vregs(reg, 4, r);
		uint32_t addr = c->r[rs] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
		for (int i = 0; i < 4; i++) {
			if (o == 0x36) c->v[r[i]] = u2f(M32(addr + 4u * i));
			else M32(addr + 4u * i) = f2u(c->v[r[i]]);
		}
		return;
	}
	case 0x12: {  // mfv/mfvc (rs=3), mtv/mtvc (rs=7)
		int rt = (op >> 16) & 31, imm = op & 0xFF;
		if (rs == 3) {
			uint32_t v;
			if (imm < 128) {
				int r[4];
				vregs(imm, 1, r);
				v = f2u(c->v[r[0]]);
			}
			else {
				v = c->vfpu_ctrl[(imm - 128) & 15];
			}
			if (rt) c->r[rt] = v;
		}
		else if (rs == 7) {
			if (imm < 128) {
				int r[4];
				vregs(imm, 1, r);
				c->v[r[0]] = u2f(c->r[rt]);
			}
			else {
				c->vfpu_ctrl[(imm - 128) & 15] = c->r[rt];
			}
		}
		else unknown(pc, op);
		return;
	}
	case 0x37: {  // prefixes, viim, vfim
		int sub = (op >> 23) & 7;
		if (sub < 2) c->vfpu_ctrl[PFX_S] = op & 0xFFFFF;
		else if (sub < 4) c->vfpu_ctrl[PFX_T] = op & 0xFFFFF;
		else if (sub < 6) c->vfpu_ctrl[PFX_D] = op & 0xFFF;
		else {
			int reg = (op >> 16) & 0x7F;
			d[0] = sub == 6 ? (float)(int16_t)(op & 0xFFFF) : half_to_float((uint16_t)op);
			write_vec(c, reg, 1, d);
			eat_prefixes(c);
		}
		return;
	}
	case 0x3F:  // vflush / vnop / vsync
		return;
	case 0x18: case 0x19: case 0x1B: {  // binary vector ops
		int sub = (op >> 23) & 7;
		read_vec(c, vs, n, s);
		read_vec(c, vt, n, t);
		apply_st(s, c->vfpu_ctrl[PFX_S], n);
		apply_st(t, c->vfpu_ctrl[PFX_T], n);
		int outn = n;
		if (o == 0x18) {
			for (int i = 0; i < n; i++) {
				switch (sub) {
				case 0: d[i] = s[i] + t[i]; break;
				case 1: d[i] = s[i] - t[i]; break;
				case 7: d[i] = s[i] / t[i]; break;
				default: unknown(pc, op);
				}
			}
		}
		else if (o == 0x19) {
			switch (sub) {
			case 0: for (int i = 0; i < n; i++) d[i] = s[i] * t[i]; break;  // vmul
			case 1: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i] * t[i]; outn = 1; break;  // vdot
			case 2: {  // vscl: t is a single register
				float tv[4];
				read_vec(c, vt, 1, tv);
				for (int i = 0; i < n; i++) d[i] = s[i] * tv[0];
				break;
			}
			case 4: d[0] = t[n - 1]; for (int i = 0; i < n - 1; i++) d[0] += s[i] * t[i]; outn = 1; break;  // vhdp
			case 6: d[0] = s[0] * t[1] - s[1] * t[0]; outn = 1; break;  // vdet
			default: unknown(pc, op);
			}
		}
		else {  // 0x1B
			if (sub == 0) {  // vcmp
				int cond = op & 0xF, bits = 0, any = 0, all = 1;
				for (int i = 0; i < n; i++) {
					bool r;
					switch (cond) {
					case 0: r = false; break;
					case 1: r = s[i] == t[i]; break;
					case 2: r = s[i] < t[i]; break;
					case 3: r = s[i] <= t[i]; break;
					case 4: r = true; break;
					case 5: r = s[i] != t[i]; break;
					case 6: r = s[i] >= t[i]; break;
					case 7: r = s[i] > t[i]; break;
					case 8: r = s[i] == 0; break;
					case 9: r = isnan(s[i]); break;
					case 10: r = isinf(s[i]); break;
					case 11: r = isnan(s[i]) || isinf(s[i]); break;
					case 12: r = s[i] != 0; break;
					case 13: r = !isnan(s[i]); break;
					case 14: r = !isinf(s[i]); break;
					default: r = !(isnan(s[i]) || isinf(s[i])); break;
					}
					bits |= (int)r << i;
					any |= r;
					all &= r;
				}
				c->vfpu_ctrl[CC] = (c->vfpu_ctrl[CC] & ~0x3Fu) | (uint32_t)bits | ((uint32_t)any << 4) | ((uint32_t)all << 5);
				eat_prefixes(c);
				return;
			}
			for (int i = 0; i < n; i++) {
				switch (sub) {
				case 2: d[i] = fminf(s[i], t[i]); break;
				case 3: d[i] = fmaxf(s[i], t[i]); break;
				case 6: d[i] = s[i] >= t[i] ? 1.f : 0.f; break;
				case 7: d[i] = s[i] < t[i] ? 1.f : 0.f; break;
				default: unknown(pc, op);
				}
			}
		}
		write_vec(c, vd, outn, d);
		eat_prefixes(c);
		return;
	}
	case 0x34: {
		int sub = (op >> 21) & 31, fn = (op >> 16) & 31;
		if (sub == 1 && fn <= 3) {  // vrnds / vrndi / vrndf1 / vrndf2: the VFPU's random numbers (state in RCX0-7)
			uint32_t *rcx = &c->vfpu_ctrl[8];
			if (fn == 0) {  // seed: the register's bits
				read_vec(c, vd, 1, d);
				vrnd_init(f2u(d[0]), rcx);
			} else {
				for (int i = n - 1; i >= 0; i--) {  // written in backwards order
					uint32_t r = vrnd_generate(rcx);
					uint32_t u = fn == 1 ? r : (fn == 2 ? 0x3F800000u : 0x40000000u) | (r & 0x007FFFFFu);
					memcpy(&d[i], &u, 4);
				}
				write_vec(c, vd, n, d);
			}
			eat_prefixes(c);
			return;
		}
		if (sub == 21) {  // vcmov: s where the condition holds (imm 0-5: one CC bit; 6: per element)
			int tf = (op >> 19) & 1, imm3 = fn & 7;
			read_vec(c, vs, n, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], n);
			read_vec(c, vd, n, d);
			apply_st(d, c->vfpu_ctrl[PFX_T], n);
			uint32_t cc = c->vfpu_ctrl[CC];
			for (int i = 0; i < n; i++)
				if (imm3 < 6 ? ((cc >> imm3) & 1) == (uint32_t)!tf : imm3 == 6 && ((cc >> i) & 1) == (uint32_t)!tf) d[i] = s[i];
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (sub == 1 && (fn == 18 || fn == 19 || fn >= 24)) {  // vf2h / vh2f, vuc2i / vc2i / vus2i / vs2i, vi2uc / vi2c / vi2us / vi2s
			uint32_t u[4] = {0}, w[4] = {0};
			read_vec(c, vs, n, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], n);
			for (int i = 0; i < 4; i++) u[i] = f2u(s[i]);
			int outn = 1;
			switch (fn) {
			case 18:  // vf2h: two floats a word
				w[0] = float_to_half(s[0]) | float_to_half(s[1]) << 16;
				if (n >= 3) w[1] = float_to_half(s[2]) | float_to_half(s[3]) << 16, outn = 2;
				break;
			case 19:  // vh2f
				for (int i = 0; i < (n == 1 ? 1 : 2); i++)
					w[2 * i] = f2u(half_to_float((uint16_t)(u[i] & 0xFFFF))), w[2 * i + 1] = f2u(half_to_float((uint16_t)(u[i] >> 16)));
				outn = n == 1 ? 2 : 4;
				break;
			case 24:  // vuc2i: each byte to the top of a word (x 0x01010101 >> 1)
				for (int i = 0; i < 4; i++) w[i] = (uint32_t)((u[0] >> (8 * i)) & 0xFF) * 0x01010101u >> 1;
				outn = 4;
				break;
			case 25:  // vc2i
				for (int i = 0; i < 4; i++) w[i] = ((u[0] >> (8 * i)) & 0xFF) << 24;
				outn = 4;
				break;
			case 26: case 27: {  // vus2i / vs2i: each halfword to the top of a word
				int m = n >= 2 ? 2 : 1;
				for (int i = 0; i < m; i++) {
					uint32_t v = u[i];
					w[2 * i] = fn == 26 ? (v & 0xFFFF) << 15 : (v & 0xFFFF) << 16;
					w[2 * i + 1] = fn == 26 ? (v & 0xFFFF0000u) >> 1 : v & 0xFFFF0000u;
				}
				outn = 2 * m;
				break;
			}
			case 28: case 29:  // vi2uc / vi2c: four words to their top bytes
				for (int i = 0; i < 4; i++) {
					int32_t v = (int32_t)u[i];
					uint32_t b = fn == 28 ? (uint32_t)((v < 0 ? 0 : v) >> 23) & 0xFF : (u[i] >> 24);
					w[0] |= b << (8 * i);
				}
				break;
			default: {  // 30 vi2us / 31 vi2s: pairs of words to halfwords
				int m = (n + 1) / 2;
				for (int i = 0; i < m; i++) {
					int32_t lo = (int32_t)u[2 * i], hi = (int32_t)u[2 * i + 1];
					if (fn == 30) {
						lo = (lo < 0 ? 0 : lo) >> 15, hi = (hi < 0 ? 0 : hi) >> 15;
						w[i] = ((uint32_t)lo & 0xFFFF) | ((uint32_t)hi & 0xFFFF) << 16;
					} else w[i] = (u[2 * i] >> 16) | (u[2 * i + 1] & 0xFFFF0000u);
				}
				outn = m;
			}
			}
			for (int i = 0; i < 4; i++) d[i] = u2f(w[i]);
			write_vec(c, vd, outn, d);
			eat_prefixes(c);
			return;
		}
		if (sub == 3) {  // vcst
			static const float k[20] = {0.f, 3.40282347e38f, 1.41421356f, 0.70710678f, 1.12837917f, 0.63661977f,
			                            0.31830989f, 0.78539816f, 1.57079633f, 3.14159265f, 2.71828183f, 1.44269504f,
			                            0.43429448f, 0.69314718f, 2.30258509f, 6.28318531f, 0.52359878f, 0.30103000f,
			                            3.32192809f, 0.86602540f};
			for (int i = 0; i < n; i++) d[i] = fn < 20 ? k[fn] : 0.f;
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (sub >= 16 && sub <= 20) {  // vf2in/iz/iu/id, vi2f (scale 2^imm)
			read_vec(c, vs, n, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], n);
			int imm = fn;
			uint32_t u[4];
			for (int i = 0; i < n; i++) {
				if (sub == 20) {
					uint32_t bits = f2u(s[i]);
					d[i] = ldexpf((float)(int32_t)bits, -imm);
					continue;
				}
				double x = ldexp((double)s[i], imm), r;
				switch (sub) {
				case 16: r = nearbyint(x); break;
				case 17: r = trunc(x); break;
				case 18: r = ceil(x); break;
				default: r = floor(x); break;
				}
				int32_t iv = isnan(x) ? 0x7FFFFFFF : r >= 2147483647.0 ? 0x7FFFFFFF : r <= -2147483648.0 ? INT32_MIN : (int32_t)r;
				u[i] = (uint32_t)iv;
				d[i] = u2f(u[i]);
			}
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (sub == 2 && fn >= 0x19 && fn <= 0x1B) {  // vt4444 / vt5551 / vt5650: four 8888 colors to 16 bits each
			read_vec(c, vs, 4, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], 4);
			uint16_t col[4];
			for (int i = 0; i < 4; i++) {
				uint32_t in = f2u(s[i]), a = in >> 24, b = (in >> 16) & 0xFF, g = (in >> 8) & 0xFF, r = in & 0xFF;
				col[i] = fn == 0x19 ? (uint16_t)((a >> 4) << 12 | (b >> 4) << 8 | (g >> 4) << 4 | r >> 4)
				       : fn == 0x1A ? (uint16_t)((a >> 7) << 15 | (b >> 3) << 10 | (g >> 3) << 5 | r >> 3)
				                    : (uint16_t)((b >> 3) << 11 | (g >> 2) << 5 | r >> 3);
			}
			d[0] = u2f((uint32_t)col[0] | (uint32_t)col[1] << 16);
			d[1] = u2f((uint32_t)col[2] | (uint32_t)col[3] << 16);
			write_vec(c, vd, n == 1 ? 1 : 2, d);
			eat_prefixes(c);
			return;
		}
		if (sub != 0 && sub != 2) unknown(pc, op);
		read_vec(c, vs, n, s);
		apply_st(s, c->vfpu_ctrl[PFX_S], n);
		int outn = n;
		if (sub == 0) {
			for (int i = 0; i < n; i++) {
				switch (fn) {
				case 0x00: d[i] = s[i]; break;
				case 0x01: d[i] = fabsf(s[i]); break;
				case 0x02: d[i] = -s[i]; break;
				case 0x03: d[i] = i == (vd & (n >= 3 ? 3 : 1)) ? 1.f : 0.f; break;  // vidt
				case 0x04: d[i] = clamp01(s[i]); break;  // vsat0
				case 0x05: d[i] = s[i] <= -1.f ? -1.f : s[i] > 1.f ? 1.f : s[i]; break;  // vsat1
				case 0x06: d[i] = 0.f; break;
				case 0x07: d[i] = 1.f; break;
				case 0x10: d[i] = 1.f / s[i]; break;
				case 0x11: d[i] = 1.f / sqrtf(s[i]); break;
				case 0x12: d[i] = sinf(s[i] * (float)M_PI_2); break;
				case 0x13: d[i] = cosf(s[i] * (float)M_PI_2); break;
				case 0x14: d[i] = exp2f(s[i]); break;
				case 0x15: d[i] = log2f(s[i]); break;
				case 0x16: d[i] = sqrtf(s[i]); break;
				case 0x17: d[i] = asinf(s[i]) / (float)M_PI_2; break;
				case 0x18: d[i] = -1.f / s[i]; break;
				default: unknown(pc, op);
				}
			}
		}
		else {  // sub == 2: VFPU9 group
			switch (fn) {
			case 0x04: for (int i = 0; i < n; i++) d[i] = 1.f - s[i]; break;  // vocp
			case 0x05: {  // vsocp: (1 - x, x) for each, saturated
				float a = s[0], b = s[1];
				d[0] = clamp01(1.f - a), d[1] = clamp01(a);
				if (n >= 2) d[2] = clamp01(1.f - b), d[3] = clamp01(b), outn = 4;
				else outn = 2;
				break;
			}
			case 0x06: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i]; outn = 1; break;  // vfad
			case 0x07: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i]; d[0] /= (float)n; outn = 1; break;  // vavg
			case 0x0A: for (int i = 0; i < n; i++) d[i] = s[i] > 0 ? 1.f : s[i] < 0 ? -1.f : 0.f; break;  // vsgn
			default: unknown(pc, op);
			}
		}
		write_vec(c, vd, outn, d);
		eat_prefixes(c);
		return;
	}
	case 0x3C: {  // matrix ops
		int idx = (op >> 21) & 31, group = idx >> 2;
		float ms[16], mt[16], md[16];
		if (idx == 29) {  // vrot: sine and cosine of a quarter-turn angle into chosen lanes
			int imm = (op >> 16) & 31, sl = (imm >> 2) & 3, cl = imm & 3;
			read_vec(c, vs, 1, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], 1);
			float sn = sinf(s[0] * (float)M_PI_2), cs = cosf(s[0] * (float)M_PI_2);
			if (imm & 0x10) sn = -sn;
			for (int i = 0; i < 4; i++) d[i] = sl == cl ? sn : 0.f;
			d[sl] = sn;
			d[cl] = cs;
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (idx == 28) {  // vmmov / vmidt / vmzero / vmone
			int fn = (op >> 16) & 15;
			if (fn == 0) read_mat(c, vs, n, md);
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++) {
					if (fn == 3) md[j * 4 + i] = i == j ? 1.f : 0.f;
					else if (fn == 6) md[j * 4 + i] = 0.f;
					else if (fn == 7) md[j * 4 + i] = 1.f;
					else if (fn != 0) unknown(pc, op);
				}
			write_mat(c, vd, n, md);
			eat_prefixes(c);
			return;
		}
		switch (group) {
		case 0:  // vmmul: each element the hardware's dot product of four (psp_dot4)
			memset(ms, 0, sizeof ms);
			memset(mt, 0, sizeof mt);
			read_mat(c, vs, n, ms);
			read_mat(c, vt, n, mt);
			for (int a = 0; a < n; a++)
				for (int b = 0; b < n; b++) md[a * 4 + b] = psp_dot4(&ms[b * 4], &mt[a * 4]);
			write_mat(c, vd, n, md);
			break;
		case 1: case 2: case 3: {  // vtfmN / vhtfmN
			int ins = group;  // rows - 1
			read_mat(c, vs, ins + 1, ms);
			float tv[4] = {0, 0, 0, 0};
			int tn = n < ins + 1 ? n : ins + 1;
			read_vec(c, vt, tn, tv);
			if (n <= ins) tv[ins] = 1.f;  // homogeneous form
			for (int i = 0; i <= ins; i++) {
				d[i] = 0;
				for (int k = 0; k <= ins; k++) d[i] += ms[i * 4 + k] * tv[k];
			}
			write_vec(c, vd, ins + 1, d);
			break;
		}
		case 4: {  // vmscl
			read_mat(c, vs, n, ms);
			float sc[4];
			read_vec(c, vt, 1, sc);
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++) md[j * 4 + i] = ms[j * 4 + i] * sc[0];
			write_mat(c, vd, n, md);
			break;
		}
		case 5:  // vcrsp.t / vqmul.q
			read_vec(c, vs, n, s);
			read_vec(c, vt, n, t);
			if (n == 3) {
				d[0] = s[1] * t[2] - s[2] * t[1];
				d[1] = s[2] * t[0] - s[0] * t[2];
				d[2] = s[0] * t[1] - s[1] * t[0];
			}
			else if (n == 4) {
				d[0] = s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
				d[1] = -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1];
				d[2] = s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
				d[3] = -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3];
			}
			else unknown(pc, op);
			write_vec(c, vd, n, d);
			break;
		default: unknown(pc, op);
		}
		eat_prefixes(c);
		return;
	}
	default: unknown(pc, op);
	}
}
