// Memory, program loading, CPU helpers and entry point.
#define _GNU_SOURCE
#include "meta.h"  // generated/meta.h (tools/prx_load.py): where the program loads and starts
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <execinfo.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "rt.h"

uint8_t *rt_mem;
int rt_trace;
uint32_t rt_module_entry, rt_module_end;

// ---- logging ---------------------------------------------------------------

void rt_log(const char *fmt, ...) {
	Thread *t = rt_current_thread();
	fprintf(stderr, "[%8.3f %-12.12s] ", rt_now_us() / 1e6, t ? rt_thread_name(t) : "-");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

_Noreturn void rt_fatal(const char *fmt, ...) {
	fprintf(stderr, "FATAL: ");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	abort();  // keeps the native call stack for gdb
}

// ---- time ------------------------------------------------------------------

static struct timespec boot_ts;

// Turbo (rt_turbo, for mods: a stretch of the game run as fast as the CPU goes, unseen): while it is
// on, waiting moves the clock on instead of passing real time (rt_warp); the clock the game sees is
// real time plus everything warped so far.
static uint64_t warped_us;
static bool turbo;
void rt_turbo(bool on) { turbo = on; }
bool rt_turbo_on(void) { return turbo; }
void rt_warp(uint64_t us) { warped_us += us; }
uint64_t rt_real_us(void);  // the clock without the warp


uint64_t rt_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec - boot_ts.tv_sec) * 1000000ull + (uint64_t)((ts.tv_nsec - boot_ts.tv_nsec) / 1000) + warped_us;
}

// ---- memory ----------------------------------------------------------------

static __attribute__((unused)) void map_region(uint32_t addr, uint32_t size) {
	if (mprotect(rt_mem + addr, size, PROT_READ | PROT_WRITE) != 0) {
		rt_fatal("mprotect %08x+%x: %s", addr, size, strerror(errno));
	}
}

static void mem_init(void) {
#ifdef __EMSCRIPTEN__
	// One zeroed 256 MB block (browsers commit pages lazily).
	rt_mem = calloc(1, (size_t)MEM_MASK + 1);
	if (!rt_mem) rt_fatal("cannot allocate guest memory");
	return;
#endif
	// Reserve the whole folded address space; only real PSP regions are accessible, so a wild
	// pointer in game code faults right away instead of corrupting memory.
	rt_mem = mmap(NULL, (size_t)MEM_MASK + 1, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (rt_mem == MAP_FAILED) {
		rt_fatal("cannot reserve guest address space");
	}
	map_region(PSP_SCRATCH_BASE, PSP_SCRATCH_SIZE);
	map_region(PSP_VRAM_BASE, PSP_VRAM_SIZE);
	map_region(PSP_RAM_BASE, PSP_RAM_END - PSP_RAM_BASE);
}

void rt_write_cstr(uint32_t dst, uint32_t cap, const char *s) {
	if (!dst || !cap) {
		return;
	}
	size_t n = strlen(s);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(MP(dst), s, n);
	M8(dst + n) = 0;
}

// ---- program image ---------------------------------------------------------

#define IMAGE_BASE 0x08804000u

static void load_image(const char *path, uint32_t bss_start, uint32_t bss_end) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		rt_fatal("cannot open %s", path);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fread(MP(IMAGE_BASE), 1, (size_t)n, f) != (size_t)n) {
		rt_fatal("short read on %s", path);
	}
	fclose(f);
	memset(MP(bss_start), 0, bss_end - bss_start);
	rt_module_end = bss_end;
	rt_log("loaded %ld bytes at %08x, bss %08x-%08x", n, IMAGE_BASE, bss_start, bss_end);
}

// ---- indirect calls --------------------------------------------------------

typedef struct {
	uint32_t addr;
	RecompFunc fn;
} FuncEntry;
extern const size_t rt_func_count;
extern const FuncEntry rt_func_table[];
#include "overlay.h"
RecompFunc rt_lookup_main(uint32_t addr);

// Every code word's function (NULL where none starts), made once from the sorted table: an
// indirect call is one load instead of a binary search (~5% of the CPU on the web).
static RecompFunc *direct;
static uint32_t direct_lo, direct_n;

// The overlay loaded now: the one whose first bytes are at its base (its header's number and the
// code after it; the header's other words may be changed by the game, so a second look skips them).
static const Overlay *last;
uint32_t rt_overlay(void) { return last ? last->number : 0; }
static const Overlay *loaded_overlay(uint32_t addr) {
	for (int pass = 0; pass < 2; pass++) {
		uint32_t from = pass ? 16 : 0;
		if (last && addr - last->base < last->size && !memcmp(MP(last->base + from), last->sig + from, last->sig_len - from))
			return last;
		for (const Overlay *const *o = rt_overlays; *o; o++) {
			const Overlay *ov = *o;
			if (addr - ov->base < ov->size && !memcmp(MP(ov->base + from), ov->sig + from, ov->sig_len - from)) {
				if (ov != last && getenv("DBZ_OVERLAY_LOG")) rt_log("overlay %u is loaded (call to %08x)", ov->number, addr);
				return last = ov;
			}
		}
	}
	return NULL;
}

