// PSP GE (graphics engine) command translation to OpenGL ES 3.0 / WebGL2.
//
// Vertex work (decode, morph, skinning, transform, lighting, fog, texgen) is done here in C;
// the GPU only rasterizes with one small shader. Every PSP framebuffer address the game draws
// into becomes an FBO, so render-to-texture and display both work by address.
#include <GLES3/gl3.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#include <unistd.h>
#endif
#include "ge.h"
#include "fonthd.h"
#include "text.h"
#include "texpack.h"
#include "platform.h"
#include "rt.h"

// What differs from game to game. This file is the same in every port (its switches' prefix
// aside, DBZ_ here); only these lines are the game's own.
#define GE_STENCIL_ALPHA 0       // the framebuffer's alpha made from its stencil before it is read (DBZ_STENCIL_ALPHA)
#define GE_FBO_CLUT_SKIP 1       // draws reading a framebuffer as a CLUT texture left out (DBZ_FBO_CLUT=1 draws them)
#define GE_BLEND_DOUBLE_ALPHA 1  // the double-alpha blend factors double the alpha (1) or the source color (0)
#define GE_TILES_PACK_KEY 0      // DBZ_TILES lines name the texture pack's key and picture size (1) or the catalog's (0)
#define GE_TEXCAT_WHERE 1        // DBZ_TEXCATALOG lines "key w h fmt addr where frame stride" (1) or "... scene frame" (0)

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
static uint32_t reg[256];
static float world[12], view[12], proj[16], tgen[12], bone[8 * 12];
static int world_i, view_i, proj_i, tgen_i, bone_i;
static uint8_t clut[4096];
static bool clut_dirty = true;
static uint64_t clut_h64, clut_h1k;

static inline float f24(uint32_t d) {
	uint32_t u = d << 8;
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static int gedebug;  // DBZ_GEDEBUG
uint32_t ge_offset_addr;  // relative-address offset (ORIGIN/OFFSETADDR), maintained by the list walker
static uint32_t vertex_addr, index_addr;
static inline uint32_t rel_addr(uint32_t data) {
	return (ge_offset_addr + (((reg[0x10] & 0x000F0000u) << 8) | (data & 0x00FFFFFFu))) & 0x0FFFFFFFu;
}

// ---------------------------------------------------------------------------------------------
// GL objects
// ---------------------------------------------------------------------------------------------
typedef struct {
	float x, y, z, w;
	float u, v;
	uint8_t c0[4];
	uint8_t c1[4];  // the secondary (specular) color (alpha unused)
	float fog;      // the fog depth as the GE has it, 1 or more: no fog. Interpolated as it is and
	                // clamped per pixel (PPSSPP's software rasterizer, the GE): clamping it per vertex
	                // fogged the middle of big polygons (the jungle's water and ground near the camera)
} GVert;  // 36 bytes: a match sends ~75,000 a frame to the GPU
typedef struct {
	GVert v;
	float box[4];  // the sprite's texture rectangle, half a texel in (u0 v0 u1 v1): sampling stays inside
} SVert;  // a sprite's corner

static GLuint prog, vbo, ibo, vao;
static GLuint fill_prog, fill_vao;  // a full-screen triangle of one alpha value (fbo_alpha_from_stencil)
static GLint fill_u_a;
static GLuint samplers[32];  // texture filter/wrap combinations (see RenderState.sampler)
static int rs = 1;
static int max_scale(void);
static int ui_filter = 0;  // filter for the 2D interface: 0 GE's own (the default), 1 xBR, 2 bicubic (DBZ_UI_FILTER)
// The game's own textures above 1x (not the pack's, not redrawn text), 2D and 3D: 0 as the GE
// samples them (the default), 1 xBR, 2 sharp pixels (DBZ_TEX_FILTER; the page's menu)
static int tex_filter = 0;           // render scale: framebuffers are rs times the PSP resolution
static GLint u_atMask;
static GLint u_tex, u_texEnable, u_tfunc, u_tfuncRGBA, u_colorDouble, u_envColor, u_alphaTest, u_atFunc, u_atRef,
	u_fogEnable, u_fogColor, u_clearMode, u_uvScale, u_blendDouble, u_ctFunc, u_ctRef, u_ctMask, u_xbr, u_lodBias;

typedef struct {
	uint32_t addr;  // VRAM offset
	int width, height;
	GLuint fbo, tex, depth;
	uint64_t last_draw;  // GE draw counter when last rendered into
	bool used;
	// Copy target: VRAM that a block transfer filled from a framebuffer (e.g. the static
	// background the game keeps while the shot gauge runs). The scaled picture lives here; the
	// hash of the 1x pixels written to VRAM tells whether the CPU has replaced them since.
	bool copy;
	uint64_t copy_hash, copy_checked;
	// The PSP's framebuffer alpha is its stencil; here the stencil is the GL stencil buffer, so
	// before the framebuffer is read as a texture its alpha is made from the stencil again
	// (fbo_alpha_from_stencil), when something drew into it since and it has a stencil at all.
	bool stencil_used;
	uint64_t alpha_synced;
} Fbo;
#define MAX_FBOS 32
static Fbo fbos[MAX_FBOS];
static Fbo *cur_fbo;
static uint64_t draw_counter;

typedef struct {
	uint64_t key, hash;
	GLuint tex;
	int w, h;
	uint64_t last_use;
	uint64_t checked;  // frame in which the data hash was last verified
	float scale;       // texels per GE texel: the render scale for redrawn text (text.c), a
	                   // replacement's own scale (texpack.c), 1 for the texture as the GE has it
	int pack1;         // a replacement still on its way (web): its index + 1, else 0
	uint32_t *ui_orig; // ... for a UI atlas with words: its pixels as the game has them (uitext_over)
	uint32_t *orig;    // ... else its pixels as the game has them (pack_upload: past a picture that
	                   // covers only a corner of the texture)
	bool mips;         // a replacement with its mipmaps (sampled smoothly when it shows smaller)
	uint64_t pack_frame, changed_frame;
	uint8_t changes;   // contents replaced in consecutive frames (movies): not looked up
	uint32_t *look;    // its pixels while the pack is still to be looked up for them (look_poll)
	uint64_t look_key; // ... their content (the memo's key) and whether text.h redraws them
	bool look_claimed;
	uint32_t *up_px;   // a big replacement going to the GPU a strip a frame (pack_poll), into up_tex
	GLuint up_tex;
	int up_W, up_H, up_row, up_s;
	uint64_t tile_key;  // DBZ_TILES with GE_TILES_PACK_KEY: the pack's key of its picture, the picture's size
	int tile_iw, tile_ih;
	uint64_t cat_key;  // DBZ_TEXCATALOG: its key in the catalog (0: not catalogued, contents that keep changing)
} TexEntry;
#define MAX_TEX 512
static TexEntry texcache[MAX_TEX];
static uint64_t frame_no;

// GE capture for debugging: every draw of one whole game frame (sceDisplaySetFrameBuf to
// sceDisplaySetFrameBuf), as one line per draw (hashes, NDC bounding box, smallest w) plus the
// vertices of small draws that cover a large area. Started by DBZ_GEDUMP_FRAME=N (first frame
// at or after vblank N) or by plat_debug_dump_requested (F9 in the browser). Written to
// DBZ_GEDUMP_FILE (default /tmp/gedump.txt in the browser, stderr natively).
static int ge_dump_state;  // 0 idle, 1 capturing
static int ge_dump_draws;
static int ge_dump_mtx_lines;
static bool ge_dump_done;
static FILE *ge_dump_file;
void rt_dump_threads(FILE *f);
static int ctx_dumped;

void ge_frame_mark(void) {
	static long at = -2;
	if (at == -2) at = getenv("DBZ_GEDUMP_FRAME") ? atol(getenv("DBZ_GEDUMP_FRAME")) : -1;
	if (ge_dump_state == 1) {
		if (!ge_dump_draws) return;  // the frame hasn't started drawing yet
		ge_dump_state = 0;
		ge_dump_done = true;
#ifdef __EMSCRIPTEN__
		fprintf(ge_dump_file, "ADDR stack base=%p end=%p current=%p sbrk=%p\n", (void *)emscripten_stack_get_base(),
		        (void *)emscripten_stack_get_end(), (void *)emscripten_stack_get_current(), sbrk(0));
#endif
		rt_dump_threads(ge_dump_file);
		fflush(ge_dump_file);
		rt_log("GE dump: end of frame (%d draws)", ge_dump_draws);
		return;
	}
	bool want = plat_debug_dump_requested();
	if (at >= 0 && (long)rt_vblank_count() >= at) want = true, at = -1;
	if (!want) return;
	const char *path = getenv("DBZ_GEDUMP_FILE");
#ifdef __EMSCRIPTEN__
	if (!path) path = "/tmp/gedump.txt";
#endif
	if (ge_dump_file && ge_dump_file != stderr) fclose(ge_dump_file);
	ge_dump_file = path ? fopen(path, "w") : stderr;
	if (!ge_dump_file) ge_dump_file = stderr;
	ge_dump_state = 1;
	ctx_dumped = 0;
	ge_dump_draws = 0;
	ge_dump_mtx_lines = 0;
	rt_log("GE dump: frame starts at vblank %ld", (long)rt_vblank_count());
}

// True once after a capture finished (the displayed frame can then be saved too).
bool ge_dump_finished(void) {
	bool d = ge_dump_done;
	ge_dump_done = false;
	return d;
}

static const char *VS =
	"#version 300 es\n"
	"layout(location=0) in vec4 a_pos; layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec4 a_c0; layout(location=3) in vec4 a_c1;\n"
	"layout(location=4) in float a_fog; layout(location=5) in vec4 a_box;\n"
	"out vec2 v_uv; out vec4 v_c0; out vec3 v_c1; out float v_fog; flat out vec4 v_box;\n"
	"void main() { gl_Position = a_pos; v_uv = a_uv; v_c0 = a_c0; v_c1 = a_c1.rgb; v_fog = a_fog; v_box = a_box; }\n";

static const char *FS =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_tex; uniform int u_texEnable, u_tfunc, u_tfuncRGBA, u_colorDouble, u_alphaTest, u_atFunc;\n"
	"uniform float u_lodBias;\n"
	"uniform int u_fogEnable, u_clearMode, u_atMask; uniform vec4 u_envColor; uniform float u_atRef; uniform vec3 u_fogColor;\n"
	"uniform vec2 u_uvScale; uniform int u_blendDouble, u_ctFunc, u_xbr; uniform ivec3 u_ctRef, u_ctMask;\n"
	"in vec2 v_uv; in vec4 v_c0; in vec3 v_c1; in float v_fog; flat in vec4 v_box; out vec4 o;\n"
	// xBR (Hyllian's xBR-lv2 rules, one pass, any scale) for the 2D interface: the PSP draws it at
	// 1:1 from small textures, so above 480x272 plain filtering only blurs it. u_xbr = 1 + clamp s
	// + 2 * clamp t (texel addressing of the draw); colors are compared premultiplied, so the
	// arbitrary RGB of transparent texels doesn't create edges.
	"ivec2 xsz; ivec2 xe;\n"
	"vec4 X(int dx, int dy) {\n"
	"  ivec2 p = xe + ivec2(dx, dy);\n"
	"  p.x = (u_xbr & 1) == 1 ? clamp(p.x, 0, xsz.x - 1) : (p.x % xsz.x + xsz.x) % xsz.x;\n"
	"  p.y = (u_xbr & 2) == 2 ? clamp(p.y, 0, xsz.y - 1) : (p.y % xsz.y + xsz.y) % xsz.y;\n"
	"  return texelFetch(u_tex, p, 0);\n"
	"}\n"
	"float Y(vec4 c) { return dot(c.rgb * c.a, vec3(14.352, 28.176, 5.472)) + 32.0 * c.a; }\n"
	"vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {\n"
	"  return abs(a - b) + abs(a - c) + abs(d - e) + abs(d - f) + 4.0 * abs(g - h);\n"
	"}\n"
	"bvec4 ne(vec4 a, vec4 b) { return greaterThanEqual(abs(a - b), vec4(15.0)); }\n"
	"bvec4 eq(vec4 a, vec4 b) { return lessThan(abs(a - b), vec4(15.0)); }\n"
	"bvec4 and4(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }\n"
	"bvec4 or4(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }\n"
	// Catmull-Rom bicubic on premultiplied colors (sharper than bilinear, no blocky look).
	"vec4 cubic(vec2 uv) {\n"
	"  xsz = textureSize(u_tex, 0);\n"
	"  vec2 tc = uv * vec2(xsz) - 0.5;\n"
	"  xe = ivec2(floor(tc)); vec2 f = tc - floor(tc);\n"
	"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
	"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
	"  float wx[4] = float[4](w0.x, w1.x, w2.x, w3.x), wy[4] = float[4](w0.y, w1.y, w2.y, w3.y);\n"
	"  vec4 acc = vec4(0.0);\n"
	"  for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) {\n"
	"    vec4 t = X(k - 1, j - 1); acc += vec4(t.rgb * t.a, t.a) * (wx[k] * wy[j]);\n"
	"  }\n"
	"  acc = clamp(acc, 0.0, 1.0);\n"
	"  return acc.a > 0.0 ? vec4(min(acc.rgb / acc.a, 1.0), acc.a) : vec4(0.0);\n"
	"}\n"
	// Sharp pixels: each texel a flat square, its edge blended over one screen pixel (the GPU's
	// bilinear does the blend: the sample moves to the texel's center except at its edge).
	"vec4 sharp(vec2 uv) {\n"
	"  vec2 sz = vec2(textureSize(u_tex, 0)), tc = uv * sz, k = 1.0 / max(fwidth(tc), vec2(1e-4));\n"
	"  vec2 cd = fract(tc) - 0.5, rr = max(0.5 - 0.5 / k, 0.0);\n"
	"  return texture(u_tex, (floor(tc) + (cd - clamp(cd, -rr, rr)) * k + 0.5) / sz);\n"
	"}\n"
	"vec4 xbr(vec2 uv) {\n"
	"  xsz = textureSize(u_tex, 0);\n"
	"  vec2 tc = uv * vec2(xsz);\n"
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
	"  vec2 px_out = fwidth(tc); float dl = clamp(max(px_out.x, px_out.y), 0.05, 1.0);\n"
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
	"void main() {\n"
	"  vec4 c = v_c0;\n"
	"  if (u_clearMode == 0) {\n"
	"    if (u_texEnable == 1) {\n"
	// a sprite samples within its own rectangle: above 1x, filtering at its edge would blend in the
	// next sprite of the atlas (lines along the edges)
	"      vec2 uv = clamp(v_uv, v_box.xy, v_box.zw);\n"
	// (a slight bias toward the full-size picture, -0.25: a tutorial page the game draws at 0.6x
	// stays sharp instead of mixing in the half-size picture, and the 3D yards keep their outlines
	// letter by letter; -1 lost them again, 0 blurred the page)
	// xBR and the sharp pixels only where the texture shows larger than itself: smaller (far away)
	// they would shimmer, and the plain sampling stands
	"      vec2 tfw = fwidth(uv * u_uvScale * vec2(textureSize(u_tex, 0))); bool mag = max(tfw.x, tfw.y) < 1.0;\n"
	"      vec4 t = u_xbr >= 12 && mag ? sharp(uv * u_uvScale) : u_xbr >= 12 ? texture(u_tex, uv * u_uvScale, u_lodBias)\n"
	"             : u_xbr >= 8 ? cubic(uv * u_uvScale) : u_xbr >= 4 && mag ? xbr(uv * u_uvScale) : texture(u_tex, uv * u_uvScale, u_lodBias);\n"
	"      vec3 rgb; float a = c.a;\n"
	"      if (u_tfuncRGBA == 2) t.a = 1.0;\n"  /* a framebuffer read as a format without alpha */
	"      if (u_tfunc == 0) { rgb = c.rgb * t.rgb; if (u_tfuncRGBA >= 1) a = c.a * t.a; }\n"
	"      else if (u_tfunc == 1) { rgb = u_tfuncRGBA >= 1 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
	"      else if (u_tfunc == 2) { rgb = mix(c.rgb, u_envColor.rgb, t.rgb); if (u_tfuncRGBA >= 1) a = c.a * t.a; }\n"
	"      else if (u_tfunc == 3) { rgb = t.rgb; if (u_tfuncRGBA >= 1) a = t.a; }\n"
	"      else { rgb = c.rgb + t.rgb; if (u_tfuncRGBA >= 1) a = c.a * t.a; }\n"
	"      if (u_colorDouble == 1) rgb *= 2.0;\n"
	"      c = vec4(rgb, a);\n"
	"    }\n"
	"    c.rgb += v_c1;\n"
	"    if (u_alphaTest == 1) {\n"
	"      float A = floor(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5);\n"
	// the PSP compares alpha & mask with ref & mask
	"      A = float(int(A) & u_atMask); float R = float(int(u_atRef) & u_atMask);\n"
	"      bool pass = u_atFunc == 0 ? false : u_atFunc == 1 ? true : u_atFunc == 2 ? A == R :\n"
	"                  u_atFunc == 3 ? A != R : u_atFunc == 4 ? A < R : u_atFunc == 5 ? A <= R :\n"
	"                  u_atFunc == 6 ? A > R : A >= R;\n"
	"      if (!pass) discard;\n"
	"    }\n"
	// Color test (0 never, 1 always, 2 equal, 3 not equal): games use it as a color key, e.g. the
	// ball shadow is a dark disc on an opaque black square whose black pixels must be dropped.
	"    if (u_ctFunc != 1) {\n"
	"      ivec3 C = ivec3(floor(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5));\n"
	"      bool eq = all(equal(C & u_ctMask, u_ctRef & u_ctMask));\n"
	"      if (u_ctFunc == 0 || (u_ctFunc == 2) != eq) discard;\n"
	"    }\n"
	"    if (u_fogEnable == 1) c.rgb = mix(u_fogColor, c.rgb, clamp(v_fog, 0.0, 1.0));\n"
	"  }\n"
	"  o = clamp(c, 0.0, 1.0);\n"
	"  if (u_blendDouble == 1) o.rgb = min(o.rgb * 2.0, vec3(1.0));\n"  // src * 2a as (2 src) * a
	"  else if (u_blendDouble == 2) o.a = min(o.a * 2.0, 1.0);\n"
	"}\n";

static GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		rt_fatal("shader compile: %s", log);
	}
	return s;
}

static const char *FILL_VS =
	"#version 300 es\n"
	"void main() { gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0); }\n";
static const char *FILL_FS =
	"#version 300 es\n"
	"precision highp float; uniform float u_a; out vec4 o;\n"
	"void main() { o = vec4(0.0, 0.0, 0.0, u_a); }\n";

void ge_init(void) {
	texpack_init();
	fill_prog = glCreateProgram();
	glAttachShader(fill_prog, compile(GL_VERTEX_SHADER, FILL_VS));
	glAttachShader(fill_prog, compile(GL_FRAGMENT_SHADER, FILL_FS));
	glLinkProgram(fill_prog);
	fill_u_a = glGetUniformLocation(fill_prog, "u_a");
	glGenVertexArrays(1, &fill_vao);
	prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
	glLinkProgram(prog);
	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		rt_fatal("shader link failed");
	}
#define U(n) n = glGetUniformLocation(prog, #n)
	U(u_tex); U(u_texEnable); U(u_tfunc); U(u_tfuncRGBA); U(u_colorDouble); U(u_envColor); U(u_alphaTest);
	U(u_atFunc); U(u_atRef); U(u_atMask); U(u_fogEnable); U(u_fogColor); U(u_clearMode); U(u_uvScale); U(u_blendDouble);
	U(u_ctFunc); U(u_ctRef); U(u_ctMask); U(u_xbr); U(u_lodBias);
#undef U
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);  // (part of the vertex array's state)
	for (int i = 0; i < 5; i++) glEnableVertexAttribArray(i);  // (pointers set by batch_flush)
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	// mipmap level bias (only textures with mipmaps, the pack's, are affected); DBZ_LOD_BIAS
	const char *bias = getenv("DBZ_LOD_BIAS");
	glUniform1f(u_lodBias, bias ? (float)atof(bias) : -0.25f);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	reg[0xC5] = 0xFF00;  // CLUT mask
	gedebug = getenv("DBZ_GEDEBUG") != NULL;
	// Texture sampling modes (filter and wrap per axis), as sampler objects.
	glGenSamplers(32, samplers);
	for (int i = 0; i < 32; i++) {
		glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, (i & 16) ? GL_LINEAR_MIPMAP_LINEAR : (i & 1) ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, (i & 2) ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, (i & 4) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, (i & 8) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	}
	// Render scale: framebuffers are rs times the PSP's 480x272 (sharper 3D, smoother edges once
	// scaled down to the window). DBZ_SCALE=1..6.
	const char *sc = getenv("DBZ_SCALE");
	rs = sc ? atoi(sc) : 4;
	if (rs < 1) rs = 1;
	if (rs > max_scale()) rs = max_scale();
	if (getenv("DBZ_TEX_FILTER")) ge_set_tex_filter(atoi(getenv("DBZ_TEX_FILTER")));
	const char *uf = getenv("DBZ_UI_FILTER");
	if (uf) ui_filter = atoi(uf) >= 0 && atoi(uf) <= 2 ? atoi(uf) : 0;
	static const char *const ui_names[] = {"off", "xBR", "bicubic"};
	rt_log("GE: rendering at %dx (%dx%d), interface filter %s", rs, 480 * rs, 272 * rs, ui_names[rs > 1 ? ui_filter : 0]);
}

// ---------------------------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------------------------
// The framebuffer at an address: the one drawn last there (a game may draw at one address with two
// widths in turn, e.g. a 64- and a 512-pixel-wide buffer each frame: both are kept).
static Fbo *fbo_find(uint32_t vaddr) {
	Fbo *best = NULL;
	for (int i = 0; i < MAX_FBOS; i++)
		if (fbos[i].used && fbos[i].addr == vaddr && (!best || fbos[i].last_draw > best->last_draw)) best = &fbos[i];
	return best;
}

static void batch_flush(void);
static void gl_invalidate(void);
static uint64_t hash_bytes(const uint8_t *p, size_t n);

