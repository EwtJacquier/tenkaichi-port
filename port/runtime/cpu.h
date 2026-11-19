// CPU state and memory access for recompiled PSP (Allegrex) code.
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct CPU {
	uint32_t r[32];  // GPRs; r[0] is never written by generated code
	uint32_t hi, lo;
	union {
		float f[32];
		uint32_t u[32];
	} fpr;
	uint32_t fcr31;  // bit 23: FPU condition, bits 0-1: rounding mode
	// VFPU state lives in the runtime (rt_vfpu.c); generated code only reads the CC.
	float v[128];
	uint32_t vfpu_ctrl[16];  // prefixes s/t/d, CC (index 3), etc.
	uint32_t thread_id;      // runtime bookkeeping
} CPU;

#define VFPU_CTRL_CC 3

// PSP physical address space mirror. All user addresses (RAM 0x08xxxxxx, VRAM 0x04xxxxxx,
// scratchpad 0x0001xxxx and their uncached 0x4xxxxxxx / kernel 0x8xxxxxxx mirrors) fold into
// 28 bits, so the guest memory is a single 256 MB block.
extern uint8_t *rt_mem;
#define MEM_MASK 0x0FFFFFFFu
#define MP(a) (rt_mem + ((uint32_t)(a) & MEM_MASK))
// VFPU loads/stores emitted inline by tools/recomp.py (register k of v[], the word at guest address a).
#define VLD(k, a) do { uint32_t t_ = M32(a); memcpy(&c->v[k], &t_, 4); } while (0)
// No VFPU prefix is set (S and T the identity swizzle, D neither saturating nor masking): the inline
// arithmetic forms apply, else the runtime does the instruction with its prefixes.
#define VPFX_IDENT (c->vfpu_ctrl[0] == 0xE4u && c->vfpu_ctrl[1] == 0xE4u && c->vfpu_ctrl[2] == 0u)
// An inline VFPU form (body) or, with prefixes set, the runtime. Built with -DVFPU_SELFCHECK, both
// run and any difference in the registers is reported (rt_vfpu_mismatch).
void rt_vfpu_mismatch(uint32_t pc, uint32_t op, const float *inline_v, const float *runtime_v);
float rt_vdot4(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3);  // vfpu.c: vmmul's exact dot
#ifdef VFPU_SELFCHECK
#define VFAST(pc, op, body) do { if (VPFX_IDENT) { float v0_[128], v1_[128]; memcpy(v0_, c->v, sizeof v0_); body \
	memcpy(v1_, c->v, sizeof v1_); memcpy(c->v, v0_, sizeof v0_); rt_vfpu(c, pc, op); \
	if (memcmp(v1_, c->v, sizeof v1_)) rt_vfpu_mismatch(pc, op, v1_, c->v); } else rt_vfpu(c, pc, op); } while (0)
#else
#define VFAST(pc, op, body) do { if (VPFX_IDENT) body else rt_vfpu(c, pc, op); } while (0)
#endif
#define VST(k, a) do { uint32_t t_; memcpy(&t_, &c->v[k], 4); M32(a) = t_; } while (0)
#define M8(a) (*(uint8_t *)MP(a))
#define M16(a) (*(uint16_t *)MP(a))
#define M32(a) (*(uint32_t *)MP(a))
#define S8(a) ((uint32_t)(int32_t)(*(int8_t *)MP(a)))
#define S16(a) ((uint32_t)(int32_t)(*(int16_t *)MP(a)))

typedef void (*RecompFunc)(CPU *c);

// Provided by the runtime.
void rt_call(CPU *c, uint32_t addr);                           // indirect call (jalr)
void rt_tail_call(CPU *c, uint32_t pc, uint32_t addr);         // jr to a register outside known tables
_Noreturn void rt_bad_jump(CPU *c, uint32_t pc, uint32_t addr);
_Noreturn void rt_fallthrough(CPU *c, uint32_t pc);
void rt_break(CPU *c, uint32_t pc, uint32_t code);
void rt_vfpu(CPU *c, uint32_t pc, uint32_t op);                // executes one VFPU instruction
uint32_t rt_f2i(float f, int mode);                            // 0 nearest,1 trunc,2 ceil,3 floor
void rt_lwl(CPU *c, int rt, uint32_t addr);
void rt_lwr(CPU *c, int rt, uint32_t addr);
void rt_swl(CPU *c, int rt, uint32_t addr);
void rt_swr(CPU *c, int rt, uint32_t addr);
void rt_div(CPU *c, uint32_t a, uint32_t b);
void rt_divu(CPU *c, uint32_t a, uint32_t b);