static RecompFunc overlay_lookup(uint32_t addr) {
	const Overlay *ov = loaded_overlay(addr);
	if (!ov) return NULL;
	size_t lo = 0, hi = ov->count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (ov->table[mid].addr < addr) lo = mid + 1;
		else hi = mid;
	}
	return lo < ov->count && ov->table[lo].addr == addr ? ov->table[lo].fn : NULL;
}

RecompFunc rt_lookup(uint32_t addr) {
	RecompFunc f = rt_lookup_main(addr);
	return f ? f : overlay_lookup(addr);
}

RecompFunc rt_lookup_main(uint32_t addr) {
	if (!direct && rt_func_count) {
		direct_lo = rt_func_table[0].addr;
		direct_n = (rt_func_table[rt_func_count - 1].addr - direct_lo) / 4 + 1;
		direct = calloc(direct_n, sizeof *direct);
		if (!direct) rt_fatal("no memory for the function table");
		for (size_t i = 0; i < rt_func_count; i++)
			if (!(rt_func_table[i].addr & 3)) direct[(rt_func_table[i].addr - direct_lo) / 4] = rt_func_table[i].fn;
	}
	uint32_t i = (addr - direct_lo) / 4;
	return !(addr & 3) && addr >= direct_lo && i < direct_n ? direct[i] : NULL;
}

void rt_call(CPU *c, uint32_t addr) {
	RecompFunc f = rt_lookup(addr);
	if (!f) {
		const Overlay *ov = loaded_overlay(addr);
		rt_fatal("indirect call to unknown address %08x (ra=%08x)%s", addr, c->r[31], ov ? " in the loaded overlay" : "");
	}
	f(c);
}

void rt_tail_call(CPU *c, uint32_t pc, uint32_t addr) {
	RecompFunc f = rt_lookup(addr);
	if (!f) {
		rt_fatal("computed jump at %08x to %08x: not a known target", pc, addr);
	}
	f(c);
}

_Noreturn void rt_bad_jump(CPU *c, uint32_t pc, uint32_t addr) {
	(void)c;
	rt_fatal("bad jump at %08x to %08x", pc, addr);
}

_Noreturn void rt_fallthrough(CPU *c, uint32_t pc) {
	rt_fatal("execution fell through to %08x (ra=%08x)", pc, c->r[31]);
}

void rt_break(CPU *c, uint32_t pc, uint32_t code) {
	rt_fatal("break %x at %08x (ra=%08x)", code, pc, c->r[31]);
}

// ---- arithmetic helpers ----------------------------------------------------

void rt_div(CPU *c, uint32_t a, uint32_t b) {
	int32_t sa = (int32_t)a, sb = (int32_t)b;
	if (sb == 0) {
		c->lo = sa < 0 ? 1 : 0xFFFFFFFFu;
		c->hi = a;
	}
	else if (sa == INT32_MIN && sb == -1) {
		c->lo = 0x80000000u;
		c->hi = 0;
	}
	else {
		c->lo = (uint32_t)(sa / sb);
		c->hi = (uint32_t)(sa % sb);
	}
}

void rt_divu(CPU *c, uint32_t a, uint32_t b) {
	if (b == 0) {
		c->lo = a <= 0xFFFF ? 0xFFFF : 0xFFFFFFFFu;
		c->hi = a;
	}
	else {
		c->lo = a / b;
		c->hi = a % b;
	}
}

uint32_t rt_f2i(float f, int mode) {
	if (isnan(f) || isinf(f)) {
		return 0x7FFFFFFFu;
	}
	double d;
	switch (mode) {
	case 0: d = nearbyint(f); break;  // default rounding mode is round-to-nearest-even
	case 1: d = trunc(f); break;
	case 2: d = ceil(f); break;
	default: d = floor(f); break;
	}
	if (d >= 2147483648.0 || d < -2147483648.0) {
		return 0x7FFFFFFFu;
	}
	return (uint32_t)(int32_t)d;
}