static void fbo_free(Fbo *f) {
	glDeleteFramebuffers(1, &f->fbo);
	glDeleteTextures(1, &f->tex);
	glDeleteRenderbuffers(1, &f->depth);
	memset(f, 0, sizeof *f);
}

static uint64_t copy_hash(const Fbo *f) {
	uint32_t a = PSP_VRAM_BASE + f->addr, n = (uint32_t)(f->width * f->height * 4);
	return rt_valid(a, n) ? hash_bytes(MP(a), n) : 0;
}

// A 1x scratch color target of at least w x h, for moving pixels between PSP-resolution memory
// and the scaled framebuffers.
static GLuint tmp_fbo, tmp_tex;
static int tmp_w, tmp_h;
static void tmp_target(int w, int h) {
	if (w <= tmp_w && h <= tmp_h) return;
	tmp_w = w > tmp_w ? w : tmp_w;
	tmp_h = h > tmp_h ? h : tmp_h;
	if (!tmp_tex) glGenTextures(1, &tmp_tex), glGenFramebuffers(1, &tmp_fbo);
	glBindTexture(GL_TEXTURE_2D, tmp_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tmp_w, tmp_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, tmp_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex, 0);
}

// Copies PSP-resolution RGBA8 pixels (rows of row_len pixels) into a framebuffer rectangle.
static void fb_upload(Fbo *f, int x, int y, int w, int h, const void *px, int row_len) {
	batch_flush();
	glDisable(GL_SCISSOR_TEST);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);
	if (rs == 1) {
		glBindTexture(GL_TEXTURE_2D, f->tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	else {
		tmp_target(w, h);
		glBindTexture(GL_TEXTURE_2D, tmp_tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
		glBlitFramebuffer(0, 0, w, h, x * rs, y * rs, (x + w) * rs, (y + h) * rs, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl_invalidate();
}

// Reads a framebuffer rectangle back at PSP resolution.
static void fb_read(GLuint fbo, int x, int y, int w, int h, void *dst, int row_len) {
	batch_flush();
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	if (rs != 1) {
		tmp_target(w, h);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp_fbo);
		glBlitFramebuffer(x * rs, y * rs, (x + w) * rs, (y + h) * rs, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
		x = y = 0;
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, row_len);
	glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl_invalidate();
}

// GL objects of a framebuffer at the current render scale (depth cleared, contents undefined).
static void fbo_alloc(Fbo *f) {
	glGenTextures(1, &f->tex);
	glBindTexture(GL_TEXTURE_2D, f->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->width * rs, f->height * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenRenderbuffers(1, &f->depth);
	glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, f->width * rs, f->height * rs);
	glGenFramebuffers(1, &f->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		rt_fatal("FBO incomplete for %08x %dx%d", f->addr, f->width, f->height);
	}
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glClearDepthf(0);
	glClearStencil(0);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static int max_scale(void) {
	GLint max_rb = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
	int n = 6;
	while (n > 1 && 512 * n > max_rb) n--;
	return n;
}

// Changes the render scale while running: every framebuffer is recreated at the new size with
// its picture scaled over (the depth buffers start cleared; the next frame redraws them anyway).
void ge_set_render_scale(int n) {
	if (n < 1) n = 1;
	if (n > max_scale()) n = max_scale();
	if (n == rs) return;
	batch_flush();
	int old = rs;
	rs = n;
	for (int i = 0; i < MAX_FBOS; i++) {
		Fbo *f = &fbos[i];
		if (!f->used) continue;
		GLuint ofbo = f->fbo, otex = f->tex, odepth = f->depth;
		fbo_alloc(f);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ofbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
		glBlitFramebuffer(0, 0, f->width * old, f->height * old, 0, 0, f->width * rs, f->height * rs, GL_COLOR_BUFFER_BIT,
		                  GL_LINEAR);
		glDeleteFramebuffers(1, &ofbo);
		glDeleteTextures(1, &otex);
		glDeleteRenderbuffers(1, &odepth);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	// Every texture is decoded again: what was drawn for the old scale is drawn for the new one.
	for (int i = 0; i < MAX_TEX; i++) texcache[i].hash = 0, texcache[i].checked = 0;
	gl_invalidate();
	rt_log("GE: rendering at %dx (%dx%d)", rs, 480 * rs, 272 * rs);
}

int ge_tex_filter(void) { return tex_filter; }

void ge_set_tex_filter(int mode) {
	if (mode < 0 || mode > 2) mode = 0;
	if (mode != tex_filter) rt_log("GE: the game's own textures %s", mode == 1 ? "through xBR" : mode == 2 ? "as sharp pixels" : "as the GE samples them");
	tex_filter = mode;
}

void ge_set_ui_filter(int mode) {
	if (mode < 0 || mode > 2) mode = 2;
	ui_filter = mode;
}

static Fbo *fbo_get(uint32_t vaddr, int width, int height) {
	static Fbo *last;  // every draw asks for its target
	if (last && last->used && last->addr == vaddr && last->width == width && last->height >= height) return last;
	Fbo *f = NULL;
	int at_addr = 0;
	for (int i = 0; i < MAX_FBOS; i++) {
		if (!fbos[i].used || fbos[i].addr != vaddr) continue;
		at_addr++;
		if (fbos[i].width == width && fbos[i].height >= height) return last = &fbos[i];
		if (!f || fbos[i].last_draw < f->last_draw) f = &fbos[i];
	}
	if (at_addr < 2) f = NULL;  // a second size at this address gets a framebuffer of its own
	batch_flush();  // queued draws may target or sample the framebuffer being replaced
	if (!f) {
		for (int i = 0; i < MAX_FBOS && !f; i++) {
			if (!fbos[i].used) f = &fbos[i];
		}
		if (!f) {  // evict the least recently drawn
			f = &fbos[0];
			for (int i = 1; i < MAX_FBOS; i++) {
				if (fbos[i].last_draw < f->last_draw) f = &fbos[i];
			}
		}
	}
	if (f->used) {
		glDeleteFramebuffers(1, &f->fbo);
		glDeleteTextures(1, &f->tex);
		glDeleteRenderbuffers(1, &f->depth);
	}
	memset(f, 0, sizeof *f);
	f->used = true;
	f->addr = vaddr;
	f->width = width;
	f->height = height;
	fbo_alloc(f);
	// Start from VRAM so whatever the CPU put there is preserved.
	uint8_t *init = calloc((size_t)width * height, 4);
	for (int y = 0; y < height; y++) {
		uint32_t row = PSP_VRAM_BASE + vaddr + (uint32_t)(y * width * 4);
		if (rt_valid(row, (uint32_t)width * 4)) memcpy(init + (size_t)y * width * 4, MP(row), (size_t)width * 4);
	}
	fb_upload(f, 0, 0, width, height, init, width);
	free(init);
	gl_invalidate();
	TRACE("GE: new framebuffer %08x %dx%d", vaddr, width, height);
	return last = f;
}

static void bind_target(void) {
	uint32_t vaddr = (((reg[0x9D] & 0xFF0000u) << 8) | (reg[0x9C] & 0xFFFFFFu)) & 0x001FFFFFu;
	int stride = (int)(reg[0x9D] & 0x7FF);
	if (stride <= 0) stride = 512;
	int sy2 = (int)((reg[0xD5] >> 10) & 0x3FF);
	int height = sy2 >= 272 ? 512 : 272;
	cur_fbo = fbo_get(vaddr, stride, height);
	cur_fbo->last_draw = ++draw_counter;
	cur_fbo->copy = false;
}

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------
static inline uint32_t expand565(uint16_t v) {
	uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
	return 0xFF000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
}
static inline uint32_t expand5551(uint16_t v) {
	uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
	return ((v >> 15) ? 0xFF000000u : 0) | ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2);
}
static inline uint32_t expand4444(uint16_t v) {
	uint32_t r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
	return ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);
}
static uint32_t conv16(uint16_t v, int fmt) {
	return fmt == 0 ? expand565(v) : fmt == 1 ? expand5551(v) : expand4444(v);
}

static uint32_t clut_lookup(uint32_t index) {
	uint32_t cm = reg[0xC5];
	int fmt = cm & 3, shift = (cm >> 2) & 0x1F;
	uint32_t mask = (cm >> 8) & 0xFF, start = ((cm >> 16) & 0x1F) << 4;
	uint32_t i = ((index >> shift) & mask) | start;
	if (fmt == 3) {
		uint32_t v;
		memcpy(&v, clut + (i & 0xFF) * 4, 4);
		return v;
	}
	uint16_t v;
	memcpy(&v, clut + (i & 0x1FF) * 2, 2);
	return conv16(v, fmt);
}

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h = (h ^ w) * 1099511628211ull;
		h ^= h >> 29;
	}
	for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

// Returns a GL texture for texture level 0 and sets *w/*h.
static bool tex_from_fbo;  // the last texture_get returned a render target
static float tex_scale;    // ... or a texture this many times finer than the GE's (TexEntry.scale)
static bool tex_mips;      // ... with its mipmaps (TexEntry.mips)

// Where a slow frame's time went (DBZ_HITCH=ms logs every frame longer than that): the
// textures' first upload, the pack's pictures read, the words drawn over them (uitext_over), the
// atlases redrawn (text.h), what went to the GPU and the mipmaps made there.
static struct {
	uint64_t tex, read, over, vec, gpu, mip, poll, hash, dec, find;
	int n_new, n_pack;
} prof;
// Timed only with DBZ_HITCH: a clock read costs (in the browser a call out to JavaScript), and a
// match draws thousands of times a frame.
static int prof_on = -1;
static inline bool prof_enabled(void) {
	if (prof_on < 0) prof_on = getenv("DBZ_HITCH") && atoi(getenv("DBZ_HITCH")) > 0;
	return prof_on;
}
#define TIMED(field, expr) ({ __typeof__(expr) r_; \
	if (prof_enabled()) { uint64_t t_ = rt_now_us(); r_ = (expr); prof.field += rt_now_us() - t_; } \
	else { r_ = (expr); } \
	r_; })
#define TIMED_DO(field, stmt) do { if (prof_enabled()) { uint64_t t_ = rt_now_us(); stmt; prof.field += rt_now_us() - t_; } else { stmt; } } while (0)

void ge_hitch(uint64_t frame_us) {
	static int ms = -1;
	if (ms < 0) ms = getenv("DBZ_HITCH") ? atoi(getenv("DBZ_HITCH")) : 0;
	if (ms > 0 && frame_us >= (uint64_t)ms * 1000)
		rt_log("hitch: %.1f ms | textures %.1f (%d new: hashing %.1f, decoding %.1f, looking up %.1f), pack read %.1f, words over %.1f, "
		       "atlases %.1f, to the GPU %.1f, mipmaps %.1f (%d pictures, poll %.1f)",
		       frame_us / 1e3, prof.tex / 1e3, prof.n_new, prof.hash / 1e3, prof.dec / 1e3, prof.find / 1e3, prof.read / 1e3, prof.over / 1e3,
		       prof.vec / 1e3, prof.gpu / 1e3, prof.mip / 1e3, prof.n_pack, prof.poll / 1e3);
	memset(&prof, 0, sizeof prof);
}

// A replacement texture (pw x ph for the iw x ih texels of a w x h texture) into e->tex at its own
// scale; texels past the picture are the texture's own (orig, w x h, the game's: a picture that is
// a corner of its texture, the lab having left out what the game never drew there), else repeat
// its last column and row. False if the GPU can't hold it.
// A big one (PACK_STRIPS_FROM texels and up: a 2048x2048 picture is 16 MB) goes to the GPU over
// several frames instead (pack_poll): sent at once it held a frame up for 25-40 ms in the browser.
#define PACK_STRIPS_FROM (1024 * 1024)
#define PACK_STRIP_TEXELS (512 * 1024)
static void gl_invalidate(void);
static bool pack_upload(TexEntry *e, const uint32_t *px, int pw, int ph, int iw, int ih, int w, int h, const uint32_t *orig) {
	static GLint max_size;
	if (!max_size) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	int s = pw / iw, W = w * s, H = h * s;
	if (W > max_size || H > max_size) return false;
	uint32_t *full = NULL;
	if (W != pw || H != ph) {
		full = malloc((size_t)W * H * 4);
		for (int y = 0; y < H; y++) {
			const uint32_t *row = px + (size_t)(y < ph ? y : ph - 1) * pw;
			const uint32_t *own = orig ? orig + (size_t)(y / s) * w : NULL;
			uint32_t *d = full + (size_t)y * W;
			if (y < ph || !own) memcpy(d, row, (size_t)pw * 4);
			else for (int x = 0; x < pw; x++) d[x] = own[x / s];
			for (int x = pw; x < W; x++) d[x] = own ? own[x / s] : row[pw - 1];
		}
	}
	if ((int64_t)W * H >= PACK_STRIPS_FROM) {  // big: a strip a frame, the original shown meanwhile
		if (!full) full = malloc((size_t)W * H * 4), memcpy(full, px, (size_t)W * H * 4);
		glGenTextures(1, &e->up_tex);
		glBindTexture(GL_TEXTURE_2D, e->up_tex);
		// (storage that can be filled again: the texture becomes the cache slot's, and the slot
		// later takes other textures' pixels with glTexImage2D. Made with glTexStorage2D it was
		// immutable, those uploads failed, and the slot went on showing this picture in their place)
		TIMED_DO(gpu, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
		gl_invalidate();
		e->up_px = full, e->up_W = W, e->up_H = H, e->up_row = 0, e->up_s = s;
		return true;
	}
	if (!e->tex) glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	TIMED_DO(gpu, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, full ? full : px));
	// Shown smaller than its own size (3D billboards, far sprites), a 4x picture sampled without
	// mipmaps skips texels: thin outlines come and go letter by letter.
	TIMED_DO(mip, glGenerateMipmap(GL_TEXTURE_2D));
	prof.n_pack++;
	gl_invalidate();
	free(full);
	e->scale = (float)s;
	e->mips = true;
	return true;
}

// A replacement that was on its way: shown once it has arrived (checked once a frame). A big one
// takes a while to go to the GPU (a 2048x2048 picture is 16 MB, with its mipmaps), and a new place
// asks for many at once: past PACK_FRAME_US in a frame, the rest wait for the next frames (their
// originals show until then) instead of holding this one up.
#define PACK_FRAME_US 6000
static void pack_poll_(TexEntry *e);
static void pack_poll(TexEntry *e) { TIMED_DO(poll, pack_poll_(e)); }
static void pack_poll_(TexEntry *e) {
	static uint64_t spent_frame, spent_us;
	if (e->pack_frame == frame_no) return;
	if (spent_frame != frame_no) spent_frame = frame_no, spent_us = 0;
	if (spent_us >= PACK_FRAME_US) return;
	e->pack_frame = frame_no;
	uint64_t t0 = rt_now_us();
	if (e->up_px) {  // a big one on its way: its next strips, within this frame's time
		batch_flush();
		glBindTexture(GL_TEXTURE_2D, e->up_tex);
		int rows = PACK_STRIP_TEXELS / e->up_W > 0 ? PACK_STRIP_TEXELS / e->up_W : 1;
		while (e->up_row < e->up_H && spent_us + (rt_now_us() - t0) < PACK_FRAME_US) {
			int n = e->up_H - e->up_row < rows ? e->up_H - e->up_row : rows;
			TIMED_DO(gpu, glTexSubImage2D(GL_TEXTURE_2D, 0, 0, e->up_row, e->up_W, n, GL_RGBA, GL_UNSIGNED_BYTE,
			                              e->up_px + (size_t)e->up_row * e->up_W));
			e->up_row += n;
		}
		if (e->up_row >= e->up_H) {  // all there: the mipmaps, then it takes the original's place
			TIMED_DO(mip, glGenerateMipmap(GL_TEXTURE_2D));
			if (e->tex) glDeleteTextures(1, &e->tex);
			e->tex = e->up_tex, e->up_tex = 0;
			free(e->up_px);
			e->up_px = NULL;
			e->scale = (float)e->up_s;
			e->mips = true;
			e->pack1 = 0;
			prof.n_pack++;
		}
		gl_invalidate();
		spent_us += rt_now_us() - t0;
		return;
	}
	int pw, ph, iw, ih;
	bool failed;
	uint32_t *px = TIMED(read, texpack_load(e->pack1 - 1, &pw, &ph, &iw, &ih, &failed));
	if (!px) {
		if (failed) e->pack1 = 0, free(e->ui_orig), e->ui_orig = NULL, free(e->orig), e->orig = NULL;
		return;
	}
	uint64_t t1 = rt_now_us();
	batch_flush();  // queued draws still show the original
	// a UI atlas: the picture with text.h's words over it, drawn by the GPU (else the CPU's)
	if (e->ui_orig && !e->tex) glGenTextures(1, &e->tex);
	bool gl = e->ui_orig && TIMED(over, uitext_over_gl(e->ui_orig, e->w, e->h, rs, px, pw, ph, iw, ih, e->tex));
	const uint32_t *over = e->ui_orig && !gl ? TIMED(over, uitext_over(e->ui_orig, e->w, e->h, rs, px, pw, ph, iw, ih)) : NULL;
	if (gl || over) {
		glBindTexture(GL_TEXTURE_2D, e->tex);
		if (over) TIMED_DO(gpu, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, e->w * rs, e->h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, over));
		prof.n_pack++;
		gl_invalidate();
		e->scale = (float)rs;
		e->mips = false;
	} else {  // (text.h stopped drawing the atlas meanwhile: the picture as it is)
		pack_upload(e, px, pw, ph, iw, ih, e->w, e->h, e->ui_orig ? e->ui_orig : e->orig);
	}
	free(e->ui_orig);
	e->ui_orig = NULL;
	free(e->orig);
	e->orig = NULL;
	free(px);
	if (!e->up_px) e->pack1 = 0;  // (a big one keeps coming here for its strips)
	uint64_t t2 = rt_now_us();
	spent_us += t2 - t0;
	static int slow_log = -1;  // DBZ_TEXPACK_SLOW=ms: the pictures that took longer than that
	if (slow_log < 0) slow_log = getenv("DBZ_TEXPACK_SLOW") ? atoi(getenv("DBZ_TEXPACK_SLOW")) : 0;
	if (slow_log && t2 - t0 >= (uint64_t)slow_log * 1000)
		rt_log("texpack: %dx%d in %.1f ms (read %.1f, to the GPU %.1f)", pw, ph, (t2 - t0) / 1000.0, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);
}

// What the pack has for a texture's contents, once looked up: a texture that comes back (the same
// screen again, the next hole's things, a shot's effects) is not looked up again.
#define MEMO_N 8192
static struct {
	uint64_t key;  // 0: empty
	int pk;        // the pack's entry, -1 none
	bool claimed;  // a UI atlas text.h redraws
} memo[MEMO_N];
static bool memo_get(uint64_t k, int *pk, bool *claimed) {
	k |= 1;
	for (uint32_t i = (uint32_t)(k ^ k >> 29) % MEMO_N, n = 0; n < 32; i = (i + 1) % MEMO_N, n++) {
		if (!memo[i].key) return false;
		if (memo[i].key == k) return *pk = memo[i].pk, *claimed = memo[i].claimed, true;
	}
	return false;
}
static void memo_put(uint64_t k, int pk, bool claimed) {
	k |= 1;
	for (uint32_t i = (uint32_t)(k ^ k >> 29) % MEMO_N, n = 0; n < 32; i = (i + 1) % MEMO_N, n++)
		if (!memo[i].key || memo[i].key == k) {
			memo[i].key = k, memo[i].pk = pk, memo[i].claimed = claimed;
			return;
		}
	memo[(uint32_t)(k ^ k >> 29) % MEMO_N].key = k;  // full there: the newest wins
	memo[(uint32_t)(k ^ k >> 29) % MEMO_N].pk = pk;
	memo[(uint32_t)(k ^ k >> 29) % MEMO_N].claimed = claimed;
}

// Mods: a texture's pixels as decoded, before the pack is looked up for them (RGBA, w x h).
__attribute__((weak)) void rt_texture_decoded(uint32_t *px, int w, int h) { (void)px, (void)w, (void)h; }

// A new texture shows at once as the game has it (or its words redrawn) and its pack picture is
// looked up afterwards, a few milliseconds' worth a frame: a new screen brings tens of textures,
// and their lookups in the frame they came in held it up (a stutter at every change). The picture
// then comes as any other (pack_poll).
#define LOOK_FRAME_US 4000
static void look_poll(TexEntry *e) {
	static uint64_t frame, spent;
	if (frame != frame_no) frame = frame_no, spent = 0;
	if (spent >= LOOK_FRAME_US) return;
	uint64_t t0 = rt_now_us();
	int pk = TIMED(find, texpack_find(e->look, e->w, e->h));
	memo_put(e->look_key, pk, e->look_claimed);
	if (pk >= 0) {
		e->pack1 = pk + 1;
		if (e->look_claimed) e->ui_orig = e->look, e->look = NULL;  // pack_poll draws the words over the picture
		else free(e->orig), e->orig = e->look, e->look = NULL;      // (pack_upload)
	}
	free(e->look);
	e->look = NULL;
	spent += rt_now_us() - t0;
}

