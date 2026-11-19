// Runtime shared declarations.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"

// ---- logging ---------------------------------------------------------------
extern int rt_trace;  // DBZ_TRACE=1 logs every HLE call
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- memory ----------------------------------------------------------------
#define PSP_RAM_BASE 0x08000000u
#define PSP_RAM_END 0x0A000000u
#define PSP_VRAM_BASE 0x04000000u
#define PSP_VRAM_SIZE 0x00200000u
#define PSP_SCRATCH_BASE 0x00010000u
#define PSP_SCRATCH_SIZE 0x00004000u
#define USER_PARTITION_START 0x08800000u

static inline bool rt_valid(uint32_t a, uint32_t len) {
	a &= MEM_MASK;
	return (a >= PSP_RAM_BASE && a + len <= PSP_RAM_END) ||
	       (a >= PSP_VRAM_BASE && a + len <= PSP_VRAM_BASE + PSP_VRAM_SIZE) ||
	       (a >= PSP_SCRATCH_BASE && a + len <= PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE);
}
static inline void *rt_ptr(uint32_t a) { return a ? (void *)MP(a) : NULL; }
static inline const char *rt_str(uint32_t a) { return a ? (const char *)MP(a) : "(null)"; }
void rt_write_cstr(uint32_t dst, uint32_t cap, const char *s);

// ---- module / program ------------------------------------------------------
extern uint32_t rt_module_entry, rt_module_end;
RecompFunc rt_lookup(uint32_t addr);

// ---- HLE helpers -----------------------------------------------------------
#define A0 (c->r[4])
#define A1 (c->r[5])
#define A2 (c->r[6])
#define A3 (c->r[7])
#define A4 (c->r[8])
#define A5 (c->r[9])
#define A6 (c->r[10])
#define RET(v) (c->r[2] = (uint32_t)(v))
#define RET64(v) (c->r[2] = (uint32_t)(uint64_t)(v), c->r[3] = (uint32_t)((uint64_t)(v) >> 32))
#define TRACE(...) \
	do { \
		if (rt_trace) rt_log(__VA_ARGS__); \
	} while (0)

// ---- time ------------------------------------------------------------------
uint64_t rt_now_us(void);  // microseconds since boot
#define VBLANK_US 16683u    // 59.94 Hz
extern bool rt_clock_vblanks;          // sceKernelGetSystemTimeWide answers rt_vblank_time_us(rt_clock_vblank)
extern uint64_t rt_clock_vblank;
uint64_t rt_vblank_time_us(uint64_t n);  // vblank n's time: VBLANK_US apart (from the real time at the first call)
extern uint64_t rt_clock_at_us;  // nonzero: what the system clock says (a GE callback delivered late)
uint64_t rt_shown_vblank(void);          // the vblank at which the last frame was set (sceDisplaySetFrameBuf)

// ---- kernel (kernel.c) -----------------------------------------------------
typedef struct Thread Thread;
Thread *rt_current_thread(void);
const char *rt_thread_name(Thread *t);
void kernel_init(void);
void kernel_create_main_thread(uint32_t entry, uint32_t prio, uint32_t stack_size, const char *argv0);
void kernel_run(void);  // scheduler loop, never returns
// Blocking helpers for other HLE modules.
void rt_wait_until(uint64_t wake_us);            // block current thread until time
void rt_call_on_main(void (*fn)(void *), void *arg);  // run on the scheduler stack (web: async JS waits)
void rt_wait_vblank(void);                       // block until next vblank start
uint64_t rt_vblank_count(void);
struct timeval;
void rt_wallclock(struct timeval *tv);  // the date the game sees (DBZ_FAKE_TIME pins it, for tests)
void rt_texture_decoded(uint32_t *px, int w, int h);  // mods may define it: a texture's RGBA as decoded (runtime/ge.c)
void rt_tap_buttons(uint32_t mask);  // mods: press PSP buttons (sceCtrl masks) for a moment
void rt_turbo(bool on);  // mods: run the game as fast as the CPU goes, nothing shown (skipping a stretch)
bool rt_turbo_on(void);
void rt_warp(uint64_t us);
uint64_t rt_real_us(void);
void rt_psmf_skip_next(void);        // mods: the next movie set ends as soon as it starts
uint32_t rt_overlay(void);            // the code overlay called last (its number in PACKFILE.BIN; 0 before any)
extern uint32_t rt_scene;             // mods: what the game is showing (ge.c DBZ_TEXCATALOG names textures by it)
void rt_psmf_hide_next(void);        // mods: the next movie set plays black and silent
bool rt_psmf_hidden_playing(void);   // ... and is playing now
bool rt_psmf_frame_at(uint32_t addr);  // a movie wrote its frames there lately
// The HD picture of the movie frame at addr (w x h RGBA standing for its iw x ih texels; psmf.c,
// from the texture pack's movies/<name>.h264), or px NULL: none any more (ge.c draws it).
void ge_movie_hd(uint32_t addr, const uint32_t *px, int w, int h, int iw, int ih);
void rt_yield(void);
void rt_resched(void);                           // let a higher-priority ready thread run
uint32_t rt_alloc_user(uint32_t size, bool high, const char *what);  // 256-byte aligned
void rt_free_user(uint32_t addr);
uint32_t rt_user_free_total(void);
void rt_call_guest(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2);  // run guest code on current thread
uint32_t rt_call_guest_args(uint32_t func, const uint32_t *args, int n);  // up to 8 arguments; v0
uint32_t rt_native_thread(const char *name, void (*fn)(void *), void *arg, uint32_t prio, uint32_t stack_size);

// ---- ad hoc (net.c): the mods' own messages between the copies in the game's room ----------
void net_mod_send(int kind, const void *data, int len);           // to the others, in order
int net_mod_recv(int kind, uint8_t from[6], void *buf, int cap);  // the oldest of that kind: length, 0 none
void net_recv_round(bool begin);  // the game reads its datagrams (one per sender in a round), then handles them

// ---- platform (display.c) --------------------------------------------------
void platform_init(void);
void platform_vblank(void);      // called by the scheduler once per vblank: present + input
void platform_idle(uint64_t us); // sleep while nothing is runnable
uint32_t platform_buttons(void);
void platform_analog(uint8_t *x, uint8_t *y);

// ---- io (io.c) -------------------------------------------------------------
void io_init(const char *iso_path, const char *ms0_dir);
bool io_find_source(uint32_t addr, uint64_t *iso_off);  // where a recently read buffer came from
void io_iso_read(uint64_t off, void *dst, size_t n);
bool io_disc_lookup(const char *path, uint64_t *off, uint32_t *size);

// ---- audio (audio.c) ---------------------------------------------------------
void audio_init(void);
void audio_pump(void);  // run the audio DMA up to now and hand the mix to the host
uint64_t audio_output_latency_us(void);  // from the DMA playing a frame to it being heard
void audio_stats(uint64_t *underrun, uint64_t *mixed, uint32_t *min_margin);