// Unaligned word access, little-endian MIPS semantics.
void rt_lwl(CPU *c, int rt, uint32_t addr) {
	uint32_t shift = (addr & 3) * 8, mem = M32(addr & ~3u);
	c->r[rt] = (c->r[rt] & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
}
void rt_lwr(CPU *c, int rt, uint32_t addr) {
	uint32_t shift = (addr & 3) * 8, mem = M32(addr & ~3u);
	c->r[rt] = (c->r[rt] & (0xFFFFFF00u << (24 - shift))) | (mem >> shift);
}
void rt_swl(CPU *c, int rt, uint32_t addr) {
	uint32_t shift = (addr & 3) * 8, mem = M32(addr & ~3u);
	M32(addr & ~3u) = (c->r[rt] >> (24 - shift)) | (mem & (0xFFFFFF00u << shift));
}
void rt_swr(CPU *c, int rt, uint32_t addr) {
	uint32_t shift = (addr & 3) * 8, mem = M32(addr & ~3u);
	M32(addr & ~3u) = (c->r[rt] << shift) | (mem & (0x00FFFFFFu >> (24 - shift)));
}

// DBZ_TRACE=2: every HLE call with its first arguments and result. DBZ_TRACE_FILTER=substr
// restricts it to matching names (e.g. "Psmf").
static const char *trace_filter;
void rt_hle_enter(CPU *c, const char *name) {
	if (trace_filter && !strstr(name, trace_filter)) return;
	rt_log("-> %s(%08x, %08x, %08x, %08x, %08x) ra=%08x", name, A0, A1, A2, A3, A4, c->r[31]);
}
void rt_hle_leave(CPU *c, const char *name) {
	if (trace_filter && !strstr(name, trace_filter)) return;
	rt_log("<- %s = %08x", name, c->r[2]);
}

void rt_hle_unimplemented(CPU *c, const char *name) {
	rt_log("UNIMPLEMENTED %s(%08x, %08x, %08x, %08x) ra=%08x", name, A0, A1, A2, A3, c->r[31]);
	RET(0);
}

// ---- watchdog ----------------------------------------------------------------
// If the scheduler loop does not run for 3 s, some guest thread is spinning without making a
// blocking call. Print which one and the native stack (recompiled functions are named f_XXXXXXXX).
volatile unsigned long rt_sched_heartbeat;
static unsigned long last_heartbeat;
static int stalled_ticks;

void kernel_dump_threads(void);
static int status_every;

#ifndef __EMSCRIPTEN__
static void watchdog(int sig) {
	(void)sig;
	static int ticks;
	if (status_every && ++ticks % status_every == 0) {
		char msg[128];
		int n = snprintf(msg, sizeof msg, "STATUS: vblank=%llu sched=%lu\n", (unsigned long long)rt_vblank_count(),
		                 rt_sched_heartbeat);
		if (write(2, msg, (size_t)n) < 0) return;
		kernel_dump_threads();
	}
	if (rt_sched_heartbeat != last_heartbeat) {
		last_heartbeat = rt_sched_heartbeat;
		stalled_ticks = 0;
		return;
	}
	if (++stalled_ticks != 3) {
		return;
	}
	Thread *t = rt_current_thread();
	char msg[160];
	int n = snprintf(msg, sizeof msg, "\nWATCHDOG: thread %s has not yielded for 3 s. Native stack:\n",
	                 t ? rt_thread_name(t) : "-");
	if (write(2, msg, (size_t)n) < 0) return;
	void *frames[48];
	int depth = backtrace(frames, 48);
	backtrace_symbols_fd(frames, depth, 2);
}

static void watchdog_init(void) {
	const char *st = getenv("DBZ_STATUS");
	status_every = st ? atoi(st) : 0;
	signal(SIGALRM, watchdog);
	sigset_t set;  // the parent process may have left SIGALRM blocked
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	struct itimerval it = {{1, 0}, {1, 0}};
	setitimer(ITIMER_REAL, &it, NULL);
}
#else
static void watchdog_init(void) {}
#endif

// ---- entry point -----------------------------------------------------------

int main(int argc, char **argv) {
	clock_gettime(CLOCK_MONOTONIC, &boot_ts);
	if (argc < 2) {
		fprintf(stderr, "usage: %s GAME.iso [image.bin]\n", argv[0]);
		return 1;
	}
	const char *iso = argv[1];
	const char *image = argc > 2 ? argv[2] : "generated/image.bin";
	const char *tr = getenv("DBZ_TRACE");
	rt_trace = tr ? atoi(tr) : 0;
	trace_filter = getenv("DBZ_TRACE_FILTER");

	mem_init();
	// Values from generated/meta.json (prx_load.py): image base, .bss range, module_start.
	load_image(image, META_IMAGE_END, META_BSS_END);
	rt_module_entry = META_ENTRY;

	const char *ms0 = getenv("DBZ_MS0");
	io_init(iso, ms0 ? ms0 : "build/ms0");
	platform_init();
	audio_init();
	kernel_init();
	watchdog_init();
	// module_start_thread_parameter: priority 0x20; the stack field (0x400) is in KB units on
	// some SDKs, so use the PSP default user_main stack of 256 KB, which the game expects.
	kernel_create_main_thread(rt_module_entry, 0x20, 0x40000, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
	kernel_run();
	return 0;
}

uint64_t rt_real_us(void) { return rt_now_us() - warped_us; }