// The framebuffer's alpha made from its stencil (as the PSP has them: one and the same), before it
// is read as a texture: games mark pixels in the stencil and read them back as alpha (a glow that
// takes only the pixels whose alpha has a bit of the glowing objects' stencil value). The alpha is
// cleared, then each stencil bit adds its value where it is set: 9 full-screen passes, color
// untouched (a glow that takes only the pixels whose alpha has one bit of it). GE_STENCIL_ALPHA
// (DBZ_STENCIL_ALPHA): off where a game reads its framebuffer only for the colors (a bloom): there
// its fighters came out striped and see-through at high render scales.
static void fbo_alpha_from_stencil(Fbo *f) {
	static int on = -1;
	if (on < 0) on = getenv("DBZ_STENCIL_ALPHA") ? atoi(getenv("DBZ_STENCIL_ALPHA")) != 0 : GE_STENCIL_ALPHA;
	if (!on || !f->stencil_used || f->alpha_synced == f->last_draw) return;
	batch_flush();
	f->alpha_synced = f->last_draw;
	glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
	glViewport(0, 0, f->width * rs, f->height * rs);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glUseProgram(fill_prog);
	glBindVertexArray(fill_vao);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glUniform1f(fill_u_a, 0.f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
	for (int b = 0; b < 8; b++) {
		glStencilFunc(GL_EQUAL, 1 << b, 1u << b);
		glUniform1f(fill_u_a, (float)(1 << b) / 255.f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	gl_invalidate();
}

// DBZ_DUMP_FBO=vramaddr:dir (diagnostics): every 30 frames, the framebuffer at that VRAM offset as it
// is read as a texture: dir/fbo_<addr>_<frame>.ppm (color) and _a.pgm (alpha = the PSP's stencil).
static void fb_read(GLuint fbo, int x, int y, int w, int h, void *dst, int row_len);
static void fbo_debug_dump(Fbo *f) {
	static int want = -2;
	static char dir[256];
	if (want == -2) {
		const char *e = getenv("DBZ_DUMP_FBO");
		want = -1;
		if (e && sscanf(e, "%x:%255s", (unsigned *)&want, dir) != 2) want = -1;
	}
	if (want < 0 || f->addr != (uint32_t)want || frame_no % 30) return;
	static uint64_t last;
	if (last == frame_no) return;
	last = frame_no;
	batch_flush();
	int w = f->width, h = f->height;
	uint32_t *px = malloc((size_t)w * h * 4);
	fb_read(f->fbo, 0, 0, w, h, px, w);
	char path[400];
	snprintf(path, sizeof path, "%s/fbo_%06x_%06llu.ppm", dir, f->addr, (unsigned long long)frame_no);
	FILE *o = fopen(path, "wb");
	if (o) {
		fprintf(o, "P6 %d %d 255\n", w, h);
		for (int i = 0; i < w * h; i++) fputc(px[i] & 0xFF, o), fputc((px[i] >> 8) & 0xFF, o), fputc((px[i] >> 16) & 0xFF, o);
		fclose(o);
	}
	snprintf(path, sizeof path, "%s/fbo_%06x_%06llu_a.pgm", dir, f->addr, (unsigned long long)frame_no);
	if ((o = fopen(path, "wb"))) {
		fprintf(o, "P5 %d %d 255\n", w, h);
		for (int i = 0; i < w * h; i++) fputc(px[i] >> 24, o);
		fclose(o);
	}
	free(px);
	gl_invalidate();
}

// DBZ_TEXCATALOG=dir (native, for making HD textures): every texture the game shows, once each
// (its contents are in packs of encrypted, compressed files, so the catalog learns them from the
// running game): dir/<key>.rgba (u16 width, u16 height, then RGBA rows) and a line in
// dir/index.txt "key w h fmt addr scene frame". The key is the texture pack's
// (texpack_key over the whole texture), so a replacement made from it is found again. Textures
// whose contents keep changing (movies, things drawn each frame) are left out; a run adds to what
// earlier runs found. rt_scene (mods: what the game is showing) names where it was seen.
uint32_t rt_scene;
static bool tex_sweeping;  // DBZ_TEXSWEEP is decoding (texture_sweep)
static const char *texcat_dir = (const char *)1;
static void texcat_uv_read(void);
static void texcat_uv_write(void);
static void texcat_add(TexEntry *slot, const uint32_t *px, int w, int h, int stride, int fmt, uint32_t addr) {
	static uint64_t *seen;
	static size_t n_seen, cap_seen;
	if (texcat_dir == (const char *)1) {
		const char *dir = texcat_dir = getenv("DBZ_TEXCATALOG");
		if (dir) {  // what earlier runs found
			char path[512];
			snprintf(path, sizeof path, "%s/index.txt", dir);
			FILE *f = fopen(path, "r");
			char line[256];
			while (f && fgets(line, sizeof line, f)) {
				unsigned long long k;
				if (sscanf(line, "%llx", &k) != 1) continue;
				if (n_seen == cap_seen) cap_seen = cap_seen * 2 + 1024, seen = realloc(seen, cap_seen * sizeof *seen);
				seen[n_seen++] = k;
			}
			if (f) fclose(f);
			rt_log("texture catalog: %zu textures in %s so far", n_seen, dir);
			texcat_uv_read();
			atexit(texcat_uv_write);
		}
	}
	const char *dir = texcat_dir;
	slot->cat_key = 0;
	if (!dir || slot->changes >= 2 || w < 4 || h < 4 || rt_psmf_frame_at(addr)) return;
	uint64_t key = texpack_key(px, w, w, h);
	if (tex_sweeping) rt_log("texture sweep: %08x %dx%d -> %016llx", addr, w, h, (unsigned long long)key);
	for (size_t i = 0; i < n_seen && !tex_sweeping; i++)
		if (seen[i] == key) {
			slot->cat_key = key;
			return;
		}
	// Contents that keep changing at one place (water, lava, things drawn by the CPU): new ones
	// within a few frames of each other more than twice. The extractor drops the ones written
	// before that was clear (the "D" lines).
	enum { PLACES = 256 };
	static struct { uint32_t addr; uint64_t frame; int quick; } place[PLACES];
	int pi = (int)((addr >> 4) * 2654435761u >> 24);
	if (place[pi].addr != addr) place[pi].addr = addr, place[pi].frame = 0, place[pi].quick = 0;
	place[pi].quick = place[pi].frame && frame_no - place[pi].frame <= 8 ? place[pi].quick + 1 : 0;
	place[pi].frame = frame_no;
	if (place[pi].quick >= 2) {
		if (place[pi].quick == 2) {
			char path[512];
			snprintf(path, sizeof path, "%s/index.txt", dir);
			FILE *f = fopen(path, "a");
			if (f) fprintf(f, "D %08x %llu\n", addr, (unsigned long long)frame_no), fclose(f);
		}
		return;
	}
	if (n_seen == cap_seen) cap_seen = cap_seen * 2 + 1024, seen = realloc(seen, cap_seen * sizeof *seen);
	seen[n_seen++] = key;
	slot->cat_key = key;
	char path[512];
	snprintf(path, sizeof path, "%s/%016llx.rgba", dir, (unsigned long long)key);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint16_t wh[2] = {(uint16_t)w, (uint16_t)h};
	fwrite(wh, 2, 2, f);
	fwrite(px, 4, (size_t)w * h, f);
	fclose(f);
	snprintf(path, sizeof path, "%s/index.txt", dir);
	if ((f = fopen(path, "a"))) {
		// rt_scene, with 0x8000 when the texture is first drawn in 3D (not through mode) and 0x4000 when
		// on a skinned model (vertex weights: the fighters; stages and effects have none)
		uint32_t where = rt_scene | ((reg[0x12] >> 23) & 1 ? 0 : 0x8000u) | ((reg[0x12] >> 9) & 3 ? 0x4000u : 0);
		if (GE_TEXCAT_WHERE)
			fprintf(f, "%016llx %d %d %d %08x %u %llu %d\n", (unsigned long long)key, w, h, fmt, addr, where, (unsigned long long)frame_no,
			        stride);
		else
			fprintf(f, "%016llx %d %d %d %08x %u %llu\n", (unsigned long long)key, w, h, fmt, addr, rt_scene, (unsigned long long)frame_no);
		fclose(f);
	}
}

// What of each catalogued texture the game draws: how far right and down the texels its draws map
// reach (2D, and 3D mapped by UV), the most seen, in dir/uv.txt "key u v" (written every few
// seconds and at the end). A texture's rows may be narrower than it, texels past a row coming
// from the next one (the character select's portraits: 272 texels in a texture 512 wide), and
// under the picture comes whatever is next in memory: the extractor keeps the corner drawn.
enum { TEXCAT_UV = 1 << 14 };
static struct {
	uint64_t key;
	uint16_t u, v;
} texcat_uv[TEXCAT_UV];
static bool texcat_uv_dirty;

static void texcat_uv_write(void) {
	if (!texcat_uv_dirty || !texcat_dir || texcat_dir == (const char *)1) return;
	texcat_uv_dirty = false;
	char path[512], tmp[520];
	snprintf(path, sizeof path, "%s/uv.txt", texcat_dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	for (int i = 0; i < TEXCAT_UV; i++)
		if (texcat_uv[i].key) fprintf(f, "%016llx %d %d\n", (unsigned long long)texcat_uv[i].key, texcat_uv[i].u, texcat_uv[i].v);
	fclose(f);
	rename(tmp, path);
}

static void texcat_uv_put(uint64_t key, int u, int v) {
	uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 50);
	for (int n = 0; n < TEXCAT_UV && texcat_uv[i].key && texcat_uv[i].key != key; n++) i = (i + 1) & (TEXCAT_UV - 1);
	if (texcat_uv[i].key && texcat_uv[i].key != key) return;  // full
	if (texcat_uv[i].key == key && texcat_uv[i].u >= u && texcat_uv[i].v >= v) return;
	texcat_uv[i].key = key;
	if (u > texcat_uv[i].u) texcat_uv[i].u = (uint16_t)u;
	if (v > texcat_uv[i].v) texcat_uv[i].v = (uint16_t)v;
	texcat_uv_dirty = true;
}

static void texcat_uv_read(void) {  // what earlier runs in the folder found
	char path[512], line[128];
	snprintf(path, sizeof path, "%s/uv.txt", texcat_dir);
	FILE *f = fopen(path, "r");
	unsigned long long k;
	int u, v;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "%llx %d %d", &k, &u, &v) == 3 && k) texcat_uv_put(k, u, v);
	if (f) fclose(f);
	texcat_uv_dirty = false;
}

static void texcat_extent(uint64_t key, const GVert *tv, int count, int tw, int th) {
	float um = 0, vm = 0;
	for (int i = 0; i < count; i++) {
		if (tv[i].u > um) um = tv[i].u;
		if (tv[i].v > vm) vm = tv[i].v;
	}
	int u = (int)ceilf(um * (float)tw - 0.01f), v = (int)ceilf(vm * (float)th - 0.01f);
	texcat_uv_put(key, u > tw ? tw : u, v > th ? th : v);
	static uint64_t written;
	if (texcat_uv_dirty && frame_no - written >= 300) written = frame_no, texcat_uv_write();
}

// DBZ_TILES=file[:N] (with DBZ_TEXCATALOG; for pictures the game draws in pieces, READY! as a
// texture of 256 and one of 128 side by side): every 2D draw of a catalogued texture (in every Nth
// frame), "frame fb key iw ih x0 y0 x1 y1 u0 v0 u1 v1" (its catalog key and size, framebuffer
// pixels, texels), for the lab's mosaics.py.
static FILE *tiles_f;
static int tiles_on = -1, tiles_every = 1;
static bool tiles_enabled(void) {
	if (tiles_on < 0) {
		const char *e = getenv("DBZ_TILES");
		char path[512];
		snprintf(path, sizeof path, "%s", e ? e : "");
		char *colon = strrchr(path, ':');
		if (colon && atoi(colon + 1) > 0) tiles_every = atoi(colon + 1), *colon = 0;
		tiles_f = e ? fopen(path, "a") : NULL;
		if (tiles_f) setvbuf(tiles_f, NULL, _IOLBF, 0);
		tiles_on = tiles_f != NULL;
	}
	return tiles_on;
}
// key and its size (kw x kh): the catalog's key and the texture's size, or (GE_TILES_PACK_KEY) the
// pack's key and its picture's size
static void tiles_log(uint64_t key, int kw, int kh, const GVert *tv, int count, int type, float fbw, float fbh, int tw, int th,
                      float uvsx, float uvsy) {
	FILE *f = tiles_f;
	if (!tiles_enabled() || frame_no % (uint64_t)tiles_every || !(type == 6 || count <= 6)) return;
	f = tiles_f;
	int step = type == 6 ? 2 : count;
	for (int i = 0; i + step <= count; i += step) {
		float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
		for (int k = i; k < i + step; k++) {
			float x = (tv[k].x + 1) * 0.5f * fbw, y = (1 + tv[k].y) * 0.5f * fbh;  // through mode: w is 1
			float u = tv[k].u / uvsx * (float)tw, v = tv[k].v / uvsy * (float)th;
			x0 = fminf(x0, x), y0 = fminf(y0, y), x1 = fmaxf(x1, x), y1 = fmaxf(y1, y);
			u0 = fminf(u0, u), v0 = fminf(v0, v), u1 = fmaxf(u1, u), v1 = fmaxf(v1, v);
		}
		fprintf(f, "%llu %06x %016llx %d %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", (unsigned long long)frame_no, cur_fbo->addr,
		        (unsigned long long)key, kw, kh, x0, y0, x1, y1, u0, v0, u1, v1);
	}
}

static int last_slot = -1;  // the cache slot of the last texture texture_get gave
static GLuint texture_get_(int *tw, int *th, float *uvsx, float *uvsy);
static GLuint texture_get(int *tw, int *th, float *uvsx, float *uvsy) { return TIMED(tex, texture_get_(tw, th, uvsx, uvsy)); }
// Test aid for the texture catalog: DBZ_TEXSWEEP=addr:stride:count:clut_offset[:first]. Once the
// game draws the texture at addr, the count textures every stride bytes after it (each with its
// CLUT clut_offset bytes into it; from first strides before it if first < 0) are decoded as if
// drawn, the same way, and go to the catalog in a group of their own (scene TEXSWEEP_SCENE, also
// those found before). The rank badges sit in a row in
// RAM (64x32, 8-bit, stride 0xC60, the CLUT 0x800 in; Beginner first, at 0x08d71470 on the main
// menu) and the game shows only the player's and the opponent's.
#define TEXSWEEP_SCENE 100
static void texture_sweep(uint32_t addr) {
	static int state = -1;  // -1 not read, 0 off or done, 1 waiting
	static uint32_t at, stride, count, clut_off;
	static int first;
	if (state < 0) {
		state = 0;
		const char *e = getenv("DBZ_TEXSWEEP");
		if (e && sscanf(e, "%x:%x:%u:%x:%d", &at, &stride, &count, &clut_off, &first) >= 4 && stride && count) state = 1;
	}
	if (state != 1 || (addr & 0x0FFFFFFFu) != (at & 0x0FFFFFFFu)) return;
	state = 0;
	uint32_t a0 = reg[0xA0], b0 = reg[0xB0], b1 = reg[0xB1], scene = rt_scene;
	uint8_t saved[sizeof clut];
	memcpy(saved, clut, sizeof clut);
	int tw, th;
	float sx, sy;
	tex_sweeping = true;
	rt_scene = TEXSWEEP_SCENE;
	for (int k = first; k <= (int)count; k++) {
		uint32_t t = at + (uint32_t)k * stride, cl = t + clut_off;
		if (!rt_valid(t, stride) || ((t ^ at) & 0x0F000000u)) break;
		reg[0xA0] = (a0 & 0xFF000000u) | (t & 0xFFFFFFu);
		reg[0xB0] = (b0 & 0xFF000000u) | (cl & 0xFFFFFFu), reg[0xB1] = (b1 & ~0xF0000u) | ((cl >> 8) & 0xF0000u);
		memcpy(clut, MP(cl), 1024);
		clut_dirty = true;
		texture_get_(&tw, &th, &sx, &sy);
	}
	tex_sweeping = false;
	rt_scene = scene;
	reg[0xA0] = a0, reg[0xB0] = b0, reg[0xB1] = b1;
	memcpy(clut, saved, sizeof clut);
	clut_dirty = true;
	rt_log("texture sweep: %d textures from %08x", (int)count + 1 - first, at + (uint32_t)first * stride);
}

// A movie frame's HD picture (runtime/psmf.c): drawn instead of the frame the game wrote at addr,
// from a texture of the picture's own size (the coordinates scaled to it), sent again only when a
// new frame comes. The game writes its frames into a few buffers in turn and draws an earlier one:
// one picture (and texture) each.
#define MOVIE_BUFS 4
static struct {
	uint32_t addr;
	const uint32_t *px;
	int w, h, iw, ih;
	uint32_t seq, sent;
	GLuint tex;
	int tw, th;
} movie_hd[MOVIE_BUFS];

void ge_movie_hd(uint32_t addr, const uint32_t *px, int w, int h, int iw, int ih) {
	int k = 0;
	while (k < MOVIE_BUFS && movie_hd[k].addr != addr) k++;
	if (!px) {
		if (k < MOVIE_BUFS) movie_hd[k].px = NULL, movie_hd[k].addr = 0;
		return;
	}
	if (k == MOVIE_BUFS) {  // a new buffer: a free place, else the one written longest ago
		k = 0;
		for (int i = 0; i < MOVIE_BUFS; i++)
			if (!movie_hd[i].px || movie_hd[i].seq < movie_hd[k].seq) k = i;
	}
	static uint32_t seq;
	movie_hd[k].addr = addr, movie_hd[k].px = px, movie_hd[k].w = w, movie_hd[k].h = h, movie_hd[k].iw = iw, movie_hd[k].ih = ih;
	movie_hd[k].seq = ++seq;
}

static int movie_hd_at(uint32_t addr) {
	for (int i = 0; i < MOVIE_BUFS; i++)
		if (movie_hd[i].px && movie_hd[i].addr == addr) return i;
	return -1;
}

static GLuint movie_hd_texture(int k) {
	typeof(movie_hd[0]) *m = &movie_hd[k];
	if (!m->tex) glGenTextures(1, &m->tex);
	if (m->sent != m->seq) {
		batch_flush();  // queued draws may use the texture about to be filled
		glBindTexture(GL_TEXTURE_2D, m->tex);
		if (m->tw != m->w || m->th != m->h) {
			m->tw = m->w, m->th = m->h;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m->tw, m->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, m->px);
		}
		else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m->tw, m->th, GL_RGBA, GL_UNSIGNED_BYTE, m->px);
		gl_invalidate();
		m->sent = m->seq;
	}
	return m->tex;
}

static GLuint texture_get_(int *tw, int *th, float *uvsx, float *uvsy) {
	uint32_t addr = ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu);
	if (!tex_sweeping) texture_sweep(addr);
	int stride = (int)(reg[0xA8] & 0x7FF);
	int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
	int fmt = (int)(reg[0xC3] & 0xF);
	bool swz = reg[0xC2] & 1;
	*tw = w;
	*th = h;
	*uvsx = *uvsy = 1.f;
	tex_from_fbo = false;
	tex_scale = 1.f;
	tex_mips = false;
	if (w > 1024) w = 1024;
	if (h > 1024) h = 1024;
	// The buffer width (row pitch) may be narrower than the power-of-two texture size; texels
	// past it come from the following rows, exactly as the GE reads them.
	if (stride <= 0) stride = w;

	// Render-to-texture: the address is a framebuffer we drew into.
	if ((addr & 0x0F000000u) == PSP_VRAM_BASE || (addr & 0x0FE00000u) == 0x04000000u) {
		Fbo *f = fbo_find(addr & 0x001FFFFFu);
		if (f && f->copy && f->copy_checked != frame_no) {  // the CPU may have reused that VRAM
			f->copy_checked = frame_no;
			if (copy_hash(f) != f->copy_hash) {
				batch_flush();
				fbo_free(f);
				f = NULL;
			}
		}
		if (f && f->last_draw) {
			fbo_alpha_from_stencil(f);
			fbo_debug_dump(f);
			tex_from_fbo = true;
			*uvsx = (float)*tw / (float)f->width;
			*uvsy = (float)*th / (float)f->height;
			return f->tex;
		}
	}

	static int movie_log = -1;  // DBZ_MOVIE_LOG=1 (tests): the textures read from a movie's frames
	if (movie_log < 0) movie_log = getenv("DBZ_MOVIE_LOG") != NULL;
	if (movie_log && rt_psmf_frame_at(addr) && movie_log++ < 40)
		rt_log("movie texture %08x fmt %d %dx%d stride %d swz %d (HD %d)", addr, fmt, w, h, stride, swz, movie_hd_at(addr & 0x0FFFFFFFu));
	// a movie frame with an HD picture (fmt 8888, as psmf.c writes them)
	int mk = fmt == 3 ? movie_hd_at(addr & 0x0FFFFFFFu) : -1;
	if (mk >= 0) {
		GLuint t = movie_hd_texture(mk);
		*uvsx = (float)*tw / (float)movie_hd[mk].iw;  // the picture stands for iw x ih texels of the texture
		*uvsy = (float)*th / (float)movie_hd[mk].ih;
		tex_scale = (float)movie_hd[mk].w / (float)movie_hd[mk].iw;
		return t;
	}

	static const int bpp_tab[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
	int bpp = bpp_tab[fmt];
	if (!bpp || !rt_valid(addr, 1)) {
		return 0;
	}
	size_t row_bytes = (size_t)stride * bpp / 8;
	size_t bytes = fmt >= 8 ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == 8 ? 8 : 16) : row_bytes * h;
	if (!rt_valid(addr, (uint32_t)bytes)) {
		// Textures may extend past the end of VRAM/RAM (e.g. a 512x512 texture over a 480x272
		// movie frame near the top of VRAM); only the rows that exist are read.
		uint32_t a = addr & MEM_MASK;
		uint32_t limit = (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) ? PSP_VRAM_BASE + PSP_VRAM_SIZE : PSP_RAM_END;
		if (fmt >= 8 || a >= limit || row_bytes == 0) return 0;
		int rows = (int)((limit - a) / row_bytes);
		if (rows <= 0) return 0;
		bytes = row_bytes * (size_t)(rows < h ? rows : h);  // missing rows decode as zero
	}
	const uint8_t *src = MP(addr);
	uint64_t key = ((uint64_t)addr << 32) ^ ((uint64_t)fmt << 28) ^ ((uint64_t)w << 16) ^ ((uint64_t)h << 4) ^ (swz ? 1 : 0) ^
	               ((uint64_t)stride << 40);
	uint64_t clut_hash = 0;
	if (fmt >= 4 && fmt <= 7) {
		// hashed again only after a CLUT load, and only the part this format reads (a match loads
		// some 300 CLUTs a frame, mostly of 16 colors)
		static bool have64, have1k;
		if (clut_dirty) have64 = have1k = false, clut_dirty = false;
		if (fmt == 4 && !have64) clut_h64 = hash_bytes(clut, 64), have64 = true;
		if (fmt != 4 && !have1k) clut_h1k = hash_bytes(clut, 1024), have1k = true;
		clut_hash = (fmt == 4 ? clut_h64 : clut_h1k) * 31 + reg[0xC5];
		key ^= clut_hash * 0x9E3779B97F4A7C15ull;
	}
	// Consecutive draws usually use the same texture (last_slot).
	if (last_slot >= 0 && !tex_sweeping) {
		TexEntry *e = &texcache[last_slot];
		if (e->tex && e->key == key && e->checked == frame_no) {
			if (e->look) look_poll(e);
			if (e->pack1) pack_poll(e);
			e->last_use = frame_no;
			tex_scale = e->scale;
			tex_mips = e->mips;
			return e->tex;
		}
	}
	// Texture data is re-hashed at most once per frame. Where each key was last found is kept (by
	// a hash of the key) and looked at first: a match switches textures thousands of times a frame,
	// and the whole table is gone through only for a texture not there.
	enum { TEX_AT = 1024 };
	static int16_t tex_at[TEX_AT];
	uint32_t at_i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 54);
	TexEntry *slot = NULL, *lru = &texcache[0];
	if (texcache[tex_at[at_i]].tex && texcache[tex_at[at_i]].key == key) {
		slot = &texcache[tex_at[at_i]];
	}
	else {
		for (int i = 0; i < MAX_TEX; i++) {
			TexEntry *e = &texcache[i];
			if (e->tex && e->key == key) {
				slot = e;
				tex_at[at_i] = (int16_t)i;
				break;
			}
			if (e->last_use < lru->last_use) lru = e;
		}
	}
	if (slot && slot->checked == frame_no && !tex_sweeping) {  // (a sweep decodes again)
		if (slot->look) look_poll(slot);
		if (slot->pack1) pack_poll(slot);
		slot->last_use = frame_no;
		last_slot = (int)(slot - texcache);
		tex_scale = slot->scale;
		tex_mips = slot->mips;
		return slot->tex;
	}
	uint64_t hash = TIMED(hash, hash_bytes(src, bytes));
	if (slot && slot->hash == hash && !tex_sweeping) {
		if (slot->look) look_poll(slot);
		if (slot->pack1) pack_poll(slot);
		slot->checked = slot->last_use = frame_no;
		last_slot = (int)(slot - texcache);
		tex_scale = slot->scale;
		tex_mips = slot->mips;
		return slot->tex;
	}
	// The same texture with new contents: how many frames in a row that happened (movie frames).
	if (slot) slot->changes = frame_no - slot->changed_frame <= 2 ? (slot->changes < 255 ? slot->changes + 1 : 255) : 0;
	else slot = lru, slot->changes = 0, tex_at[at_i] = (int16_t)(slot - texcache);
	slot->changed_frame = frame_no;
	slot->pack1 = 0;
	slot->cat_key = 0;
	if (slot->up_px || slot->up_tex) {  // a replacement half way to the GPU: dropped with the old contents
		free(slot->up_px);
		slot->up_px = NULL;
		if (slot->up_tex) glDeleteTextures(1, &slot->up_tex);
		slot->up_tex = 0;
	}
	free(slot->ui_orig);
	slot->ui_orig = NULL;
	free(slot->orig);
	slot->orig = NULL;
	free(slot->look);
	slot->look = NULL;
	last_slot = (int)(slot - texcache);
	batch_flush();  // queued draws may use the GL texture about to be (re)filled

	// A string from the game's font engine: drawn again at the render scale (text.h, if the build
	// has it and it is on; else the HD font of the texture pack, if there is one).
	const uint32_t *hi = fmt == 3 && !swz && rs > 1 ? text_hires(addr, src, bytes, w, h, stride, rs) : NULL;
	if (!hi && fmt == 3 && !swz && rs > 1) hi = fonthd_hires(addr, src, bytes, w, h, stride, rs);
	static int font_log = -1;
	if (font_log < 0) font_log = getenv("DBZ_FONTHD_LOG") != NULL;
	if (font_log && fmt == 3) rt_log("fonthd: %s RGBA texture at %08x %dx%d (stride %d)%s", hi ? "drew" : "left", addr, w, h, stride, swz ? " swizzled" : "");
	if (hi) {
		if (!slot->tex) glGenTextures(1, &slot->tex);
		glBindTexture(GL_TEXTURE_2D, slot->tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, hi);
		gl_invalidate();
		slot->key = key;
		slot->hash = hash;
		slot->checked = slot->last_use = frame_no;
		slot->w = w;
		slot->h = h;
		slot->scale = tex_scale = (float)rs;
		slot->mips = tex_mips = false;
		return slot->tex;
	}

	// Linear copy (unswizzled if needed: blocks of 16 bytes x 8 rows), padded so reads past the
	// last row's pitch stay in bounds.
	size_t pad = (size_t)w * 4 + 16;
	uint64_t t_dec = rt_now_us();
	uint8_t *unswz = calloc(1, row_bytes * h + pad);
	const uint8_t *lin = unswz;
	if (fmt < 8) {
		if (swz) {
			size_t bw = row_bytes / 16;
			for (int y = 0; y < h; y++) {
				for (size_t xb = 0; xb < row_bytes; xb++) {
					size_t blk = (size_t)(y / 8) * bw + xb / 16;
					size_t so = blk * 128 + (size_t)(y % 8) * 16 + xb % 16;
					if (so < bytes) unswz[(size_t)y * row_bytes + xb] = src[so];
				}
			}
		}
		else {
			memcpy(unswz, src, bytes);
		}
	}

	uint32_t *out = malloc((size_t)w * h * 4);
	for (int y = 0; y < h && fmt < 8; y++) {
		const uint8_t *row = lin + (size_t)y * row_bytes;
		for (int x = 0; x < w; x++) {
			uint32_t px;
			switch (fmt) {
			case 0: case 1: case 2: { uint16_t v; memcpy(&v, row + x * 2, 2); px = conv16(v, fmt); break; }
			case 3: memcpy(&px, row + x * 4, 4); break;
			case 4: px = clut_lookup((row[x / 2] >> ((x & 1) * 4)) & 0xF); break;
			case 5: px = clut_lookup(row[x]); break;
			case 6: { uint16_t v; memcpy(&v, row + x * 2, 2); px = clut_lookup(v); break; }
			default: { uint32_t v; memcpy(&v, row + x * 4, 4); px = clut_lookup(v); break; }
			}
			out[y * w + x] = px;
		}
	}
	if (fmt >= 8) {  // PSP DXT blocks: indices first, then the two 565 colors (alpha after for DXT3/5)
		// (DXT colors keep red in the high bits, as DXT does elsewhere: the other way round from the
		// GE's 565 textures; read as those, a game's character select portraits came out blue)
		int bs = fmt == 8 ? 8 : 16;
		for (int by = 0; by < (h + 3) / 4; by++) {
			for (int bx = 0; bx < (w + 3) / 4; bx++) {
				const uint8_t *b = src + ((size_t)by * ((w + 3) / 4) + bx) * bs;
				const uint8_t *cb = b;
				uint32_t lines;
				uint16_t c1, c2;
				memcpy(&lines, cb, 4);
				memcpy(&c1, cb + 4, 2);
				memcpy(&c2, cb + 6, 2);
				uint32_t pal[4];
				pal[0] = expand565(c1);
				pal[1] = expand565(c2);
				for (int k = 0; k < 2; k++) pal[k] = (pal[k] & 0xFF00FF00u) | (pal[k] & 0xFFu) << 16 | ((pal[k] >> 16) & 0xFFu);
				for (int k = 0; k < 3; k++) {
					uint32_t a = (pal[0] >> (k * 8)) & 0xFF, bb = (pal[1] >> (k * 8)) & 0xFF;
					uint32_t m2, m3;
					if (c1 > c2 || fmt != 8) {
						m2 = (2 * a + bb) / 3;
						m3 = (a + 2 * bb) / 3;
					}
					else {
						m2 = (a + bb) / 2;
						m3 = 0;
					}
					pal[2] = (k ? pal[2] : 0xFF000000u) | (m2 << (k * 8));
					pal[3] = (k ? pal[3] : ((c1 > c2 || fmt != 8) ? 0xFF000000u : 0)) | (m3 << (k * 8));
				}
				for (int py = 0; py < 4; py++)
					for (int px2 = 0; px2 < 4; px2++) {
						int x = bx * 4 + px2, y = by * 4 + py;
						if (x >= w || y >= h) continue;
						uint32_t c = pal[(lines >> ((py * 4 + px2) * 2)) & 3];
						if (fmt == 9) {
							uint16_t al;
							memcpy(&al, b + 8 + py * 2, 2);
							c = (c & 0xFFFFFFu) | (((al >> (px2 * 4)) & 0xF) * 17u) << 24;
						}
						else if (fmt == 10) {
							uint8_t a0 = b[14], a1 = b[15];
							uint64_t bits = 0;
							memcpy(&bits, b + 8, 6);
							int idx = (int)((bits >> ((py * 4 + px2) * 3)) & 7);
							uint32_t a;
							if (idx == 0) a = a0;
							else if (idx == 1) a = a1;
							else if (a0 > a1) a = ((8 - idx) * a0 + (idx - 1) * a1) / 7;
							else a = idx == 6 ? 0 : idx == 7 ? 255 : ((6 - idx) * a0 + (idx - 1) * a1) / 5;
							c = (c & 0xFFFFFFu) | a << 24;
						}
						out[y * w + x] = c;
					}
			}
		}
	}
	free(unswz);
	prof.dec += rt_now_us() - t_dec;

	texcat_add(slot, out, w, h, stride, fmt, addr);
	if (GE_TILES_PACK_KEY && tiles_enabled()) {
		int i = texpack_find(out, w, h);
		slot->tile_key = i >= 0 ? texpack_entry_key(i, &slot->tile_iw, &slot->tile_ih) : 0;
	}
	rt_texture_decoded(out, w, h);  // mods: a texture changed as it comes (mods/title.c hides one)
	// A replacement from the texture pack (the HD textures) takes the place of everything below.
	static int pack_log = -1;
	if (pack_log < 0) pack_log = getenv("DBZ_TEXPACK_LOG") != NULL;
	if (pack_log && slot->changes < 4)
		rt_log("texpack: upload %016llx %dx%d fmt %d at %08x", (unsigned long long)texpack_key(out, w, w, h), w, h, fmt, addr);
	// A UI atlas with words that text.h draws again: with the pack's picture too, the picture with
	// those words over it (uitext_over), else the redrawn atlas below.
	// What the pack has for these contents: known (the memo), or looked up later (look_poll).
	uint64_t mkey = hash ^ clut_hash * 0x9E3779B97F4A7C15ull ^ ((uint64_t)fmt << 56) ^ ((uint64_t)w << 44) ^ ((uint64_t)h << 32) ^
	                ((uint64_t)stride << 16) ^ ((uint64_t)rs << 8) ^ (swz ? 1 : 0);
	bool claimed = false, look = false;
	int pk = -1;
	if (slot->changes < 4) {
		bool mcl;
		if (memo_get(mkey, &pk, &mcl)) claimed = mcl;
		else {
			claimed = rs > 1 && fmt >= 4 && fmt <= 7 && TIMED(find, uitext_claims(out, w, h));
			if (texpack_active()) look = true;
			else memo_put(mkey, -1, claimed);
		}
	}
	if (pk >= 0 && claimed) {
		int pw, ph, iw, ih;
		bool failed;
		uint32_t *px = TIMED(read, texpack_load(pk, &pw, &ph, &iw, &ih, &failed));
		if (px && !slot->tex) glGenTextures(1, &slot->tex);
		bool gl = px && TIMED(over, uitext_over_gl(out, w, h, rs, px, pw, ph, iw, ih, slot->tex));
		const uint32_t *over = px && !gl ? TIMED(over, uitext_over(out, w, h, rs, px, pw, ph, iw, ih)) : NULL;
		free(px);
		if (gl || over) {
			glBindTexture(GL_TEXTURE_2D, slot->tex);
			if (over) TIMED_DO(gpu, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, over));
			prof.n_pack++;
			gl_invalidate();
			free(out);
			slot->key = key, slot->hash = hash, slot->w = w, slot->h = h;
			slot->checked = slot->last_use = frame_no;
			slot->scale = tex_scale = (float)rs;
			slot->mips = tex_mips = false;
			return slot->tex;
		}
		if (!px && !failed) {  // on its way (web): the redrawn atlas until then
			slot->pack1 = pk + 1;
			slot->ui_orig = malloc(sizeof *out * (size_t)w * h);
			if (slot->ui_orig) memcpy(slot->ui_orig, out, sizeof *out * (size_t)w * h);
			else slot->pack1 = 0;
		}
		pk = -1;
	}
	if (pk >= 0) {
		int pw, ph, iw, ih;
		bool failed;
		uint32_t *px = TIMED(read, texpack_load(pk, &pw, &ph, &iw, &ih, &failed));
		slot->w = w, slot->h = h;
		bool shown = px && pack_upload(slot, px, pw, ph, iw, ih, w, h, out);
		if (shown && slot->up_px) shown = false, slot->pack1 = pk + 1;  // big: by strips, the original meanwhile
		free(px);
		if (shown) {
			if (pack_log) rt_log("texpack: replaced %016llx (%dx%d of %dx%d) at %dx", (unsigned long long)texpack_key(out, w, iw, ih), iw, ih, w, h, (int)slot->scale);
			free(out);
			slot->key = key;
			slot->hash = hash;
			slot->checked = slot->last_use = frame_no;
			tex_scale = slot->scale;
			tex_mips = slot->mips;
			return slot->tex;
		}
		if (!px && !failed) {  // on its way (web): the original until then
			slot->pack1 = pk + 1;
			free(slot->orig);
			slot->orig = malloc(sizeof *out * (size_t)w * h);
			if (slot->orig) memcpy(slot->orig, out, sizeof *out * (size_t)w * h);
		}
	}
	static const char *texdump = (const char *)1;
	if (texdump == (const char *)1) texdump = getenv("DBZ_TEXDUMP");
	if (texdump) {
		static int n;
		char path[512];
		snprintf(path, sizeof path, "%s/tex%04d_%08x_f%d_%dx%d_s%d%s.ppm", texdump, n++, addr, fmt, w, h, stride, swz ? "_swz" : "");
		FILE *df = fopen(path, "wb");
		if (df) {
			fprintf(df, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				uint8_t rgb[3] = {(uint8_t)out[i], (uint8_t)(out[i] >> 8), (uint8_t)(out[i] >> 16)};
				fwrite(rgb, 1, 3, df);
			}
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d_alpha.pgm", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fprintf(df, "P5\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) fputc((int)(out[i] >> 24), df);
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d.raw", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fwrite(src, 1, bytes, df);
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d.clut", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fwrite(clut, 1, sizeof clut, df);
			fclose(df);
		}
	}
	// A UI atlas with text baked in: drawn again at the render scale (text.h), if it is.
	// (atlases are paletted; the GPU draws it, or else the CPU)
	if (!slot->tex) glGenTextures(1, &slot->tex);
	bool ui_gl = rs > 1 && fmt >= 4 && fmt <= 7 && TIMED(vec, uitext_hires_gl(out, w, h, rs, slot->tex));
	const uint32_t *ui = !ui_gl && rs > 1 && fmt >= 4 && fmt <= 7 ? TIMED(vec, uitext_hires(out, w, h, rs)) : NULL;
	prof.n_new++;
	glBindTexture(GL_TEXTURE_2D, slot->tex);
	if (ui) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui);
	else if (!ui_gl) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
	gl_invalidate();
	if (look) slot->look = out, slot->look_key = mkey, slot->look_claimed = claimed;  // look_poll's
	else free(out);
	slot->key = key;
	slot->hash = hash;
	slot->checked = frame_no;
	slot->w = w;
	slot->h = h;
	slot->scale = tex_scale = ui != NULL || ui_gl ? (float)rs : 1.f;
	slot->mips = tex_mips = false;
	slot->last_use = frame_no;
	return slot->tex;
}

// ---------------------------------------------------------------------------------------------
// Vertex decoding and transform
// ---------------------------------------------------------------------------------------------
typedef struct {
	float pos[3], nrm[3], uv[2], w[8];
	uint8_t col[4];
	bool has_col;
} RawVert;

typedef struct {
	int tc, col, nrm, pos, wt, idx, nw, morphs;
	bool through;
	int off_w, off_tc, off_col, off_nrm, off_pos, size;
} VFmt;

static int align_to(int v, int a) { return (v + a - 1) & ~(a - 1); }

static VFmt vfmt_decode(uint32_t vt) {
	VFmt f = {0};
	f.tc = vt & 3;
	f.col = (vt >> 2) & 7;
	f.nrm = (vt >> 5) & 3;
	f.pos = (vt >> 7) & 3;
	f.wt = (vt >> 9) & 3;
	f.idx = (vt >> 11) & 3;
	f.nw = (int)((vt >> 14) & 7) + 1;
	f.morphs = (int)((vt >> 18) & 7) + 1;
	f.through = (vt >> 23) & 1;
	static const int csz[4] = {0, 1, 2, 4};
	int o = 0, maxa = 1;
	if (f.wt) {
		int s = csz[f.wt];
		o = align_to(o, s);
		f.off_w = o;
		o += s * f.nw;
		maxa = s > maxa ? s : maxa;
	}
	if (f.tc) {
		int s = csz[f.tc];
		o = align_to(o, s);
		f.off_tc = o;
		o += s * 2;
		maxa = s > maxa ? s : maxa;
	}
	if (f.col) {
		int s = f.col == 7 ? 4 : 2;
		o = align_to(o, s);
		f.off_col = o;
		o += s;
		maxa = s > maxa ? s : maxa;
	}
	if (f.nrm) {
		int s = csz[f.nrm];
		o = align_to(o, s);
		f.off_nrm = o;
		o += s * 3;
		maxa = s > maxa ? s : maxa;
	}
	{
		int s = csz[f.pos ? f.pos : 3];
		o = align_to(o, s);
		f.off_pos = o;
		o += s * 3;
		maxa = s > maxa ? s : maxa;
	}
	f.size = align_to(o, maxa);
	return f;
}

// n components of one kind (1: 8-bit, 2: 16-bit, 3: float) from p: the kind is looked at once per
// attribute, not once per component.
static inline void rd_n(const uint8_t *p, int kind, int n, bool is_signed, bool normalize, float *out) {
	switch (kind) {
	case 1:
		for (int i = 0; i < n; i++) {
			float v = is_signed ? (float)((const int8_t *)p)[i] : (float)p[i];
			out[i] = normalize ? v / 128.f : v;
		}
		break;
	case 2:
		for (int i = 0; i < n; i++) {
			int16_t s;
			uint16_t u;
			memcpy(&s, p + i * 2, 2);
			memcpy(&u, p + i * 2, 2);
			float v = is_signed ? (float)s : (float)u;
			out[i] = normalize ? v / 32768.f : v;
		}
		break;
	default: memcpy(out, p, sizeof(float) * (size_t)n); break;
	}
}

static void read_vertex(const VFmt *f, const uint8_t *p, RawVert *v) {
	bool norm = !f->through;
	if (f->wt) rd_n(p + f->off_w, f->wt, f->nw, false, true, v->w);
	if (f->tc) rd_n(p + f->off_tc, f->tc, 2, false, norm, v->uv);
	v->has_col = f->col != 0;
	if (f->col == 7) {
		memcpy(v->col, p + f->off_col, 4);
	}
	else if (f->col >= 4) {
		uint16_t c;
		memcpy(&c, p + f->off_col, 2);
		uint32_t rgba = conv16(c, f->col - 4);
		memcpy(v->col, &rgba, 4);
	}
	else if (f->col) memset(v->col, 0, 4);  // (a color format the GE doesn't have)
	if (f->nrm) rd_n(p + f->off_nrm, f->nrm, 3, true, norm, v->nrm);
	int pk = f->pos ? f->pos : 3;
	if (f->through && pk != 3) {  // through-mode z is unsigned
		rd_n(p + f->off_pos, pk, 2, true, false, v->pos);
		rd_n(p + f->off_pos + (pk == 1 ? 2 : 4), pk, 1, false, false, v->pos + 2);
	}
	else {
		rd_n(p + f->off_pos, pk, 3, true, norm && pk != 3, v->pos);
	}
}

static void mul43(const float *m, const float *v, float w, float *out) {
	out[0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + w * m[9];
	out[1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + w * m[10];
	out[2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + w * m[11];
}

static void unpack_rgba(uint32_t rgb, uint32_t a, float *o) {
	o[0] = (rgb & 0xFF) / 255.f;
	o[1] = ((rgb >> 8) & 0xFF) / 255.f;
	o[2] = ((rgb >> 16) & 0xFF) / 255.f;
	o[3] = (a & 0xFF) / 255.f;
}

static inline uint8_t to8(float v) {
	float t = v * 255.f + 0.5f;
	t = t < 0 ? 0 : t;
	t = t > 255.f ? 255.f : t;
	return (uint8_t)(int)t;
}

// Transforms one decoded vertex into a GL vertex.
// What every vertex of a draw shares (xf_setup): the whole chain object -> world -> view ->
// projection -> viewport as one matrix, the texture mapping, fog, the material color and the
// lights. It is worked out again only when something it reads changed (ge_command counts those
// changes in xf_gen, and those of view, projection and viewport in vp_gen): a match draws some
// 7,000 times a frame, mostly with the same lights and camera. The world position and normal are
// only made when lighting or the environment map uses them, and skinned normals only when
// something reads them.
static uint32_t xf_gen = 1, vp_gen = 1;

typedef struct {
	int kind, comp;      // directional, point, spot; ambient+diffuse, +specular, powered diffuse
	float L[3];          // directional: the unit direction to the light; else its position
	float ka, kb, kc;    // attenuation
	float dir[3], cutoff, spot_exp;
	float ac[4], dc[4], sc[4];  // ambient, diffuse, specular colors
	float H[3];          // directional: the unit half vector (to the light and the eye, +z)
	float dif[3], spe[3];  // directional: the diffuse and specular colors times the material's
} Light;

typedef struct {
	float m[16];       // object position -> output x, y, z, w (row-major, rows of 4)
	float fogz[4];     // object position -> view-space z (fog)
	float c0[4];       // the material color (vertices without a color)
	uint8_t c0b[4];    // the same as bytes
	float su, sv, ou, ov;
	float fog_end, fog_scale;
	int mapmode, tg_src;
	bool light, fog, world_needed, nrm_needed;
	int tw, th;
	float fbw, fbh;
	// lighting
	float mat_amb[4], mat_dif[4], mat_spe[4], emis[4], amb[4], spec_coef;
	uint32_t matupd;
	bool reverse, separate;
	int nlights;
	Light lt[4];
	bool dir_only;     // every light directional: light_vertex's short way
	int spec_int;      // the specular power when a whole number from 1 to 64 (multiplied out), else 0
	float base[3];     // directional lights and the material's ambient: emissive + all the ambient light
	float amb_sum[3];  // ... the ambient light, for the vertex's color as the material's ambient
	// what it was made from
	uint32_t gen, vgen;
	float kfbw, kfbh;
	double PV[16];     // viewport * projection * view
} Xf;
static Xf xf;

static void xf_setup(const VFmt *f, float fbw, float fbh, int tw, int th) {
	(void)f;
	Xf *x = &xf;
	x->tw = tw, x->th = th, x->fbw = fbw, x->fbh = fbh;
	if (x->gen == xf_gen && x->kfbw == fbw && x->kfbh == fbh) return;
	bool port_changed = x->vgen != vp_gen || x->kfbw != fbw || x->kfbh != fbh;
	x->gen = xf_gen, x->vgen = vp_gen, x->kfbw = fbw, x->kfbh = fbh;
	unpack_rgba(reg[0x55], reg[0x58], x->c0);
	for (int k = 0; k < 4; k++) x->c0b[k] = to8(x->c0[k]);
	// 4x4 row-major forms of the GE's matrices (world/view: 3 columns of 4 rows, translation last)
#define MUL4(out, a, b) for (int r_ = 0; r_ < 4; r_++) for (int c_ = 0; c_ < 4; c_++) \
		out[r_ * 4 + c_] = a[r_ * 4] * b[c_] + a[r_ * 4 + 1] * b[4 + c_] + a[r_ * 4 + 2] * b[8 + c_] + a[r_ * 4 + 3] * b[12 + c_]
	if (port_changed) {
		double V[16] = {0}, P[16], PV[16];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++) V[r * 4 + c] = view[c * 3 + r];
		V[15] = 1;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) P[r * 4 + c] = proj[c * 4 + r];
		float vsx = f24(reg[0x42]), vsy = f24(reg[0x43]), vsz = f24(reg[0x44]);
		float vcx = f24(reg[0x45]), vcy = f24(reg[0x46]), vcz = f24(reg[0x47]);
		float offx = (float)(reg[0x4C] & 0xFFFF) / 16.f, offy = (float)(reg[0x4D] & 0xFFFF) / 16.f;
		double port[16] = {2.0 * vsx / fbw, 0, 0, 2.0 * (vcx - offx) / fbw - 1.0,
		                   0, 2.0 * vsy / fbh, 0, 2.0 * (vcy - offy) / fbh - 1.0,
		                   0, 0, 2.0 * vsz / 65535.0, 2.0 * vcz / 65535.0 - 1.0,
		                   0, 0, 0, 1};
		MUL4(PV, P, V);
		MUL4(x->PV, port, PV);
	}
	double W[16] = {0}, A[16];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++) W[r * 4 + c] = world[c * 3 + r];
	W[15] = 1;
	MUL4(A, x->PV, W);
#undef MUL4
	for (int i = 0; i < 16; i++) x->m[i] = (float)A[i];
	for (int c = 0; c < 4; c++)  // view-space z: the view's third row times the world matrix
		x->fogz[c] = (float)((double)view[2] * W[c] + (double)view[5] * W[4 + c] + (double)view[8] * W[8 + c] +
		                     (double)view[11] * W[12 + c]);
	x->mapmode = reg[0xC0] & 3;
	x->tg_src = (reg[0xC0] >> 8) & 3;
	x->su = f24(reg[0x48]), x->sv = f24(reg[0x49]), x->ou = f24(reg[0x4A]), x->ov = f24(reg[0x4B]);
	x->light = reg[0x17] & 1;
	x->fog = reg[0x1F] & 1;
	x->fog_end = f24(reg[0xCD]), x->fog_scale = f24(reg[0xCE]);
	x->world_needed = x->light || x->mapmode == 2;
	x->nrm_needed = x->world_needed;
	if (!x->light) return;
	x->matupd = reg[0x53];
	unpack_rgba(reg[0x55], reg[0x58], x->mat_amb);
	unpack_rgba(reg[0x56], 0xFF, x->mat_dif);
	unpack_rgba(reg[0x57], 0xFF, x->mat_spe);
	unpack_rgba(reg[0x54], 0xFF, x->emis);
	unpack_rgba(reg[0x5C], reg[0x5D], x->amb);
	x->spec_coef = f24(reg[0x5B]);
	x->reverse = reg[0x51] & 1;
	x->separate = reg[0x5E] & 1;
	x->nlights = 0;
	for (int i = 0; i < 4; i++) {
		if (!(reg[0x18 + i] & 1)) continue;
		Light *l = &x->lt[x->nlights++];
		uint32_t lt = reg[0x5F + i];
		l->comp = lt & 3, l->kind = (lt >> 8) & 3;
		float lp[3] = {f24(reg[0x63 + i * 3]), f24(reg[0x64 + i * 3]), f24(reg[0x65 + i * 3])};
		memcpy(l->L, lp, sizeof l->L);
		if (l->kind == 0) {
			float ll = sqrtf(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
			if (ll > 0) l->L[0] /= ll, l->L[1] /= ll, l->L[2] /= ll;
		}
		l->ka = f24(reg[0x7B + i * 3]), l->kb = f24(reg[0x7C + i * 3]), l->kc = f24(reg[0x7D + i * 3]);
		if (l->kind == 2) {
			float dir[3] = {f24(reg[0x6F + i * 3]), f24(reg[0x70 + i * 3]), f24(reg[0x71 + i * 3])};
			float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (dl > 0) dir[0] /= dl, dir[1] /= dl, dir[2] /= dl;
			memcpy(l->dir, dir, sizeof dir);
			l->cutoff = f24(reg[0x8B + i]);
			l->spot_exp = f24(reg[0x87 + i]);
		}
		unpack_rgba(reg[0x8F + i * 3], 0, l->ac);
		unpack_rgba(reg[0x90 + i * 3], 0, l->dc);
		unpack_rgba(reg[0x91 + i * 3], 0, l->sc);
	}
	// The short way (light_vertex): directional lights only, the same for every vertex but the
	// normal: the ambient parts summed, the colors multiplied by the material's, the half vectors.
	x->dir_only = true;
	float sc = x->spec_coef;
	x->spec_int = sc >= 1 && sc <= 64 && sc == (float)(int)sc ? (int)sc : 0;
	for (int k = 0; k < 3; k++) x->amb_sum[k] = x->amb[k];
	for (int i = 0; i < x->nlights; i++) {
		Light *l = &x->lt[i];
		if (l->kind != 0) x->dir_only = false;
		float H[3] = {l->L[0], l->L[1], l->L[2] + 1.f};
		float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
		for (int k = 0; k < 3; k++) {
			l->H[k] = hl > 0 ? H[k] / hl : 0;
			l->dif[k] = l->dc[k] * x->mat_dif[k];
			l->spe[k] = l->sc[k] * x->mat_spe[k];
			x->amb_sum[k] += l->ac[k];
		}
	}
	for (int k = 0; k < 3; k++) x->base[k] = x->emis[k] + x->amb_sum[k] * x->mat_amb[k];
}

// s to the specular power (powf is slow in the browser; the game's powers are whole numbers)
static inline float spec_pow(const Xf *x, float s) {
	if (!x->spec_int) return powf(s, x->spec_coef);
	float r = 1.f, b = s;
	for (int e = x->spec_int; e; e >>= 1) {
		if (e & 1) r *= b;
		b *= b;
	}
	return r;
}

// Directional lights only (what the game uses): per light one dot product with the normal for the
// diffuse light and, for specular lights, one with the light's half vector.
static void light_vertex_dir(bool has_nrm, const float *wn, const float *vcol, float *c0, float *c1) {
	const Xf *x = &xf;
	float out[3], spec[3] = {0, 0, 0};
	float alpha = x->amb[3] * (x->matupd & 1 ? vcol[3] : x->mat_amb[3]);
	if (x->matupd & 1) {
		for (int k = 0; k < 3; k++) out[k] = x->emis[k] + x->amb_sum[k] * vcol[k];
	}
	else {
		memcpy(out, x->base, sizeof out);
	}
	if (has_nrm && x->nlights) {
		float n[3] = {wn[0], wn[1], wn[2]};
		float nl = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
		if (nl > 0) {
			float inv = (x->reverse ? -1.f : 1.f) / sqrtf(nl);
			n[0] *= inv, n[1] *= inv, n[2] *= inv;
			for (int i = 0; i < x->nlights; i++) {
				const Light *l = &x->lt[i];
				float d = n[0] * l->L[0] + n[1] * l->L[1] + n[2] * l->L[2];
				if (d <= 0) {
					if (l->comp != 2 || x->spec_coef != 0) continue;
					d = 0;  // (0 to the power 0: 1)
				}
				if (l->comp == 2) d = spec_pow(x, d);
				if (x->matupd & 2) {
					for (int k = 0; k < 3; k++) out[k] += d * l->dc[k] * vcol[k];
				}
				else {
					for (int k = 0; k < 3; k++) out[k] += d * l->dif[k];
				}
				if (l->comp == 1) {
					float s = n[0] * l->H[0] + n[1] * l->H[1] + n[2] * l->H[2];
					if (s <= 0) continue;
					s = spec_pow(x, s);
					if (x->matupd & 4) {
						for (int k = 0; k < 3; k++) spec[k] += s * l->sc[k] * vcol[k];
					}
					else {
						for (int k = 0; k < 3; k++) spec[k] += s * l->spe[k];
					}
				}
			}
		}
	}
	for (int k = 0; k < 3; k++) {
		c0[k] = out[k] + (x->separate ? 0 : spec[k]);
		c1[k] = x->separate ? spec[k] : 0;
	}
	c0[3] = alpha;
	c1[3] = 0;
}

static void light_vertex(const float *wpos, const float *wn, const float *vcol, float *c0, float *c1) {
	const Xf *x = &xf;
	// vcol may be c0 itself: c0 is written only at the end
	const float *mat_amb = x->matupd & 1 ? vcol : x->mat_amb;
	const float *mat_dif = x->matupd & 2 ? vcol : x->mat_dif;
	const float *mat_spe = x->matupd & 4 ? vcol : x->mat_spe;
	float out[4] = {x->emis[0] + x->amb[0] * mat_amb[0], x->emis[1] + x->amb[1] * mat_amb[1],
	                x->emis[2] + x->amb[2] * mat_amb[2], x->amb[3] * mat_amb[3]};
	float spec[3] = {0, 0, 0};
	float n[3] = {wn[0], wn[1], wn[2]};
	if (x->reverse) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
	float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (nl > 0) n[0] /= nl, n[1] /= nl, n[2] /= nl;
	for (int i = 0; i < x->nlights; i++) {
		const Light *l = &x->lt[i];
		float L[3], att = 1.f;
		if (l->kind == 0) {
			memcpy(L, l->L, sizeof L);
		}
		else {
			L[0] = l->L[0] - wpos[0], L[1] = l->L[1] - wpos[1], L[2] = l->L[2] - wpos[2];
			float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			float den = l->ka + l->kb * d + l->kc * d * d;
			att = den > 0 ? 1.f / den : 1.f;
			if (att > 1) att = 1;
			if (d > 0) L[0] /= d, L[1] /= d, L[2] /= d;
			if (l->kind == 2) {  // spot
				float sd = -(L[0] * l->dir[0] + L[1] * l->dir[1] + L[2] * l->dir[2]);
				att = sd >= l->cutoff ? att * powf(sd > 0 ? sd : 0, l->spot_exp) : 0;
			}
		}
		float d = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
		if (d < 0) d = 0;
		if (l->comp == 2) d = powf(d, x->spec_coef);
		for (int k = 0; k < 3; k++) out[k] += att * (l->ac[k] * mat_amb[k] + d * l->dc[k] * mat_dif[k]);
		if (l->comp == 1 && d > 0) {
			float H[3] = {L[0], L[1], L[2] + 1.f};
			float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
			float s = hl > 0 ? (n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) / hl : 0;
			s = s > 0 ? powf(s, x->spec_coef) : 0;
			for (int k = 0; k < 3; k++) spec[k] += att * s * l->sc[k] * mat_spe[k];
		}
	}
	for (int k = 0; k < 3; k++) {
		c0[k] = out[k] + (x->separate ? 0 : spec[k]);
		c1[k] = x->separate ? spec[k] : 0;
	}
	c0[3] = out[3];  // alpha: global ambient alpha * material ambient alpha
	c1[3] = 0;
}

static void transform(const VFmt *f, const RawVert *rv, GVert *o) {
	const Xf *x = &xf;
	if (f->through) {
		o->x = rv->pos[0] / x->fbw * 2.f - 1.f;
		o->y = rv->pos[1] / x->fbh * 2.f - 1.f;
		o->z = rv->pos[2] / 65535.f * 2.f - 1.f;
		o->w = 1.f;
		o->u = x->tw ? rv->uv[0] / (float)x->tw : 0;
		o->v = x->th ? rv->uv[1] / (float)x->th : 0;
		memcpy(o->c0, rv->has_col ? rv->col : x->c0b, 4);
		o->c1[0] = o->c1[1] = o->c1[2] = 0, o->c1[3] = 255;
		o->fog = 1.f;  // no fog
		return;
	}
	float p[3] = {rv->pos[0], rv->pos[1], rv->pos[2]}, n[3] = {rv->nrm[0], rv->nrm[1], rv->nrm[2]};
	if (f->wt) {  // skinning
		float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
		for (int b = 0; b < f->nw; b++) {
			float wb = rv->w[b], tp[3];
			if (wb == 0.f) continue;
			mul43(bone + b * 12, p, 1.f, tp);
			for (int k = 0; k < 3; k++) sp[k] += tp[k] * wb;
			if (x->nrm_needed) {
				float tn[3];
				mul43(bone + b * 12, n, 0.f, tn);
				for (int k = 0; k < 3; k++) sn[k] += tn[k] * wb;
			}
		}
		memcpy(p, sp, sizeof p);
		memcpy(n, sn, sizeof n);
	}
	const float *m = x->m;
	o->x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
	o->y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
	o->z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
	o->w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
	float wp[3] = {0, 0, 0}, wn[3] = {0, 0, 0};
	if (x->world_needed) {
		if (!x->light || !x->dir_only) mul43(world, p, 1.f, wp);  // (a light's distance)
		mul43(world, n, 0.f, wn);
	}
	// Texture coordinates
	if (x->mapmode == 0) {
		o->u = rv->uv[0] * x->su + x->ou;
		o->v = rv->uv[1] * x->sv + x->ov;
	}
	else if (x->mapmode == 1) {
		float in[3];
		if (x->tg_src == 0) memcpy(in, rv->pos, sizeof in);
		else if (x->tg_src == 1) in[0] = rv->uv[0], in[1] = rv->uv[1], in[2] = 0;
		else memcpy(in, rv->nrm, sizeof in);
		float t[3];
		mul43(tgen, in, 1.f, t);
		o->u = t[2] != 0 ? t[0] / t[2] : t[0];
		o->v = t[2] != 0 ? t[1] / t[2] : t[1];
	}
	else {  // environment map from the normal
		float nn = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
		o->u = nn > 0 ? (wn[0] / nn + 1.f) * 0.5f : 0.5f;
		o->v = nn > 0 ? (wn[1] / nn + 1.f) * 0.5f : 0.5f;
	}
	o->fog = 1.f;  // no fog
	if (x->fog) {
		float vz = x->fogz[0] * p[0] + x->fogz[1] * p[1] + x->fogz[2] * p[2] + x->fogz[3];
		float fd = (vz + x->fog_end) * x->fog_scale;
		o->fog = fd == fd ? fmaxf(-1e6f, fminf(fd, 1e6f)) : 1.f;  // not clamped to 0..1 here (see GVert)
	}
	if (!x->light) {  // the vertex's own color or the material's, as they are
		memcpy(o->c0, rv->has_col ? rv->col : x->c0b, 4);
		o->c1[0] = o->c1[1] = o->c1[2] = 0, o->c1[3] = 255;
		return;
	}
	float c0[4], c1[4];
	if (rv->has_col) {
		for (int k = 0; k < 4; k++) c0[k] = rv->col[k] / 255.f;
	}
	else {
		memcpy(c0, x->c0, sizeof c0);
	}
	if (x->dir_only) light_vertex_dir(f->nrm != 0, wn, c0, c0, c1);
	else light_vertex(wp, wn, c0, c0, c1);
	for (int k = 0; k < 4; k++) o->c0[k] = to8(c0[k]);
	if (x->separate) {
		for (int k = 0; k < 3; k++) o->c1[k] = to8(c1[k]);
	}
	else {
		o->c1[0] = o->c1[1] = o->c1[2] = 0;  // the specular light is in c0
	}
	o->c1[3] = 255;
}

// BOUNDINGBOX (as PPSSPP has it, DrawEngineCommon::TestBoundingBox): the vertices (skinned, then
// by the world matrix) are tested against the planes of the screen area the GE draws to (offset,
// scissor and region; near and far only with depth clamping); out of sight when all of them are
// outside one plane. Of more than 0x100 vertices the GE looks at one run of 0x100 only. a game
// tests ~440 boxes a frame and skips (BJUMP) what is out of sight; drawn anyway, things the game
// had stopped updating came out as polygons from nowhere.
bool ge_bounding_box(uint32_t count) {
	static int off = -1;  // DBZ_NO_BBOX=1: as before (no test, the addresses left alone), to compare
	if (off < 0) off = getenv("DBZ_NO_BBOX") != NULL;
	if (off) return true;
	VFmt f = vfmt_decode(reg[0x12]);
	int isz = f.idx == 1 ? 1 : f.idx == 2 ? 2 : 0;
	uint32_t vaddr = vertex_addr, iaddr = index_addr;
	if (isz) index_addr += count * (uint32_t)isz;  // the addresses move on, as after a drawing
	else vertex_addr += count * (uint32_t)f.size;
	if (!count) return false;
	if (f.through) return true;
	uint32_t first = 0, n = count;
	if (count > 0x200) first = count - 0x200, n = 0x100;
	else if (count > 0x100) n = count - 0x100;
	if (n > 1024 || !rt_valid(vaddr, 1) || (isz && !rt_valid(iaddr, count * (uint32_t)isz))) return true;
	// the planes: projection * view, then the screen area as -1..1
	double V[16] = {0}, P[16], PV[16];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++) V[r * 4 + c] = view[c * 3 + r];
	V[15] = 1;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) P[r * 4 + c] = proj[c * 4 + r];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) PV[r * 4 + c] = P[r * 4] * V[c] + P[r * 4 + 1] * V[4 + c] + P[r * 4 + 2] * V[8 + c] + P[r * 4 + 3] * V[12 + c];
	float offx = (float)(reg[0x4C] & 0xFFFF) / 16.f, offy = (float)(reg[0x4D] & 0xFFFF) / 16.f;
	int sx1 = (int)(reg[0xD4] & 0x3FF), sy1 = (int)((reg[0xD4] >> 10) & 0x3FF);
	int sx2 = (int)(reg[0xD5] & 0x3FF), sy2 = (int)((reg[0xD5] >> 10) & 0x3FF);
	int rx1 = (int)(reg[0x15] & 0x3FF) - 0x100, ry1 = (int)((reg[0x15] >> 10) & 0x3FF) - 0x100;
	int rx2 = (int)(reg[0x16] & 0x3FF), ry2 = (int)((reg[0x16] >> 10) & 0x3FF);
	float minx = offx + (float)(rx1 > sx1 ? rx1 : sx1) - 1.f, miny = offy + (float)(ry1 > sy1 ? ry1 : sy1) - 1.f;
	float maxx = offx + (float)(rx2 < sx2 ? rx2 : sx2) + 1.f, maxy = offy + (float)(ry2 < sy2 ? ry2 : sy2) + 1.f;
	float vsx = f24(reg[0x42]), vsy = f24(reg[0x43]), vcx = f24(reg[0x45]), vcy = f24(reg[0x46]);
	if (vsx == 0 || vsy == 0) return true;
	float minvx = (minx - vcx) / vsx, maxvx = (maxx - vcx) / vsx, minvy = (miny - vcy) / vsy, maxvy = (maxy - vcy) / vsy;
	if (maxvx == minvx || maxvy == minvy) return true;
	float ix = 1.f / (maxvx - minvx), iy = 1.f / (maxvy - minvy);
	int planes = (reg[0x1C] & 1) ? 6 : 4;  // near and far only with depth clamping
	bool edge[4] = {maxx >= 4096.f, minx < 1.f, miny < 1.f, maxy >= 4096.f};
	int inside[6] = {0};
	float mw[8];
	for (int m = 0; m < 8; m++) mw[m] = f24(reg[0x2C + m]);
	for (uint32_t k = 0; k < n; k++) {
		uint32_t i = first + k, vi = i;
		if (isz == 1) vi = M8(iaddr + i);
		else if (isz == 2) vi = M16(iaddr + i * 2);
		if (!rt_valid(vaddr + vi * (uint32_t)f.size * (uint32_t)f.morphs, (uint32_t)f.size * (uint32_t)f.morphs)) return true;
		RawVert rv = {0};
		if (f.morphs == 1) {
			read_vertex(&f, MP(vaddr + vi * (uint32_t)f.size), &rv);
		}
		else {
			for (int m = 0; m < f.morphs; m++) {
				RawVert t = {0};
				read_vertex(&f, MP(vaddr + (vi * (uint32_t)f.morphs + (uint32_t)m) * (uint32_t)f.size), &t);
				for (int q = 0; q < 3; q++) rv.pos[q] += t.pos[q] * mw[m];
				for (int q = 0; q < 8; q++) rv.w[q] += t.w[q] * mw[m];
			}
		}
		float p[3] = {rv.pos[0], rv.pos[1], rv.pos[2]};
		if (f.wt) {  // skinning
			float sp[3] = {0, 0, 0};
			for (int b = 0; b < f.nw; b++) {
				float tp[3];
				if (rv.w[b] == 0.f) continue;
				mul43(bone + b * 12, rv.pos, 1.f, tp);
				for (int q = 0; q < 3; q++) sp[q] += tp[q] * rv.w[b];
			}
			memcpy(p, sp, sizeof p);
		}
		float wp[3];
		mul43(world, p, 1.f, wp);
		double c4[4];
		for (int r = 0; r < 4; r++) c4[r] = PV[r * 4] * wp[0] + PV[r * 4 + 1] * wp[1] + PV[r * 4 + 2] * wp[2] + PV[r * 4 + 3];
		float x = (float)c4[0], y = (float)c4[1], z = (float)c4[2], w = (float)c4[3];
		float xs = (2.f * x - (maxvx + minvx) * w) * ix, ys = (2.f * y - (maxvy + minvy) * w) * iy;
		float val[6] = {w - xs, w + xs, w + ys, w - ys, w + z, w - z};
		for (int pl = 0; pl < planes; pl++)
			if (val[pl] > -FLT_EPSILON) inside[pl]++;
	}
	for (int pl = 0; pl < planes; pl++)
		if (!inside[pl] && !(pl < 4 && edge[pl])) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
static GLenum gl_blend_factor(int f, bool src, uint32_t fix) {
	switch (f) {
	case 0: return src ? GL_DST_COLOR : GL_SRC_COLOR;
	case 1: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
	case 2: case 6: return GL_SRC_ALPHA;
	case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
	case 4: case 8: return GL_DST_ALPHA;
	case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
		if ((fix & 0xFFFFFF) == 0) return GL_ZERO;
		return GL_CONSTANT_COLOR;
	}
}

static const GLenum gl_func[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};

// Everything the GL needs for a draw. Draws with an identical state are merged into one batch
// (the game sends the terrain as thousands of single triangles), and only the parts that differ
// from what the GL already has are re-applied.
typedef struct {
	Fbo *fbo;
	GLenum mode;
	GLuint tex;
	int sampler;      // index into samplers[]: min linear | mag linear << 1 | clamp s << 2 | clamp t << 3 | mipmaps << 4
	int scissor[4];   // x, y, w, h in PSP pixels
	uint8_t cmask[4];
	uint8_t blend, depth_test, depth_mask, cull;
	GLenum bsrc, bdst, beq, depth_func, cull_face;
	float bcolor[3];
	int32_t clear_mode, tex_enable, tfunc, tfunc_rgba, color_double, alpha_test, at_func, fog_enable, blend_double;
	float at_ref, uvs[2], env[3], fog[3];
	int32_t at_mask;
	int32_t ct_func, ct_ref[3], ct_mask[3];  // color test (ct_func 1 = always / off)
	int32_t xbr;  // 2D upscaling filter: 0 off, else 4 * (1 xBR | 2 bicubic) + clamp s + 2 * clamp t
	// Stencil: the PSP keeps it in the framebuffer's alpha; here it is the GL stencil buffer (as in
	// PPSSPP), driven by the PSP's test and ops. Clears set it from the clear color's alpha.
	int32_t st_on, st_ref, st_mask, st_wmask;
	GLenum st_func, st_ops[3];  // stencil fail, depth fail, depth pass
	int32_t sprite;  // the vertices are SVerts (all_s)
} RenderState;

static RenderState glc;  // what the GL currently has
static bool glc_valid;

// The texture bound on unit 0 (TEX_UNKNOWN: something else bound one since).
#define TEX_UNKNOWN 0xFFFFFFFFu
static GLuint unit0 = TEX_UNKNOWN;

// Something else (presentation, transfers) touched the GL state: re-apply everything next time.
static void gl_invalidate(void) {
	glc_valid = false;
	unit0 = TEX_UNKNOWN;
}

// An untextured draw still has a texture bound on unit 0, and the shader's sampler counts: WebGL2
// refuses the whole draw when that texture is the framebuffer being drawn into ("feedback loop")
// though the shader never samples it (desktop GL draws it). A new framebuffer's texture stays
// bound after it is made, and a draw that sampled one leaves it there: such draws were lost in
// the browser (the glow effect's buffer kept a stale glow: an aura stuck around the player, whom
// the camera keeps at the centre). Untextured draws get a blank 1x1 texture then.
// DBZ_GE_FEEDBACK=1 logs those draws.
static GLuint untextured_unit0(const RenderState *s) {
	if (unit0 != TEX_UNKNOWN && unit0 != s->fbo->tex) return unit0;
	static GLuint blank;
	if (!blank) {
		static const uint8_t px[4] = {0, 0, 0, 0};
		glGenTextures(1, &blank);
		glBindTexture(GL_TEXTURE_2D, blank);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	static int log = -1, logged;
	if (log < 0) log = getenv("DBZ_GE_FEEDBACK") != NULL;
	if (log && unit0 == s->fbo->tex && logged < 200) {
		logged++;
		rt_log("GE: untextured draw into %08x (%dx%d)%s with its own texture bound", s->fbo->addr, s->fbo->width,
		       s->fbo->height, s->clear_mode ? ", a clear," : "");
	}
	return blank;
}

static void gl_apply(const RenderState *s) {
#define CH(f) (!glc_valid || memcmp(&glc.f, &s->f, sizeof s->f))
	if (!glc_valid) {
		glUseProgram(prog);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glActiveTexture(GL_TEXTURE0);
		glEnable(GL_SCISSOR_TEST);
	}
	if (CH(fbo)) {
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo->fbo);
		glViewport(0, 0, s->fbo->width * rs, s->fbo->height * rs);
	}
	if (CH(scissor)) glScissor(s->scissor[0] * rs, s->scissor[1] * rs, s->scissor[2] * rs, s->scissor[3] * rs);
	if (CH(cmask)) glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
	if (CH(blend)) (s->blend ? glEnable : glDisable)(GL_BLEND);
	if (s->blend && (CH(bsrc) || CH(bdst))) glBlendFuncSeparate(s->bsrc, s->bdst, GL_ONE, GL_ZERO);
	if (s->blend && CH(beq)) glBlendEquation(s->beq);
	if (s->blend && CH(bcolor)) glBlendColor(s->bcolor[0], s->bcolor[1], s->bcolor[2], 1.f);
	if (CH(depth_test)) (s->depth_test ? glEnable : glDisable)(GL_DEPTH_TEST);
	if (s->depth_test && CH(depth_func)) glDepthFunc(s->depth_func);
	if (CH(depth_mask)) glDepthMask(s->depth_mask);
	if (CH(st_on)) (s->st_on ? glEnable : glDisable)(GL_STENCIL_TEST);
	if (s->st_on) s->fbo->stencil_used = true;
	if (s->st_on && (CH(st_func) || CH(st_ref) || CH(st_mask))) glStencilFunc(s->st_func, s->st_ref, (GLuint)s->st_mask);
	if (s->st_on && CH(st_ops)) glStencilOp(s->st_ops[0], s->st_ops[1], s->st_ops[2]);
	if (CH(st_wmask)) glStencilMask((GLuint)s->st_wmask);
	if (CH(cull)) (s->cull ? glEnable : glDisable)(GL_CULL_FACE);
	if (s->cull && CH(cull_face)) glCullFace(s->cull_face);
	if (CH(clear_mode)) glUniform1i(u_clearMode, s->clear_mode);
	if (CH(tex_enable)) glUniform1i(u_texEnable, s->tex_enable);
	if (CH(tfunc)) glUniform1i(u_tfunc, s->tfunc);
	if (CH(tfunc_rgba)) glUniform1i(u_tfuncRGBA, s->tfunc_rgba);
	if (CH(color_double)) glUniform1i(u_colorDouble, s->color_double);
	if (CH(alpha_test)) glUniform1i(u_alphaTest, s->alpha_test);
	if (CH(at_func)) glUniform1i(u_atFunc, s->at_func);
	if (CH(at_ref)) glUniform1f(u_atRef, s->at_ref);
	if (CH(at_mask)) glUniform1i(u_atMask, s->at_mask);
	if (CH(fog_enable)) glUniform1i(u_fogEnable, s->fog_enable);
	if (CH(fog)) glUniform3f(u_fogColor, s->fog[0], s->fog[1], s->fog[2]);
	if (CH(blend_double)) glUniform1i(u_blendDouble, s->blend_double);
	if (CH(ct_func)) glUniform1i(u_ctFunc, s->ct_func);
	if (CH(xbr)) glUniform1i(u_xbr, s->xbr);
	if (CH(ct_ref)) glUniform3i(u_ctRef, s->ct_ref[0], s->ct_ref[1], s->ct_ref[2]);
	if (CH(ct_mask)) glUniform3i(u_ctMask, s->ct_mask[0], s->ct_mask[1], s->ct_mask[2]);
	if (CH(uvs)) glUniform2f(u_uvScale, s->uvs[0], s->uvs[1]);
	if (CH(env)) glUniform4f(u_envColor, s->env[0], s->env[1], s->env[2], 1.f);
	GLuint t0 = s->tex_enable ? s->tex : untextured_unit0(s);
	if (t0 != unit0) {
		glBindTexture(GL_TEXTURE_2D, t0);
		unit0 = t0;
	}
	if (s->tex_enable && CH(sampler)) glBindSampler(0, samplers[s->sampler]);
#undef CH
	glc = *s;
	if (!s->tex_enable) glc.sampler = -1;  // not bound: compare as different next time
	glc_valid = true;
}

// Batches: draws in a row with the same state share one glDrawElements. Vertices go to the GPU once
// each (all_v) and the triangles as 32-bit indices into them (all_i): a strip of n vertices costs n
// vertices and 3(n-2) indices instead of 3(n-2) vertices (a match draws ~40,000 strip vertices a
// frame; as plain triangles that was ~8 MB of vertices a frame). A batch that ends (another state
// comes) waits, recorded with its state; the batches go to the GPU together (glBufferSubData into
// ring buffers made anew only when full) and are drawn in order when something needs the drawing
// done (batch_flush: another render target, a texture refilled, the frame shown...) or when
// FLUSH_BYTES are queued: the browser passes uploads to its GPU process through a shared buffer of a
// few megabytes, and one huge upload a frame waits for it, where pieces overlap with the GPU's work.
typedef struct {
	RenderState st;
	uint32_t first, n;  // indices
} BatchRec;
static BatchRec *recs;
static size_t n_recs, recs_cap;
static GVert *all_v;
static size_t all_n, all_cap;  // the queued vertices (the open batch's included)
static SVert *all_s;
static size_t all_ns, all_scap;  // the queued sprite corners: sprite batches index these
static uint32_t *all_i;
static size_t all_ni, all_icap;  // the queued indices (into all_v, or all_s for a sprite batch)
static size_t batch_first;      // the open batch's first index (all_i + batch_first ..all_ni)
static RenderState batch_state;
static uint64_t stat_draws, stat_batches;
static uint64_t draws_total;  // every draw so far (ge_draws_total: the display's frame pacing)

static void batch_close(void) {
	if (all_ni == batch_first) return;
	if (n_recs == recs_cap) recs_cap = recs_cap * 2 + 64, recs = realloc(recs, recs_cap * sizeof *recs);
	recs[n_recs++] = (BatchRec){batch_state, (uint32_t)batch_first, (uint32_t)(all_ni - batch_first)};
	batch_first = all_ni;
}

#define VBO_RING (16u << 20)
#define IBO_RING (4u << 20)
#define FLUSH_BYTES (512u << 10)
static size_t vbo_used = VBO_RING, ibo_used = IBO_RING;  // full: the first upload makes the buffers

static size_t queued_bytes(void) { return all_n * sizeof(GVert) + all_ns * sizeof(SVert) + all_ni * sizeof(uint32_t); }

// Room for nv more vertices (sprite corners if sprite) and ni more indices of the open batch.
static void batch_reserve(bool sprite, size_t nv, size_t ni) {
	if (!sprite && all_n + nv > all_cap) all_cap = (all_n + nv) * 2 + 4096, all_v = realloc(all_v, all_cap * sizeof(GVert));
	if (sprite && all_ns + nv > all_scap) all_scap = (all_ns + nv) * 2 + 1024, all_s = realloc(all_s, all_scap * sizeof(SVert));
	if (all_ni + ni > all_icap) all_icap = (all_ni + ni) * 2 + 4096, all_i = realloc(all_i, all_icap * sizeof(uint32_t));
}

// Uploads bytes of data to the ring buffer bound to target; returns its offset there.
static size_t ring_upload(GLenum target, size_t ring, size_t *used, const void *data, size_t bytes) {
	if (bytes > ring) {  // bigger than the whole ring: a buffer of its own
		glBufferData(target, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
		*used = ring;
		return 0;
	}
	if (*used + bytes > ring) {
		glBufferData(target, (GLsizeiptr)ring, NULL, GL_STREAM_DRAW);
		*used = 0;
	}
	size_t at = *used;
	glBufferSubData(target, (GLintptr)at, (GLsizeiptr)bytes, data);
	*used += (bytes + 15) & ~(size_t)15;
	return at;
}

// Points the vertex attributes at GVerts (or SVerts) at offset at of the vertex buffer. Attribute 5,
// the sprite rectangle, is a constant "anywhere" for the others.
static void vertex_layout(size_t at, bool sprite) {
	GLsizei stride = sprite ? sizeof(SVert) : sizeof(GVert);
#define ATTR(i, n, type, norm, f) glVertexAttribPointer(i, n, type, norm, stride, (void *)(at + offsetof(GVert, f)))
	ATTR(0, 4, GL_FLOAT, GL_FALSE, x);
	ATTR(1, 2, GL_FLOAT, GL_FALSE, u);
	ATTR(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, c0);
	ATTR(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, c1);
	ATTR(4, 1, GL_FLOAT, GL_FALSE, fog);
#undef ATTR
	if (sprite) {
		glEnableVertexAttribArray(5);
		glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void *)(at + offsetof(SVert, box)));
	}
	else {
		glDisableVertexAttribArray(5);
		glVertexAttrib4f(5, -1e9f, -1e9f, 1e9f, 1e9f);
	}
}

// Draws everything queued. Called whenever the GL is about to read or change something a queued
// draw depends on (textures, framebuffers, presentation).
static void batch_flush(void) {
	batch_close();
	if (n_recs) {
		gl_apply(&recs[0].st);  // (the vertex array and buffers bound)
		// The vertices and the sprite corners share the ring: made anew for the corners, it would
		// lose the vertices just put in the old one (every ~5 s the main menu's background, its
		// only vertices, came out black under the sprites of its interface). Room for both first.
		size_t vbytes = (all_n * sizeof(GVert) + 15) & ~(size_t)15, both = vbytes + all_ns * sizeof(SVert);
		if (all_n && all_ns && vbo_used + both > VBO_RING && both <= VBO_RING) {
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VBO_RING, NULL, GL_STREAM_DRAW);
			vbo_used = 0;
		}
		size_t vat = all_n ? ring_upload(GL_ARRAY_BUFFER, VBO_RING, &vbo_used, all_v, all_n * sizeof(GVert)) : 0;
		size_t sat = all_ns ? ring_upload(GL_ARRAY_BUFFER, VBO_RING, &vbo_used, all_s, all_ns * sizeof(SVert)) : 0;
		size_t iat = ring_upload(GL_ELEMENT_ARRAY_BUFFER, IBO_RING, &ibo_used, all_i, all_ni * sizeof(uint32_t));
		int layout = -1;
		for (size_t i = 0; i < n_recs; i++) {
			const RenderState *st = &recs[i].st;
			gl_apply(st);
			if (layout != st->sprite) layout = st->sprite, vertex_layout(layout ? sat : vat, layout);
			glDrawElements(st->mode, (GLsizei)recs[i].n, GL_UNSIGNED_INT, (void *)(iat + recs[i].first * sizeof(uint32_t)));
			stat_batches++;
		}
		n_recs = 0;
	}
	all_n = all_ns = all_ni = batch_first = 0;
}

// Starts or continues the batch for a draw with state s and room for nv vertices and ni indices;
// the caller then appends them (indices count from all_v, or all_s for sprites). Another render
// target draws what is queued first: what reads a framebuffer counts on its draws being done once
// another one is drawn to.
static void batch_begin(const RenderState *s, size_t nv, size_t ni) {
	bool open = all_ni > batch_first, same = open && s->mode == GL_TRIANGLES && !memcmp(s, &batch_state, sizeof *s);
	if (queued_bytes() >= FLUSH_BYTES || ((open || n_recs) && s->fbo != (open ? batch_state.fbo : recs[n_recs - 1].st.fbo)))
		batch_flush();
	else if (open && !same)
		batch_close();
	if (all_ni == batch_first) batch_state = *s;
	batch_reserve(s->sprite, nv, ni);
	stat_draws++;
	draws_total++;
}

// The PSP state of the current draw.
static void build_state(RenderState *s, bool through, bool clear) {
	memset(s, 0, sizeof *s);
	s->fbo = cur_fbo;
	s->mode = GL_TRIANGLES;
	s->sampler = 0;
	int x1 = (int)(reg[0xD4] & 0x3FF), y1 = (int)((reg[0xD4] >> 10) & 0x3FF);
	int x2 = (int)(reg[0xD5] & 0x3FF), y2 = (int)((reg[0xD5] >> 10) & 0x3FF);
	s->scissor[0] = x1;
	s->scissor[1] = y1;
	s->scissor[2] = x2 - x1 + 1 > 0 ? x2 - x1 + 1 : 0;
	s->scissor[3] = y2 - y1 + 1 > 0 ? y2 - y1 + 1 : 0;
	s->uvs[0] = s->uvs[1] = 1.f;
	if (clear) {
		uint32_t cm = reg[0xD3];
		bool color = cm & 0x100, alpha = cm & 0x200, depth = cm & 0x400;
		s->clear_mode = 1;
		s->cmask[0] = s->cmask[1] = s->cmask[2] = color;
		s->cmask[3] = alpha;
		s->depth_test = 1;
		s->depth_func = GL_ALWAYS;
		s->depth_mask = depth;
		if (alpha) {  // the stencil takes the clear color's alpha (st_ref, set from the vertices)
			s->st_on = 1;
			s->st_func = GL_ALWAYS;
			s->st_mask = 0xFF;
			s->st_wmask = 0xFF;
			s->st_ops[0] = s->st_ops[1] = s->st_ops[2] = GL_REPLACE;
		}
		return;
	}
	if (reg[0x24] & 1) {
		static const GLenum ops[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};
		uint32_t dc = reg[0xDC], dd = reg[0xDD];
		s->st_on = 1;
		s->st_func = gl_func[dc & 7];
		s->st_ref = (int32_t)((dc >> 8) & 0xFF);
		s->st_mask = (int32_t)((dc >> 16) & 0xFF);
		s->st_ops[0] = ops[dd & 7];
		s->st_ops[1] = ops[(dd >> 8) & 7];
		s->st_ops[2] = ops[(dd >> 16) & 7];
		s->st_wmask = (int32_t)(~reg[0xE9] & 0xFF);
		// Drawing only into the stencil (color masked, the same op whatever the depth test says):
		// the PSP writes it everywhere the stencil test passes; PPSSPP treats the test as ALWAYS.
		bool zt = reg[0x23] & 1;
		if (((dd & 7) == ((dd >> 8) & 7) || !zt) && (dd & 7) == ((dd >> 16) & 7) && (reg[0xE8] & 0xFFFFFF) == 0xFFFFFF &&
		    (!zt || (reg[0xE7] & 1)))
			s->st_func = GL_ALWAYS;
	}
	// The framebuffer alpha channel is the stencil buffer: it is only written when the stencil
	// test is on and some stencil op modifies it (otherwise blending reads a stencil-like alpha).
	uint32_t mrgb = reg[0xE8], ma = reg[0xE9], sop = reg[0xDD];
	bool stencil_writes = (reg[0x24] & 1) && (ma & 0xFF) != 0xFF &&
	                      ((sop & 0xFF) != 2 || ((sop >> 8) & 0xFF) != 2 || ((sop >> 16) & 0xFF) != 2);
	s->cmask[0] = (mrgb & 0xFF) != 0xFF;
	s->cmask[1] = (mrgb & 0xFF00) != 0xFF00;
	s->cmask[2] = (mrgb & 0xFF0000) != 0xFF0000;
	s->cmask[3] = stencil_writes;
	if (reg[0x21] & 1) {
		uint32_t bm = reg[0xDF];
		int sf = bm & 0xF, df = (bm >> 4) & 0xF, eq = (bm >> 8) & 7;
		s->blend = 1;
		s->bsrc = gl_blend_factor(sf, true, reg[0xE0]);
		s->bdst = gl_blend_factor(df, false, reg[0xE1]);
		// DOUBLESRCALPHA / ONE_MINUS_DOUBLESRCALPHA: the source alpha doubled (to 1 at most) for
		// both factors (games with alpha 0x80 as "opaque" blend src * 1 + dst * 0 with them)
		// DOUBLESRCALPHA / DOUBLEINVSRCALPHA: the alpha doubled (2), or (GE_BLEND_DOUBLE_ALPHA 0, the
		// older way) the source color for DOUBLESRCALPHA as the source factor (1)
		s->blend_double = GE_BLEND_DOUBLE_ALPHA ? (sf == 6 || sf == 7 || df == 6 || df == 7 ? 2 : 0) : sf == 6;
		if (s->bsrc == GL_CONSTANT_COLOR || s->bdst == GL_CONSTANT_COLOR) {
			uint32_t fix = s->bsrc == GL_CONSTANT_COLOR ? reg[0xE0] : reg[0xE1];
			s->bcolor[0] = (fix & 0xFF) / 255.f;
			s->bcolor[1] = ((fix >> 8) & 0xFF) / 255.f;
			s->bcolor[2] = ((fix >> 16) & 0xFF) / 255.f;
		}
		static const GLenum eqs[8] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX,
		                              GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_ADD};
		s->beq = eqs[eq];
	}
	if (reg[0x23] & 1) {
		s->depth_test = 1;
		s->depth_func = gl_func[reg[0xDE] & 7];
	}
	s->depth_mask = !(reg[0xE7] & 1) && (reg[0x23] & 1);
	static int nocull = -1;  // DBZ_NOCULL: draw both faces (debugging)
	if (nocull < 0) nocull = getenv("DBZ_NOCULL") != NULL;
	if (!through && (reg[0x1D] & 1) && !nocull) {
		s->cull = 1;
		s->cull_face = (reg[0x9B] & 1) ? GL_BACK : GL_FRONT;
	}
	s->alpha_test = reg[0x22] & 1;
	s->ct_func = (reg[0x27] & 1) ? (int)(reg[0xD8] & 3) : 1;
	for (int k = 0; k < 3; k++) {
		s->ct_ref[k] = (int)((reg[0xD9] >> (8 * k)) & 0xFF);
		s->ct_mask[k] = (int)((reg[0xDA] >> (8 * k)) & 0xFF);
	}
	s->at_func = (int)(reg[0xDB] & 7);
	s->at_ref = (float)((reg[0xDB] >> 8) & 0xFF);
	s->at_mask = (int32_t)((reg[0xDB] >> 16) & 0xFF);
	s->fog_enable = !through && (reg[0x1F] & 1);
	uint32_t fc = reg[0xCF];
	s->fog[0] = (fc & 0xFF) / 255.f;
	s->fog[1] = ((fc >> 8) & 0xFF) / 255.f;
	s->fog[2] = ((fc >> 16) & 0xFF) / 255.f;
	uint32_t tf = reg[0xC9];
	s->tfunc = (int)(tf & 7);
	s->tfunc_rgba = (tf >> 8) & 1;
	s->color_double = (tf >> 16) & 1;
	uint32_t ec = reg[0xCA];
	s->env[0] = (ec & 0xFF) / 255.f;
	s->env[1] = ((ec >> 8) & 0xFF) / 255.f;
	s->env[2] = ((ec >> 16) & 0xFF) / 255.f;
}

static uint32_t prim_cmd_addr;  // list address of the PRIM being drawn (debug dumps)
static GVert *vbuf;
static size_t vbuf_cap;
static int dbg_prims;

static void draw_prim(uint32_t data) {
	int count = (int)(data & 0xFFFF), type = (int)((data >> 16) & 7);
	if (count <= 0) return;
	VFmt f = vfmt_decode(reg[0x12]);
	uint32_t vaddr = vertex_addr;
	uint32_t iaddr = index_addr;
	if (!rt_valid(vaddr, 1)) {
		if (gedebug) rt_log("GE prim skipped: bad vertex address %08x", vaddr);
		return;
	}

	bind_target();
	bool clear = reg[0xD3] & 1;
	bool tex_on = (reg[0x1E] & 1) && !clear;
	int tw = 0, th = 0;
	float uvsx = 1, uvsy = 1;
	GLuint tex = 0;
	if (tex_on) {
		tex = texture_get(&tw, &th, &uvsx, &uvsy);
		if (!tex) tex_on = false;
	}
	if (tex_on && tex == cur_fbo->tex) {
		// Sampling the framebuffer being drawn into is undefined in GL and an error in WebGL2:
		// sample a snapshot instead.
		batch_flush();
		static GLuint snap;
		static int snap_w, snap_h;
		int w = cur_fbo->width * rs, h = cur_fbo->height * rs;
		if (!snap) glGenTextures(1, &snap);
		glBindTexture(GL_TEXTURE_2D, snap);
		if (snap_w != w || snap_h != h) {
			snap_w = w;
			snap_h = h;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, cur_fbo->fbo);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
		gl_invalidate();
		tex = snap;
	}
	RenderState st;
	build_state(&st, f.through, clear);
	st.tex_enable = tex_on;
	// A framebuffer read as a 565 texture has no alpha (the GE reads 1): ours keeps whatever alpha
	// the draws left (a game's bloom reads its quarter-size copy so, and its glow came out patchy,
	// the scenery showing through the fighters)
	if (tex_on && tex_from_fbo && (reg[0xC3] & 0xF) == 0 && st.tfunc_rgba) st.tfunc_rgba = 2;
	{  // A framebuffer read as a CLUT texture (its pixels as palette indices: the fight's shadows, an
		// ID buffer with one bit per character projected onto the stage) is not done yet: drawn with
		// its colors as they are, it painted the characters black; left out (DBZ_FBO_CLUT=1 draws it).
		static int raw = -1;
		if (raw < 0) raw = getenv("DBZ_FBO_CLUT") ? atoi(getenv("DBZ_FBO_CLUT")) != 0 : !GE_FBO_CLUT_SKIP;
		if (!raw && tex_on && tex_from_fbo && (reg[0xC3] & 0xF) >= 4) return;
	}
	if (tex_on) {
		uint32_t flt = reg[0xC6], wrap = reg[0xC7];
		st.tex = tex;
		st.uvs[0] = uvsx;
		st.uvs[1] = uvsy;
		st.sampler = (int)((flt & 1) | ((flt >> 8) & 1) << 1 | (wrap & 1) << 2 | ((wrap >> 8) & 1) << 3);
		// The 2D interface (through mode, ordinary textures) goes through the xBR filter above 1x.
		static int mips_on = -1;
		if (mips_on < 0) mips_on = !getenv("DBZ_TEXPACK_MIPS") || atoi(getenv("DBZ_TEXPACK_MIPS")) != 0;
		if (tex_scale > 1.f) st.sampler |= 3 | (tex_mips && mips_on ? 16 : 0);  // finer than the GE's texture: smooth sampling
		else if (tex_filter && rs > 1 && !tex_from_fbo) {
			st.xbr = 4 * (tex_filter == 1 ? 1 : 3) + (int)(wrap & 1) + 2 * (int)((wrap >> 8) & 1);
			if (tex_filter == 2) st.sampler |= 3;  // the sharp pixels' edges come from bilinear
		} else if (ui_filter && rs > 1 && f.through && !tex_from_fbo)
			st.xbr = 4 * ui_filter + (int)(wrap & 1) + 2 * (int)((wrap >> 8) & 1);
	}

	// Decode + transform every referenced vertex.
	if ((size_t)count > vbuf_cap) {
		vbuf_cap = (size_t)count * 2;
		vbuf = realloc(vbuf, vbuf_cap * sizeof(GVert));
	}
	float fbw = (float)cur_fbo->width, fbh = (float)cur_fbo->height;
	GVert *tv = vbuf;
	int isz = f.idx == 1 ? 1 : f.idx == 2 ? 2 : 0;
	float mw[8];
	for (int m = 0; m < 8; m++) mw[m] = f24(reg[0x2C + m]);
	bool dumping = ge_dump_state == 1;
	uint64_t h_raw = 1469598103934665603ull;
	uint32_t dump_idx[6];
	float dump_pos[6][3];
	xf_setup(&f, fbw, fbh, tw, th);
	// Indexed draws name the same vertex several times: each one is transformed once per draw.
	enum { VCACHE = 4096 };
	static uint32_t vc_tag[VCACHE], vc_gen[VCACHE], vc_now;
	static GVert vc_val[VCACHE];
	bool vcache = isz != 0 && !dumping;
	if (vcache && ++vc_now == 0) memset(vc_gen, 0, sizeof vc_gen), vc_now = 1;
	for (int i = 0; i < count; i++) {
		uint32_t vi = (uint32_t)i;
		if (isz == 1) vi = M8(iaddr + (uint32_t)i);
		else if (isz == 2) vi = M16(iaddr + (uint32_t)i * 2);
		uint32_t slot = vi & (VCACHE - 1);
		if (vcache && vc_gen[slot] == vc_now && vc_tag[slot] == vi) {
			tv[i] = vc_val[slot];
			continue;
		}
		// (only what read_vertex may leave unwritten and a later step reads is cleared: zeroing the
		// whole vertex was a memset call per vertex, millions a second in the browser)
		RawVert rv;
		if (f.morphs == 1) {
			if (dumping) memset(&rv, 0, sizeof rv);  // (the dump hashes it whole)
			else rv.uv[0] = rv.uv[1] = 0, rv.nrm[0] = rv.nrm[1] = rv.nrm[2] = 0;
			read_vertex(&f, MP(vaddr + vi * (uint32_t)f.size * 1), &rv);
		}
		else {  // morph targets are stored back to back
			memset(&rv, 0, sizeof rv);
			for (int m = 0; m < f.morphs; m++) {
				RawVert t = {0};
				read_vertex(&f, MP(vaddr + (vi * (uint32_t)f.morphs + (uint32_t)m) * (uint32_t)f.size), &t);
				for (int k = 0; k < 3; k++) rv.pos[k] += t.pos[k] * mw[m], rv.nrm[k] += t.nrm[k] * mw[m];
				for (int k = 0; k < 2; k++) rv.uv[k] += t.uv[k] * mw[m];
				for (int k = 0; k < 8; k++) rv.w[k] += t.w[k] * mw[m];
				if (t.has_col) {
					for (int k = 0; k < 4; k++) rv.col[k] = (uint8_t)(rv.col[k] + t.col[k] * mw[m]);
					rv.has_col = true;
				}
			}
		}
		if (dumping) {
			h_raw = hash_bytes((const uint8_t *)&rv, sizeof rv) ^ (h_raw * 31);
			if (i < 6) dump_idx[i] = vi, memcpy(dump_pos[i], rv.pos, sizeof dump_pos[i]);
		}
		transform(&f, &rv, &tv[i]);
		if (vcache) vc_gen[slot] = vc_now, vc_tag[slot] = vi, vc_val[slot] = tv[i];
	}
	if (clear && st.st_on && count > 0) st.st_ref = tv[count - 1].c0[3];  // the clear's stencil value
	if (tex_on && !tex_from_fbo && texcat_dir && texcat_dir != (const char *)1 && last_slot >= 0 && texcache[last_slot].tex == tex &&
	    texcache[last_slot].cat_key && (f.through || (reg[0xC0] & 3) == 0)) {
		texcat_extent(texcache[last_slot].cat_key, tv, count, tw, th);
		if (f.through && !GE_TILES_PACK_KEY) tiles_log(texcache[last_slot].cat_key, tw, th, tv, count, type, fbw, fbh, tw, th, uvsx, uvsy);
	}
	if (GE_TILES_PACK_KEY && f.through && tex_on && last_slot >= 0 && texcache[last_slot].tex == tex && texcache[last_slot].tile_key &&
	    tiles_enabled()) {
		const TexEntry *e = &texcache[last_slot];
		tiles_log(e->tile_key, e->tile_iw, e->tile_ih, tv, count, type, fbw, fbh, tw, th, uvsx, uvsy);
	}
	// DBZ_GE_BIG=1 (diagnostics): blended draws of a few vertices covering most of the screen
	static int ge_big = -1;
	if (ge_big < 0) ge_big = getenv("DBZ_GE_BIG") != NULL;
	if (ge_big && !clear && (reg[0x21] & 1) && count > 0 && count <= 12) {
		float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
		for (int i = 0; i < count; i++) {
			float w = tv[i].w != 0 ? tv[i].w : 1e-9f;
			float x = tv[i].x / w, y = tv[i].y / w;
			bx0 = fminf(bx0, x), by0 = fminf(by0, y), bx1 = fmaxf(bx1, x), by1 = fmaxf(by1, y);
		}
		float cx = fmaxf(0, fminf(bx1, 1) - fmaxf(bx0, -1)), cy = fmaxf(0, fminf(by1, 1) - fmaxf(by0, -1));
		if (cx * cy > 0.7f * 4) {
			rt_log("GE big: frame %llu fb=%06x type=%d n=%d through=%d tex=%d@%08x %dx%d fmt=%d c0=%02x%02x%02x%02x "
			       "blend=%03x/%06x/%06x st=%d func=%x op=%x zt=%d/%d bb=%.2f,%.2f,%.2f,%.2f w0=%.2f",
			       (unsigned long long)frame_no, cur_fbo->addr, type, count, f.through, tex_on,
			       ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), tw, th, (int)(reg[0xC3] & 0xF),
			       tv[0].c0[0], tv[0].c0[1], tv[0].c0[2], tv[0].c0[3], reg[0xDF], reg[0xE0], reg[0xE1], reg[0x24] & 1,
			       reg[0xDC], reg[0xDD], reg[0x23] & 1, reg[0xDE] & 7, bx0, by0, bx1, by1, tv[0].w);
		}
	}
	if (dumping) {
		FILE *df = ge_dump_file;
		int draw_i = ge_dump_draws++;
		uint64_t h_m = hash_bytes((const uint8_t *)world, sizeof world) ^ hash_bytes((const uint8_t *)view, sizeof view) * 3 ^
		               hash_bytes((const uint8_t *)proj, sizeof proj) * 7;
		if (f.wt) h_m ^= hash_bytes((const uint8_t *)bone, sizeof(float) * 12 * (size_t)f.nw) * 13;
		float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f, wmin = 1e30f;
		for (int i = 0; i < count; i++) {
			float w = tv[i].w != 0 ? tv[i].w : 1e-9f;
			float x = tv[i].x / w, y = tv[i].y / w;
			bx0 = fminf(bx0, x), by0 = fminf(by0, y), bx1 = fmaxf(bx1, x), by1 = fmaxf(by1, y), wmin = fminf(wmin, tv[i].w);
		}
		fprintf(df, "D2 %d t=%d n=%d vt=%06x raw=%016llx mtx=%016llx bb=%.3f,%.3f,%.3f,%.3f wmin=%.3f v=%08x i=%08x at=%08x fb=%06x tex=%d@%08x tm=%x"
		        " blend=%d/%06x fix=%06x/%06x tfunc=%06x psm=%x tfmt=%x atest=%d/%06x stest=%d/%06x zt=%d/%x zmsk=%x cmask=%06x/%02x lmode=%d\n",
		        draw_i, type, count, reg[0x12], (unsigned long long)h_raw, (unsigned long long)h_m, bx0, by0, bx1, by1, wmin, vaddr,
		        iaddr, prim_cmd_addr, cur_fbo->addr, tex_on, tex_on ? (((reg[0xA8] & 0xFF0000u) << 8) | (reg[0xA0] & 0xFFFFFFu)) : 0,
		        reg[0xC0], reg[0x21] & 1, reg[0xDF] & 0xFFFFFF, reg[0xE0] & 0xFFFFFF, reg[0xE1] & 0xFFFFFF, reg[0xC9] & 0xFFFFFF,
		        reg[0xD2] & 3, reg[0xC3] & 0xF, reg[0x22] & 1, reg[0xDB] & 0xFFFFFF, reg[0x24] & 1, reg[0xDC] & 0xFFFFFF,
		        reg[0x23] & 1, reg[0xDE] & 7, reg[0xE7] & 1, reg[0xE8] & 0xFFFFFF, reg[0xE9] & 0xFF, reg[0x17] & 1);
		// Vertices of DBZ_GEDUMP_VERTS=vtype:mincount draws, and of small draws spanning more
		// than the screen (the shape of a broken triangle).
		static int want_n = -2;
		static unsigned want_vt;
		if (want_n == -2) {
			want_n = -1;
			if (getenv("DBZ_GEDUMP_VERTS")) sscanf(getenv("DBZ_GEDUMP_VERTS"), "%x:%d", &want_vt, &want_n);
		}
		bool big = count <= 6 && (bx1 - bx0 > 2.5f || by1 - by0 > 2.5f);
		if (big || (want_n >= 0 && reg[0x12] == want_vt && count >= want_n)) {
			float vsx = f24(reg[0x42]), vsy = f24(reg[0x43]), vsz = f24(reg[0x44]);
			float vcx = f24(reg[0x45]), vcy = f24(reg[0x46]), vcz = f24(reg[0x47]);
			fprintf(df, "  viewport s=(%g %g %g) c=(%g %g %g) off=(%g %g) clip=%x minz=%x maxz=%x blend=%d zt=%d/%d zw=%d tex=%d fog=%d cull=%d\n",
			        vsx, vsy, vsz, vcx, vcy, vcz, (reg[0x4C] & 0xFFFF) / 16.f, (reg[0x4D] & 0xFFFF) / 16.f, reg[0x1C], reg[0xD6],
			        reg[0xD7], reg[0x21] & 1, reg[0x23] & 1, reg[0xDE] & 7, !(reg[0xE7] & 1), tex_on, reg[0x1F] & 1, reg[0x1D] & 1);
			if (ctx_dumped < 4 && rt_valid(prim_cmd_addr - 192, 196)) {
				ctx_dumped++;
				fprintf(df, "  list before PRIM:");
				for (uint32_t a = prim_cmd_addr - 192; a <= prim_cmd_addr; a += 4) fprintf(df, " %08x", M32(a));
				fprintf(df, "\n");
			}
			if (f.wt) {
				fprintf(df, "  bone0=");
				for (int k = 0; k < 12; k++) fprintf(df, "%g%c", bone[k], k == 11 ? '\n' : ' ');
			}
			fprintf(df, "  world=");
			for (int k = 0; k < 12; k++) fprintf(df, "%g%c", world[k], k == 11 ? '\n' : ' ');
			for (int i = 0; i < count; i++) {
				const GVert *g = &tv[i];
				fprintf(df, "  v%d", i);
				if (i < 6) fprintf(df, " idx=%u pos=(%g %g %g)", dump_idx[i], dump_pos[i][0], dump_pos[i][1], dump_pos[i][2]);
				fprintf(df, " clip=(%.2f %.2f %.2f %.2f) ndc=(%.3f %.3f %.3f) c=%02x%02x%02x%02x uv=(%.3f %.3f)\n", g->x, g->y, g->z,
				        g->w, g->x / g->w, g->y / g->w, g->z / g->w, g->c0[0], g->c0[1], g->c0[2], g->c0[3], g->u, g->v);
			}
		}
	}

	// The GE's vertex range culling: a primitive with a vertex outside the screen space it can
	// address (x, y of 0..4096 before the drawing offset) is not drawn at all, nor (without depth
	// clamping) one with a vertex outside the depth range; a vertex behind the near plane is
	// clipped instead (as PPSSPP has it). The GL would draw such triangles stretched to the edge of
	// the screen: polygons out of nowhere in some games' stages. cull[i]: tv[i] is out.
	static uint8_t *cull;
	static size_t cull_cap;
	bool any_cull = false;
	if (!f.through && count > 0) {
		if ((size_t)count > cull_cap) cull_cap = (size_t)count * 2, cull = realloc(cull, cull_cap);
		static int off = -1;
		if (off < 0) off = getenv("DBZ_NO_RANGE_CULL") != NULL;
		float offx = (float)(reg[0x4C] & 0xFFFF) / 16.f, offy = (float)(reg[0x4D] & 0xFFFF) / 16.f;
		float vsz = f24(reg[0x44]), vcz = f24(reg[0x47]);
		bool clamp = reg[0x1C] & 1;  // depth clamping (CLIPENABLE)
		for (int i = 0; i < count; i++) {
			const GVert *g = &tv[i];
			uint8_t out = 0;
			if (!off && g->w != 0 && vsz != 0) {
				float iw = 1.f / g->w;
				// back to the GE's own projected z (the viewport may turn depth around)
				float pz = ((g->z * iw + 1.f) * 32767.5f - vcz) / vsz;
				if (!clamp && (pz < -1.00003f || pz > 1.00003f)) out = 1;
				if (!clamp || pz * g->w > -g->w) {  // not clipped by the near plane
					float X = (g->x * iw + 1.f) * fbw * 0.5f + offx, Y = (g->y * iw + 1.f) * fbh * 0.5f + offy;
					if (X < 0 || X >= 4096.f || Y < 0 || Y >= 4096.f) out = 1;
				}
			}
			cull[i] = out;
			any_cull |= out;
		}
		static int log = -1;  // DBZ_CULL_LOG=1: how many draws lost vertices to it
		if (log < 0) log = getenv("DBZ_CULL_LOG") != NULL;
		if (log && any_cull && (type == 3 || type == 4 || type == 5)) {  // CULLDBG: culled triangles reaching the screen
			static uint64_t lastf; static int n_this;
			if (frame_no != lastf) lastf = frame_no, n_this = 0;
			for (int i = 2; i < count && n_this < 3; i++) {
				int a = type == 5 ? 0 : i - 2, b = i - 1, c3 = i;
				if (type == 3 && i % 3 != 2) continue;
				if (!(cull[a] | cull[b] | cull[c3])) continue;
				int vis = 0; const int idx[3] = {a, b, c3};
				char buf[300]; int o = 0;
				for (int k = 0; k < 3; k++) {
					const GVert *g = &tv[idx[k]];
					float nx = g->w > 0 ? g->x / g->w : 99, ny = g->w > 0 ? g->y / g->w : 99;
					if (fabsf(nx) <= 1 && fabsf(ny) <= 1) vis = 1;
					o += snprintf(buf + o, sizeof buf - o, " (%.1f %.1f w=%.2f c=%d)", nx, ny, g->w, cull[idx[k]]);
				}
				if (vis) {
					n_this++;
					rt_log("CULLVIS f%llu vt=%06x type=%d n=%d tex=%08x v=%08x tri%d%s", (unsigned long long)frame_no, reg[0x12], type, count,
					       ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), vaddr, i, buf);
				}
			}
		}
		if (log) {
			static uint64_t n_draws, n_hit, last;
			n_draws++, n_hit += any_cull;
			if (frame_no - last >= 300) rt_log("range cull: %llu of %llu draws had vertices out", (unsigned long long)n_hit, (unsigned long long)n_draws), n_draws = n_hit = 0, last = frame_no;
		}
		static int show = -1;  // DBZ_CULL_SHOW=1 (tests): culled vertices drawn magenta instead of culled
		if (show < 0) show = getenv("DBZ_CULL_SHOW") != NULL;
		if (show && any_cull) {
			for (int i = 0; i < count; i++)
				if (cull[i]) tv[i].c0[0] = 255, tv[i].c0[1] = 0, tv[i].c0[2] = 255, tv[i].c0[3] = 255;
			any_cull = false;
		}
	}
	// Assemble GL triangles (sprites -> two triangles, strips/fans -> lists) straight into the batch.
	static int nodraw = -1;
	if (nodraw < 0) nodraw = getenv("DBZ_NODRAW") != NULL;
	{  // DBZ_SKIPDRAW=a-b (tests): the frame's draws a..b left out (finding what draws what)
		static int skip_a = -2, skip_b;
		static uint64_t fr;
		static int nth;
		if (skip_a == -2) {
			skip_a = -1;
			if (getenv("DBZ_SKIPDRAW")) sscanf(getenv("DBZ_SKIPDRAW"), "%d-%d", &skip_a, &skip_b);
		}
		if (fr != frame_no) fr = frame_no, nth = 0;
		int me = nth++;
		if (skip_a >= 0 && me >= skip_a && me <= skip_b) return;
	}
	st.mode = type == 0 ? GL_POINTS : type == 1 ? GL_LINES : type == 2 ? GL_LINE_STRIP : GL_TRIANGLES;
	if (!nodraw && type <= 6) {
		st.sprite = type == 6;
		batch_begin(&st, (size_t)count * 2, (size_t)count * 3);
		GVert *o = all_v + all_n;
		uint32_t *ix = all_i + all_ni;
		uint32_t b0 = (uint32_t)all_n;
		size_t nv = 0, ni = 0;
		switch (type) {
		case 0: case 1: case 2: case 3: {
			nv = type == 3 ? (size_t)count - (size_t)count % 3 : (size_t)count;
			memcpy(o, tv, sizeof(GVert) * nv);
			size_t per = type == 0 ? 1 : type == 3 ? 3 : 2;
			for (size_t i = 0; i < nv; i++) {
				if (any_cull && type != 2 && i % per == 0) {  // the whole primitive goes when one of its vertices is out
					bool out = false;
					for (size_t k = i; k < i + per && k < nv; k++) out |= cull[k];
					if (out) { i += per - 1; continue; }
				}
				ix[ni++] = b0 + (uint32_t)i;
			}
			break;
		}
		case 4:
			if (count < 3) break;
			nv = (size_t)count;
			memcpy(o, tv, sizeof(GVert) * nv);
			for (uint32_t i = 2; i < (uint32_t)count; i++) {
				if (any_cull && (cull[i] | cull[i - 1] | cull[i - 2])) continue;
				if (i & 1) { ix[ni++] = b0 + i - 1; ix[ni++] = b0 + i - 2; }
				else { ix[ni++] = b0 + i - 2; ix[ni++] = b0 + i - 1; }
				ix[ni++] = b0 + i;
			}
			break;
		case 5:
			if (count < 3) break;
			nv = (size_t)count;
			memcpy(o, tv, sizeof(GVert) * nv);
			for (uint32_t i = 2; i < (uint32_t)count; i++) {
				if (any_cull && (cull[0] | cull[i - 1] | cull[i])) continue;
				ix[ni++] = b0; ix[ni++] = b0 + i - 1; ix[ni++] = b0 + i;
			}
			break;
		case 6:
			for (int i = 0; i + 1 < count; i += 2) {
				if (any_cull && (cull[i] | cull[i + 1])) continue;
				GVert tl = tv[i], br = tv[i + 1];  // the second vertex carries color/z
				float bx[4] = {-1e9f, -1e9f, 1e9f, 1e9f};  // no rectangle
				if (tex_on && !tex_from_fbo && tw > 0 && th > 0) {
					// half a texel of the texture as sampled (an atlas redrawn at the render scale is finer)
					float hu = 0.5f / ((float)tw * tex_scale), hv = 0.5f / ((float)th * tex_scale);
					float u0 = fminf(tl.u, br.u), u1 = fmaxf(tl.u, br.u), v0 = fminf(tl.v, br.v), v1 = fmaxf(tl.v, br.v);
					bx[0] = u0 + hu, bx[1] = v0 + hv, bx[2] = u1 - hu, bx[3] = v1 - hv;
					if (bx[0] > bx[2]) bx[0] = bx[2] = (u0 + u1) / 2.f;
					if (bx[1] > bx[3]) bx[1] = bx[3] = (v0 + v1) / 2.f;
				}
				SVert *q = all_s + all_ns + nv;  // corners 00 10 01 11
				for (int k = 0; k < 4; k++) q[k].v = br, memcpy(q[k].box, bx, sizeof bx);
				q[0].v.x = tl.x, q[0].v.y = tl.y, q[0].v.u = tl.u, q[0].v.v = tl.v;
				q[1].v.y = tl.y, q[1].v.v = tl.v;
				q[2].v.x = tl.x, q[2].v.u = tl.u;
				uint32_t k = (uint32_t)(all_ns + nv);
				ix[ni++] = k, ix[ni++] = k + 1, ix[ni++] = k + 2;
				ix[ni++] = k + 1, ix[ni++] = k + 3, ix[ni++] = k + 2;
				nv += 4;
			}
			break;
		default: break;
		}
		if (type == 6) all_ns += nv;
		else all_n += nv;
		all_ni += ni;
		if (st.mode != GL_TRIANGLES) batch_flush();
	}
	if (gedebug && dbg_prims++ < 400) {
		rt_log("GE prim tex@%08x%s type=%d n=%d fb=%05x(%dx%d) through=%d clear=%d(%x) tex=%d fmt=%d %dx%d vt=%06x "
		       "v0=(%.2f %.2f %.2f %.2f) c=%02x%02x%02x%02x uv=(%.3f %.3f) scissor=%x/%x zt=%d/%d blend=%d at=%d/%x",
		       ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), uvsx != 1.f ? "(FBO)" : "",
		       type, count, cur_fbo->addr, cur_fbo->width, cur_fbo->height, f.through, clear, reg[0xD3], tex_on,
		       (int)(reg[0xC3] & 0xF), tw, th, reg[0x12], tv[0].x, tv[0].y, tv[0].z, tv[0].w, tv[0].c0[0], tv[0].c0[1],
		       tv[0].c0[2], tv[0].c0[3], tv[0].u, tv[0].v, reg[0xD4], reg[0xD5], reg[0x23] & 1, reg[0xDE], reg[0x21] & 1,
		       reg[0x22] & 1, reg[0xDB]);
	}
	if (dumping && getenv("DBZ_GEDUMP_STATE")) {
		fprintf(ge_dump_file, "DRAW flt=%06x/%06x fbfmt=%d fb=%05x(%dx%d) type=%d n=%d vt=%06x tex=%d@%08x f%d %dx%d clut=%x | blend=%d df=%03x fa=%06x fb=%06x tfunc=%05x "
		       "at=%d/%06x zt=%d/%d zmask=%d mask=%06x/%02x light=%d fog=%d clear=%x cull=%d/%d | c0=%02x%02x%02x%02x "
		       "uv=(%.2f,%.2f) w=%.2f ct=%d/%d/%06x/%06x st=%d/%06x/%06x\n",
		       reg[0xC6], reg[0xC7], (int)(reg[0xD2] & 3), cur_fbo->addr, cur_fbo->width, cur_fbo->height, type, count, reg[0x12], tex_on,
		       ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), (int)(reg[0xC3] & 0xF), tw, th, reg[0xC5], reg[0x21] & 1, reg[0xDF], reg[0xE0], reg[0xE1], reg[0xC9], reg[0x22] & 1, reg[0xDB],
		       reg[0x23] & 1, reg[0xDE] & 7, reg[0xE7] & 1, reg[0xE8], reg[0xE9], reg[0x17] & 1, reg[0x1F] & 1, reg[0xD3],
		       reg[0x1D] & 1, reg[0x9B] & 1, tv[0].c0[0], tv[0].c0[1], tv[0].c0[2], tv[0].c0[3], tv[0].u, tv[0].v, tv[0].w,
		       reg[0x27] & 1, reg[0xD8] & 3, reg[0xD9] & 0xFFFFFF, reg[0xDA] & 0xFFFFFF, reg[0x24] & 1, reg[0xDC] & 0xFFFFFF,
		       reg[0xDD] & 0xFFFFFF);
	}
	// The GE advances the vertex/index pointer past what it consumed.
	if (isz) index_addr += (uint32_t)(count * isz);
	else vertex_addr += (uint32_t)(count * f.size);
}

// ---------------------------------------------------------------------------------------------
// Block transfer
// ---------------------------------------------------------------------------------------------
// Framebuffer that contains a VRAM address (32-bit framebuffers only), with the pixel position.
static Fbo *fbo_containing(uint32_t addr, int *px, int *py) {
	if ((addr & 0x0FE00000u) != PSP_VRAM_BASE) return NULL;
	uint32_t off = addr & 0x001FFFFFu;
	for (int i = 0; i < MAX_FBOS; i++) {
		Fbo *f = &fbos[i];
		uint32_t size = (uint32_t)(f->width * f->height * 4);
		if (f->used && f->last_draw && off >= f->addr && off < f->addr + size) {
			uint32_t pix = (off - f->addr) / 4;
			*px = (int)(pix % (uint32_t)f->width);
			*py = (int)(pix / (uint32_t)f->width);
			return f;
		}
	}
	return NULL;
}

// A framebuffer region copied to VRAM at dst (its pixels already there): kept at full resolution
// in a framebuffer of its own at that address, used while VRAM still holds those pixels.
static void copy_target(Fbo *sf, int x0, int y0, uint32_t dst, int dstride, int w, int h) {
	Fbo *t = fbo_get(dst & 0x001FFFFFu, dstride, h);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sf->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
	glBlitFramebuffer(x0 * rs, y0 * rs, (x0 + w) * rs, (y0 + h) * rs, 0, 0, w * rs, h * rs, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	t->copy = true;
	t->copy_hash = copy_hash(t);
	t->copy_checked = frame_no;
	t->last_draw = ++draw_counter;
	gl_invalidate();
}

static void block_transfer(uint32_t data) {
	int bpp = (data & 1) ? 4 : 2;
	uint32_t src = ((reg[0xB3] & 0xFF0000u) << 8) | (reg[0xB2] & 0xFFFFFFu);
	uint32_t dst = ((reg[0xB5] & 0xFF0000u) << 8) | (reg[0xB4] & 0xFFFFFFu);
	int sstride = (int)(reg[0xB3] & 0x7FF), dstride = (int)(reg[0xB5] & 0x7FF);
	int sx = (int)(reg[0xEB] & 0x3FF), sy = (int)((reg[0xEB] >> 10) & 0x3FF);
	int dx = (int)(reg[0xEC] & 0x3FF), dy = (int)((reg[0xEC] >> 10) & 0x3FF);
	int w = (int)(reg[0xEE] & 0x3FF) + 1, h = (int)((reg[0xEE] >> 10) & 0x3FF) + 1;
	if (gedebug) rt_log("GE transfer %08x(+%d,%d stride %d) -> %08x(+%d,%d stride %d) %dx%d bpp=%d", src, sx, sy, sstride, dst, dx, dy, dstride, w, h, bpp);

	// The live image of a framebuffer is on the GPU, not in VRAM.
	int spx = 0, spy = 0, dpx = 0, dpy = 0;
	Fbo *sf = bpp == 4 ? fbo_containing(src, &spx, &spy) : NULL;
	Fbo *df = bpp == 4 ? fbo_containing(dst, &dpx, &dpy) : NULL;
	if (!df && bpp == 4) {  // the destination may be a framebuffer that was never drawn yet
		Fbo *f = fbo_find(dst & 0x001FFFFFu);
		if (f) df = f, dpx = dpy = 0;
	}
	if (gedebug && (sf || df))
		rt_log("GE transfer: src fbo %08x %dx%d (+%d,%d), dst fbo %08x %dx%d (+%d,%d)", sf ? sf->addr : 0, sf ? sf->width : 0,
		       sf ? sf->height : 0, spx, spy, df ? df->addr : 0, df ? df->width : 0, df ? df->height : 0, dpx, dpy);
	if (sf || df) batch_flush();  // queued draws must land before the GPU image is read or changed
	if (sf && sstride == sf->width) {
		int x0 = spx + sx, y0 = spy + sy;
		if (df && dstride == df->width) {  // GPU -> GPU (coordinates in scaled pixels)
			int ox = dpx + dx, oy = dpy + dy;
			glDisable(GL_SCISSOR_TEST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, sf->fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df->fbo);
			if (sf == df) {  // overlapping copy inside one framebuffer: go through a temp texture
				GLuint tmp;
				glGenTextures(1, &tmp);
				glBindTexture(GL_TEXTURE_2D, tmp);
				glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, x0 * rs, y0 * rs, w * rs, h * rs, 0);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
				glBlitFramebuffer(0, 0, w * rs, h * rs, ox * rs, oy * rs, (ox + w) * rs, (oy + h) * rs, GL_COLOR_BUFFER_BIT,
				                  GL_NEAREST);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sf->tex, 0);
				glDeleteTextures(1, &tmp);
			}
			else {
				glBlitFramebuffer(x0 * rs, y0 * rs, (x0 + w) * rs, (y0 + h) * rs, ox * rs, oy * rs, (ox + w) * rs, (oy + h) * rs,
				                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			df->last_draw = ++draw_counter;
			// Keep VRAM in step (the picture may later be read as a texture from memory, e.g.
			// after the target is evicted) and, when the copy lands inside a larger framebuffer
			// rather than at its start, keep it at full resolution at its own address too.
			uint32_t d0 = dst + (uint32_t)((dy * dstride + dx) * 4);
			if (rt_valid(d0, (uint32_t)(((h - 1) * dstride + w) * 4))) fb_read(df->fbo, ox, oy, w, h, MP(d0), dstride);
			if (df->copy) df->copy_hash = copy_hash(df);
			if ((dst & 0x001FFFFFu) != df->addr && dx == 0 && dy == 0 && (dst & 0x0FE00000u) == PSP_VRAM_BASE &&
			    dstride >= w && dstride <= 1024 && h <= 512)
				copy_target(sf, x0, y0, dst, dstride, w, h);
			gl_invalidate();
			cur_fbo = NULL;
			return;
		}
		// GPU -> memory (e.g. part of a frame kept to be drawn again as a texture): read back the
		// pixels for the CPU, and keep the picture at full resolution in a copy target there.
		uint32_t d0 = dst + (uint32_t)((dy * dstride + dx) * 4);
		if (rt_valid(d0, (uint32_t)(((h - 1) * dstride + w) * 4))) fb_read(sf->fbo, x0, y0, w, h, MP(d0), dstride);
		if ((dst & 0x0FE00000u) == PSP_VRAM_BASE && dx == 0 && dy == 0 && dstride >= w && dstride <= 1024 && h <= 512)
			copy_target(sf, x0, y0, dst, dstride, w, h);
		cur_fbo = NULL;
		return;
	}
	for (int y = 0; y < h; y++) {
		uint32_t s = src + (uint32_t)(((sy + y) * sstride + sx) * bpp);
		uint32_t d = dst + (uint32_t)(((dy + y) * dstride + dx) * bpp);
		if (rt_valid(s, (uint32_t)(w * bpp)) && rt_valid(d, (uint32_t)(w * bpp))) memmove(MP(d), MP(s), (size_t)(w * bpp));
	}
	// Memory -> framebuffer: upload the copied pixels into the GPU image too.
	if (df && dstride == df->width) {
		fb_upload(df, dpx + dx, dpy + dy, w, h, MP(dst + (uint32_t)((dy * dstride + dx) * 4)), dstride);
		df->last_draw = ++draw_counter;
		if (df->copy) df->copy_hash = copy_hash(df);
	}
}

// ---------------------------------------------------------------------------------------------
// Command entry point (called by the list walker in display.c)
// ---------------------------------------------------------------------------------------------
void ge_command(uint32_t cmd, uint32_t data, uint32_t addr) {
	(void)addr;
	switch (cmd) {
	case 0x04: prim_cmd_addr = addr; draw_prim(data); return;
	case 0x01: vertex_addr = rel_addr(data); break;
	case 0x02: index_addr = rel_addr(data); break;
	case 0x3A: world_i = (int)(data & 0xF); break;
	case 0x3B:
		if (ge_dump_state == 1 && ge_dump_mtx_lines < 400) {
			ge_dump_mtx_lines++;
			fprintf(ge_dump_file, "W %08x [%d] %06x\n", addr, world_i, data);
		}
		if (world_i < 12) {
			float v = f24(data);
			if (world[world_i] != v) world[world_i] = v, xf_gen++;
			world_i++;
		}
		break;
	case 0x3C: view_i = (int)(data & 0xF); break;
	case 0x3D:
		if (view_i < 12) {
			float v = f24(data);
			if (view[view_i] != v) view[view_i] = v, xf_gen++, vp_gen++;
			view_i++;
		}
		break;
	case 0x3E: proj_i = (int)(data & 0xF); break;
	case 0x3F:
		if (proj_i < 16) {
			float v = f24(data);
			if (proj[proj_i] != v) proj[proj_i] = v, xf_gen++, vp_gen++;
			proj_i++;
		}
		break;
	case 0x40: tgen_i = (int)(data & 0xF); break;
	case 0x41: if (tgen_i < 12) tgen[tgen_i++] = f24(data); break;
	case 0x2A: bone_i = (int)(data & 0x7F); break;
	case 0x2B:
		if (bone_i < 96) bone[bone_i++] = f24(data);
		break;
	case 0xC4: {  // load CLUT
		uint32_t a = ((reg[0xB1] & 0xF0000u) << 8) | (reg[0xB0] & 0xFFFFFFu);
		uint32_t bytes = (data & 0x3F) * 32;
		if (bytes > sizeof clut) bytes = sizeof clut;
		// a game loads the same palette again draw after draw (thousands of times a frame): only one
		// that changes what is there is hashed again
		if (rt_valid(a, bytes) && memcmp(clut, MP(a), bytes)) memcpy(clut, MP(a), bytes), clut_dirty = true;
		break;
	}
	case 0xEA: block_transfer(data); break;
	default:
		// what xf_setup reads: lighting (0x17-0x1B, 0x50-0x9A), fog (0x1F, 0xCD, 0xCE), texture
		// mapping (0x48-0x4B, 0xC0); and the viewport (0x42-0x47, 0x4C, 0x4D)
		if (reg[cmd] != data && ((cmd >= 0x17 && cmd <= 0x1B) || cmd == 0x1F || (cmd >= 0x42 && cmd <= 0x9A) || cmd == 0xC0 ||
		                         cmd == 0xCD || cmd == 0xCE)) {
			xf_gen++;
			if ((cmd >= 0x42 && cmd <= 0x47) || cmd == 0x4C || cmd == 0x4D) vp_gen++;
		}
		break;
	}
	reg[cmd] = data;
}

// ---------------------------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------------------------
// Every texture is looked up and drawn again (the texture pack or the text drawing changed).
void ge_forget_textures(void) {
	for (int i = 0; i < MAX_TEX; i++) {
		texcache[i].hash = 0, texcache[i].checked = 0, texcache[i].pack1 = 0;
		free(texcache[i].ui_orig);
		texcache[i].ui_orig = NULL;
		free(texcache[i].orig);
		texcache[i].orig = NULL;
		free(texcache[i].look);
		texcache[i].look = NULL;
		free(texcache[i].up_px);
		texcache[i].up_px = NULL;
		if (texcache[i].up_tex) glDeleteTextures(1, &texcache[i].up_tex);
		texcache[i].up_tex = 0;
	}
	memset(memo, 0, sizeof memo);  // the pack or the switches changed
}

// Returns the texture to show for the displayed framebuffer address (an FBO if the GE rendered
// into it, otherwise the raw VRAM contents uploaded to a scratch texture).
GLuint ge_display_texture(uint32_t fb_addr, int stride, int fmt, int *src_w, int *src_h) {
	frame_no++;
	batch_flush();
	if (texpack_changed()) ge_forget_textures();  // the texture pack changed
	if (fonthd_changed()) ge_forget_textures();   // an HD font arrived: the lines are drawn again
	Fbo *f = fbo_find(fb_addr & 0x001FFFFFu);
	static unsigned long dbg_every = 0;
	if (!dbg_every) dbg_every = getenv("DBZ_GEDEBUG_EVERY") ? strtoul(getenv("DBZ_GEDEBUG_EVERY"), NULL, 10) : 120;
	if (gedebug > 0 && frame_no % dbg_every == 0) {
		rt_log("GE display %08x fbo=%s last_draw=%llu; fbos:", fb_addr, f ? "yes" : "no", f ? (unsigned long long)f->last_draw : 0ull);
		for (int i = 0; i < MAX_FBOS; i++)
			if (fbos[i].used) rt_log("   fbo %05x %dx%d last_draw=%llu", fbos[i].addr, fbos[i].width, fbos[i].height, (unsigned long long)fbos[i].last_draw);
		dbg_prims = 0;
	}
	if (f && f->last_draw) {  // the visible 480x272 corner of the (scaled) framebuffer
		*src_w = 480 * rs;
		*src_h = 272 * rs;
		return f->tex;
	}
	static GLuint scratch;
	static uint32_t px[512 * 272];
	if (!scratch) glGenTextures(1, &scratch);
	int bpp = fmt == 3 ? 4 : 2;
	if (stride <= 0) stride = 512;
	bool valid = fb_addr && rt_valid(fb_addr, (uint32_t)(stride * 272 * bpp));
	for (int y = 0; y < 272; y++) {
		for (int x = 0; x < 480; x++) {
			uint32_t a = fb_addr + (uint32_t)((y * stride + x) * bpp);
			if (!valid) px[y * 512 + x] = 0xFF000000u;
			else if (fmt == 3) px[y * 512 + x] = M32(a) | 0xFF000000u;
			else px[y * 512 + x] = conv16(M16(a), fmt) | 0xFF000000u;
		}
	}
	glBindTexture(GL_TEXTURE_2D, scratch);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl_invalidate();
	*src_w = 480;
	*src_h = 272;
	return scratch;
}

// The presentation code changed GL bindings and state.
void ge_unbind_target(void) {
	cur_fbo = NULL;
	gl_invalidate();
}

// Draw calls the game issued and GL draws they were merged into, since the last call.
uint64_t ge_draws_total(void) { return draws_total; }

void ge_draw_stats(uint64_t *draws, uint64_t *batches) {
	*draws = stat_draws;
	*batches = stat_batches;
	stat_draws = stat_batches = 0;
}

int ge_render_scale(void) { return rs; }
