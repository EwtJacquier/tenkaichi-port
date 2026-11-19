// PSP kernel services used by the game: threads (as coroutines on one OS thread, see coro.h),
// scheduler, event flags, variable pools, callbacks, partition memory, modules, misc.
#define _GNU_SOURCE
#include "meta.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

#include "coro.h"

#include "ge.h"
#include "hle.h"
#include "platform.h"
#include "rt.h"

// ---- error codes -------------------------------------------------------------
#define ERR_ILLEGAL_ARG 0x800200D2u
#define ERR_NO_MEMORY 0x800200D9u
#define ERR_ILLEGAL_PRIORITY 0x8002012Au
#define ERR_UNKNOWN_UID 0x800200CBu
#define ERR_UNKNOWN_THID 0x80020198u
#define ERR_NOT_DORMANT 0x800201A4u
#define ERR_DORMANT 0x800201A2u
#define ERR_WAIT_TIMEOUT 0x800201A8u
#define ERR_WAIT_DELETE 0x800201B5u
#define ERR_EVF_COND 0x800201AFu
#define ERR_EVF_MULTI 0x800201B0u
#define ERR_EVF_ILPAT 0x800201B1u
#define ERR_UNKNOWN_EVFID 0x800201AEu
#define ERR_UNKNOWN_VPLID 0x800201C3u
#define ERR_ILLEGAL_MEMBLOCK 0x800200C0u
#define ERR_UNKNOWN_CBID 0x800201A1u
#define ERR_UNKNOWN_SEMID 0x80020199u
#define ERR_SEMA_ZERO 0x800201ADu
#define ERR_SEMA_OVF 0x800201AEu
#define ERR_ILLEGAL_COUNT 0x800201BDu

// ---- kernel object table -------------------------------------------------------
enum ObjType { OBJ_NONE, OBJ_THREAD, OBJ_EVF, OBJ_VPL, OBJ_CALLBACK, OBJ_BLOCK, OBJ_MODULE, OBJ_SEMA, OBJ_MUTEX, OBJ_FPL, OBJ_VTIMER, OBJ_MBX };
#define MAX_OBJS 2048
#define UID_BASE 0x00100000u
static struct {
	int type;
	void *p;
} objs[MAX_OBJS];

static uint32_t obj_new(int type, void *p) {
	for (int i = 1; i < MAX_OBJS; i++) {
		if (objs[i].type == OBJ_NONE) {
			objs[i].type = type;
			objs[i].p = p;
			return UID_BASE + (uint32_t)i;
		}
	}
	rt_fatal("kernel object table full");
}
static void *obj_get(uint32_t uid, int type) {
	uint32_t i = uid - UID_BASE;
	if (uid < UID_BASE || i >= MAX_OBJS || objs[i].type != type) {
		return NULL;
	}
	return objs[i].p;
}
static void obj_free(uint32_t uid) {
	uint32_t i = uid - UID_BASE;
	if (i < MAX_OBJS) {
		objs[i].type = OBJ_NONE;
		objs[i].p = NULL;
	}
}

// ---- partition memory ----------------------------------------------------------
typedef struct Block {
	uint32_t addr, size, uid;
	char name[32];
	struct Block *next;  // sorted by address
} Block;
static Block *blocks;
static uint32_t heap_lo, heap_hi;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// type: 0 low, 1 high, 2 at address, 3 low aligned, 4 high aligned (arg = alignment).
static Block *block_alloc(uint32_t size, int type, uint32_t arg, const char *name) {
	size = align_up(size ? size : 1, 0x100);
	uint32_t align = (type == 3 || type == 4) && arg > 0x100 ? arg : 0x100;
	uint32_t found = 0;
	bool ok = false;
	// Walk gaps between blocks.
	uint32_t prev_end = heap_lo;
	Block *b = blocks;
	for (;;) {
		uint32_t gap_end = b ? b->addr : heap_hi;
		uint32_t lo = align_up(prev_end, align);
		uint32_t hi = gap_end >= size ? ((gap_end - size) & ~(align - 1)) : 0;
		if (gap_end > prev_end && lo <= hi && hi >= prev_end) {
			if ((type == 0 || type == 3) && !ok) {  // low: first fit
				found = lo;
				ok = true;
			}
			else if (type == 1 || type == 4) {  // high: last fit wins
				found = hi;
				ok = true;
			}
			else if (type == 2 && arg >= prev_end && arg + size <= gap_end) {
				found = arg;
				ok = true;
			}
		}
		if (!b) {
			break;
		}
		prev_end = b->addr + b->size;
		b = b->next;
	}
	if (!ok) {
		return NULL;
	}
	Block *nb = calloc(1, sizeof(Block));
	nb->addr = found;
	nb->size = size;
	snprintf(nb->name, sizeof nb->name, "%s", name ? name : "");
	Block **pp = &blocks;
	while (*pp && (*pp)->addr < found) {
		pp = &(*pp)->next;
	}
	nb->next = *pp;
	*pp = nb;
	nb->uid = obj_new(OBJ_BLOCK, nb);
	return nb;
}

static void block_free(Block *blk) {
	for (Block **pp = &blocks; *pp; pp = &(*pp)->next) {
		if (*pp == blk) {
			*pp = blk->next;
			obj_free(blk->uid);
			free(blk);
			return;
		}
	}
}

uint32_t rt_alloc_user(uint32_t size, bool high, const char *what) {
	Block *b = block_alloc(size, high ? 1 : 0, 0, what);
	return b ? b->addr : 0;
}

void rt_free_user(uint32_t addr) {
	for (Block *b = blocks; b; b = b->next) {
		if (b->addr == addr) {
			block_free(b);
			return;
		}
	}
}

uint32_t rt_user_free_total(void) {
	uint32_t used = 0;
	for (Block *b = blocks; b; b = b->next) {
		used += b->size;
	}
	return heap_hi - heap_lo - used;
}

// ---- threads -------------------------------------------------------------------
enum { TS_RUNNING = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_DORMANT = 16, TS_DEAD = 32 };
enum { WT_NONE, WT_DELAY, WT_VBLANK, WT_EVF, WT_THREADEND, WT_VPL, WT_TIME, WT_SEMA, WT_SLEEP, WT_MUTEX, WT_FPL, WT_MBX };

struct Thread {
	uint32_t uid;
	void (*native)(void *);  // a thread of the runtime's own (C), not the game's code
	void *native_arg;
	char name[32];
	CPU cpu;
	Coro co;
	uint32_t entry, init_prio, prio, attr, stack_addr, stack_size;
	int status;
	int64_t ready_seq;
	int wait_type;
	void *wait_obj;
	uint64_t wake_us;  // 0 = no timeout
	uint32_t timeout_ptr;
	uint32_t wait_bits, wait_mode, wait_out;  // event flag: bits, mode, out ptr; vpl: size, -, out ptr
	int32_t wait_result;
	uint32_t exit_status;
	uint64_t vblank_target;
	uint32_t wakeups;  // sceKernelWakeupThread calls not yet slept off
};

#define MAX_THREADS 64
static Thread threads[MAX_THREADS];
static Thread *cur;

// Debug (GE capture): every live thread with its VFPU registers.
void rt_dump_threads(FILE *f) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid || t->status == TS_DEAD) continue;
		fprintf(f, "THREAD %s uid=%08x status=%d attr=%08x prio=%u pc~ra=%08x vctrl=%05x/%05x/%03x\nVREG %s", t->name, t->uid,
		        t->status, t->attr, t->prio, t->cpu.r[31], t->cpu.vfpu_ctrl[0], t->cpu.vfpu_ctrl[1], t->cpu.vfpu_ctrl[2], t->name);
		for (int k = 0; k < 128; k++) {
			uint32_t u;
			memcpy(&u, &t->cpu.v[k], 4);
			fprintf(f, " %08x", u);
		}
		fprintf(f, "\n");
	}
}
static Coro sched_co;
static int64_t seq_back, seq_front;
static uint64_t vblanks, next_vblank_us;
static uint64_t next_timer_us = UINT64_MAX;  // earliest pending wake-up of a waiting thread

Thread *rt_current_thread(void) { return cur; }
const char *rt_thread_name(Thread *t) { return t->name; }
uint64_t rt_vblank_count(void) { return vblanks; }

static Thread *thread_get(uint32_t uid) {
	if (uid == 0) {
		return cur;
	}
	return obj_get(uid, OBJ_THREAD);
}

static void make_ready(Thread *t, bool front) {
	t->status = (t->status & TS_SUSPEND) ? (TS_SUSPEND) : TS_READY;
	t->ready_seq = front ? --seq_front : ++seq_back;
}

static void wake(Thread *t, int32_t result) {
	t->wait_type = WT_NONE;
	t->wait_obj = NULL;
	t->wait_result = result;
	t->wake_us = 0;
	if (t->status & TS_SUSPEND) {
		t->status = TS_SUSPEND;
	}
	else {
		make_ready(t, false);
	}
}

static void switch_to_scheduler(void) {
	Thread *t = cur;
	coro_switch(&t->co, &sched_co);
}

// Block the current thread; returns the wait result.
static int32_t block_current(int wait_type, void *obj, uint64_t wake_us) {
	Thread *t = cur;
	t->status = TS_WAIT;
	t->wait_type = wait_type;
	t->wait_obj = obj;
	t->wake_us = wake_us;
	t->wait_result = 0;
	if (wake_us && wake_us < next_timer_us) {
		next_timer_us = wake_us;
	}
	switch_to_scheduler();
	return t->wait_result;
}

static uint64_t timeout_deadline(uint32_t timeout_ptr) {
	if (!timeout_ptr) {
		return 0;
	}
	uint32_t us = M32(timeout_ptr);
	return rt_now_us() + (us ? us : 1);
}

void rt_wait_until(uint64_t wake_us) { block_current(WT_DELAY, NULL, wake_us ? wake_us : 1); }

void rt_wait_vblank(void) {
	cur->vblank_target = vblanks + 1;
	block_current(WT_VBLANK, NULL, 0);
}
void rt_wait_vblanks(unsigned n) {
	cur->vblank_target = vblanks + (n ? n : 1);
	block_current(WT_VBLANK, NULL, 0);
}

// Runs fn(arg) on the scheduler's own stack and returns. In the browser the asynchronous
// JavaScript waits (Asyncify) all happen there, so a game thread's fiber only ever unwinds for
// fiber switches.
static void (*main_call_fn)(void *);
static void *main_call_arg;
void rt_call_on_main(void (*fn)(void *), void *arg) {
	if (!cur) {
		fn(arg);
		return;
	}
	main_call_fn = fn;
	main_call_arg = arg;
	coro_switch(&cur->co, &sched_co);
}

void rt_yield(void) {
	make_ready(cur, false);
	switch_to_scheduler();
}

static Thread *best_ready(void) {
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (t->uid && t->status == TS_READY &&
		    (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq))) {
			best = t;
		}
	}
	return best;
}

// Called before every HLE call: emulates the timer/vblank interrupts that would preempt the
// running thread on a real PSP, so a woken higher-priority thread gets to run.
static bool dispatch_off;  // sceKernelSuspendDispatchThread: the running thread keeps the CPU
void ge_poll(void);  // display.c: GE callbacks that are due
void rt_hle_poll(void) {
	if (!cur || dispatch_off) {
		return;
	}
	ge_poll();
	uint64_t now = rt_now_us();
	if (now >= next_vblank_us || now >= next_timer_us) {
		make_ready(cur, true);  // stays first among its priority level
		switch_to_scheduler();
	}
}

void rt_resched(void) {
	Thread *b = best_ready();
	if (b && cur && !dispatch_off && b->prio < cur->prio) {
		make_ready(cur, true);  // preempted: stays at the head of its priority level
		switch_to_scheduler();
	}
}

static void thread_exit(Thread *t, uint32_t status);

static void thread_trampoline(void *arg) {
	Thread *t = arg;
	if (t->native) {
		t->native(t->native_arg);
		thread_exit(t, 0);
	}
	RecompFunc f = rt_lookup(t->entry);
	if (!f) {
		rt_fatal("thread %s: entry %08x is not a known function", t->name, t->entry);
	}
	f(&t->cpu);
	thread_exit(t, t->cpu.r[2]);
}

static void thread_exit(Thread *t, uint32_t status) {
	TRACE("thread %s exits with %08x", t->name, status);
	t->status = TS_DORMANT;
	t->exit_status = status;
	t->wait_type = WT_NONE;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_THREADEND && w->wait_obj == t) {
			wake(w, (int32_t)status);
		}
	}
	switch_to_scheduler();
	rt_fatal("dormant thread %s resumed", t->name);
}

static Thread *thread_create(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t attr) {
	Thread *t = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!threads[i].uid) {
			t = &threads[i];
			break;
		}
	}
	if (!t) {
		return NULL;
	}
	Coro co = t->co;  // keep the host stack allocations for reuse
	memset(t, 0, sizeof *t);
	t->co = co;
	snprintf(t->name, sizeof t->name, "%s", name);
	t->entry = entry;
	t->init_prio = t->prio = prio;
	t->attr = attr;
	t->stack_size = align_up(stack_size < 0x200 ? 0x200 : stack_size, 0x100);
	t->stack_addr = rt_alloc_user(t->stack_size, true, name);
	if (!t->stack_addr) {
		return NULL;
	}
	if (!(attr & 0x00100000)) {  // PSP_THREAD_ATTR_NO_FILLSTACK
		memset(MP(t->stack_addr), 0xFF, t->stack_size);
	}
	M32(t->stack_addr) = 0;
	t->status = TS_DORMANT;
	t->uid = obj_new(OBJ_THREAD, t);
	M32(t->stack_addr) = t->uid;  // the kernel stores the thread id at the stack bottom
	return t;
}

static void thread_start(Thread *t, uint32_t arg_len, uint32_t argp) {
	memset(&t->cpu, 0, sizeof t->cpu);
	t->cpu.vfpu_ctrl[0] = t->cpu.vfpu_ctrl[1] = 0xE4;  // VFPU S/T prefixes: identity swizzle
	static const uint32_t rcx0[8] = {1, 2, 4, 8, 0, 0, 0, 0};  // the random generator's state at reset (PPSSPP's vrnd_init_default)
	memcpy(&t->cpu.vfpu_ctrl[8], rcx0, sizeof rcx0);
	uint32_t top = t->stack_addr + t->stack_size;
	uint32_t sp = top - 0x100;
	t->cpu.r[26] = sp;  // k0: thread context area
	if (arg_len && argp) {
		sp -= align_up(arg_len, 16);
		memmove(MP(sp), MP(argp), arg_len);
		t->cpu.r[5] = sp;
	}
	t->cpu.r[4] = arg_len;
#ifdef META_GP
	t->cpu.r[28] = META_GP;  // the module's gp, as the kernel gives every thread (a plain executable doesn't set it)
#endif
	t->cpu.r[29] = sp - 0x40;
	t->cpu.r[31] = 0;
	t->prio = t->init_prio;
	coro_init(&t->co, thread_trampoline, t);
	make_ready(t, false);
}

// A thread running C code of the runtime (a library's own thread, as the PSP's system modules have:
// the ad hoc matching's event thread), scheduled like the game's; it may call the game's code
// (rt_call_guest_args) and wait. Returns its id (0: no room).
uint32_t rt_native_thread(const char *name, void (*fn)(void *), void *arg, uint32_t prio, uint32_t stack_size) {
	Thread *t = thread_create(name, 0, prio, stack_size, 0);
	if (!t) return 0;
	t->native = fn;
	t->native_arg = arg;
	thread_start(t, 0, 0);
	return t->uid;
}

// Calls the game's function at func with up to 8 arguments (a0-a3, t0-t3) on the current thread;
// its result (v0).
uint32_t rt_call_guest_args(uint32_t func, const uint32_t *args, int n) {
	CPU *c = &cur->cpu;
	CPU saved = *c;
	for (int i = 0; i < n && i < 8; i++) c->r[4 + i] = args[i];
	c->r[31] = 0;
	rt_call(c, func);
	uint32_t v0 = c->r[2];
	*c = saved;
	return v0;
}

void kernel_create_main_thread(uint32_t entry, uint32_t prio, uint32_t stack_size, const char *argv0) {
	Thread *t = thread_create("user_main", entry, prio, stack_size, 0x80000000u);
	if (!t) {
		rt_fatal("cannot create main thread");
	}
	// Argument block: the boot path, copied by thread_start to the top of the stack.
	uint32_t len = (uint32_t)strlen(argv0) + 1;
	uint32_t tmp = rt_alloc_user(len, false, "argv");
	memcpy(MP(tmp), argv0, len);
	thread_start(t, len, tmp);
	rt_free_user(tmp);
	rt_log("main thread %08x entry %08x stack %08x+%x", t->uid, entry, t->stack_addr, t->stack_size);
}

// ---- event flags -------------------------------------------------------------
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr, init, pattern;
} EventFlag;

static bool evf_match(EventFlag *e, uint32_t bits, uint32_t mode) {
	return (mode & 1) ? (e->pattern & bits) != 0 : (e->pattern & bits) == bits;
}
static void evf_consume(EventFlag *e, uint32_t bits, uint32_t mode, uint32_t out) {
	if (out) {
		M32(out) = e->pattern;
	}
	if (mode & 0x10) {
		e->pattern = 0;  // CLEARALL
	}
	else if (mode & 0x20) {
		e->pattern &= ~bits;  // CLEAR
	}
}

static int evf_waiters(EventFlag *e) {
	int n = 0;
	for (int i = 0; i < MAX_THREADS; i++) {
		n += threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_EVF && threads[i].wait_obj == e;
	}
	return n;
}

static void evf_wake_waiters(EventFlag *e) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_EVF && w->wait_obj == e &&
		    evf_match(e, w->wait_bits, w->wait_mode)) {
			evf_consume(e, w->wait_bits, w->wait_mode, w->wait_out);
			wake(w, 0);
		}
	}
}

// ---- semaphores ---------------------------------------------------------------
// A count; waiters ask for a number of it and are served in the order they came (attr 0x100: by
// priority instead), each only when the count covers it (wait_bits: what it asks for, wait_mode:
// its place in the queue).
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr;
	int32_t init, count, max;
} Sema;
static uint32_t sema_queue;

static Thread *sema_first(Sema *s) {  // the waiter served next
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (!w->uid || w->status != TS_WAIT || w->wait_type != WT_SEMA || w->wait_obj != s) continue;
		if (!best || ((s->attr & 0x100) ? w->prio < best->prio || (w->prio == best->prio && w->wait_mode < best->wait_mode)
		                                : w->wait_mode < best->wait_mode))
			best = w;
	}
	return best;
}

static int sema_waiters(Sema *s) {
	int n = 0;
	for (int i = 0; i < MAX_THREADS; i++)
		n += threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_SEMA && threads[i].wait_obj == s;
	return n;
}

static void sema_wake_waiters(Sema *s) {
	for (Thread *w; (w = sema_first(s)) && (int32_t)w->wait_bits <= s->count;) {
		s->count -= (int32_t)w->wait_bits;
		wake(w, 0);
	}
}

// ---- variable pools ----------------------------------------------------------------
typedef struct VplAlloc {
	uint32_t addr, size;  // addr = block start (header), size includes 8-byte header
	struct VplAlloc *next;
} VplAlloc;
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr, base, size;
	VplAlloc *allocs;  // sorted
	Block *mem;
} Vpl;

static uint32_t vpl_try_alloc(Vpl *v, uint32_t size) {
	uint32_t need = align_up(size, 8) + 8;
	uint32_t prev = v->base;
	VplAlloc **pp = &v->allocs;
	for (;;) {
		uint32_t end = *pp ? (*pp)->addr : v->base + v->size;
		if (end >= prev && end - prev >= need) {
			VplAlloc *a = calloc(1, sizeof *a);
			a->addr = prev;
			a->size = need;
			a->next = *pp;
			*pp = a;
			M32(prev) = v->uid;
			M32(prev + 4) = need;
			return prev + 8;
		}
		if (!*pp) {
			return 0;
		}
		prev = (*pp)->addr + (*pp)->size;
		pp = &(*pp)->next;
	}
}

static uint32_t vpl_free_bytes(Vpl *v) {
	uint32_t used = 0;
	for (VplAlloc *a = v->allocs; a; a = a->next) {
		used += a->size;
	}
	return v->size - used;
}

static void vpl_wake_waiters(Vpl *v) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_VPL && w->wait_obj == v) {
			uint32_t a = vpl_try_alloc(v, w->wait_bits);
			if (a) {
				M32(w->wait_out) = a;
				wake(w, 0);
			}
		}
	}
}

// ---- callbacks -------------------------------------------------------------------
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t func, arg;
	Thread *owner;
	int notify_count;
	uint32_t notify_arg;
} Callback;

void kernel_run_callbacks(void);

// ---- scheduler ---------------------------------------------------------------------
void kernel_init(void) {
	heap_lo = align_up(rt_module_end, 0x100);
	heap_hi = PSP_RAM_END;
	next_vblank_us = VBLANK_US;
	// The loaded module occupies its own block so allocations never overlap it.
	(void)block_alloc(rt_module_end - USER_PARTITION_START, 2, USER_PARTITION_START, "module");
}

static void check_timers(uint64_t now) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid || t->status != TS_WAIT) {
			continue;
		}
		if (t->wait_type == WT_VBLANK && vblanks >= t->vblank_target) {
			wake(t, 0);
		}
		else if (t->wake_us && now >= t->wake_us) {
			bool timeout = t->wait_type != WT_DELAY && t->wait_type != WT_TIME;
			if (timeout && t->timeout_ptr) {
				M32(t->timeout_ptr) = 0;
			}
			wake(t, timeout ? (int32_t)ERR_WAIT_TIMEOUT : 0);
		}
	}
}

// ---- interrupts: the vblank sub-interrupt handlers (sceKernelRegisterSubIntrHandler on
// PSP_VBLANK_INT), called once per vblank outside any thread, on their own stack. They must not
// block, as on the PSP.
#define VBLANK_INT 30
typedef struct {
	uint32_t func, arg;
	bool enabled;
} SubIntr;
static SubIntr vblank_subs[16];
static CPU intr_cpu;
static uint32_t intr_stack;

static void run_vblank_handlers(void) {
	Thread *saved = cur;
	for (int i = 0; i < 16; i++) {
		SubIntr *h = &vblank_subs[i];
		if (!h->func || !h->enabled) continue;
		if (!intr_stack) intr_stack = rt_alloc_user(0x4000, true, "interrupt stack");
		memset(&intr_cpu, 0, sizeof intr_cpu);
		intr_cpu.vfpu_ctrl[0] = intr_cpu.vfpu_ctrl[1] = 0xE4;
		intr_cpu.r[4] = (uint32_t)i;
		intr_cpu.r[5] = h->arg;
		intr_cpu.r[29] = intr_stack + 0x4000 - 0x40;
		intr_cpu.r[28] = saved ? saved->cpu.r[28] : 0;  // gp
		intr_cpu.r[31] = 0;
		cur = NULL;  // no thread is running: HLE calls must not switch threads
		rt_call(&intr_cpu, h->func);
		cur = saved;
	}
}

void hle_sceKernelRegisterSubIntrHandler(CPU *c) {
	TRACE("sceKernelRegisterSubIntrHandler(%d, %d, %08x, %08x)", A0, A1, A2, A3);
	if (A0 != VBLANK_INT || A1 >= 16) {
		rt_log("sceKernelRegisterSubIntrHandler(%d, %d): only vblank handlers are run", A0, A1);
		RET(0);
		return;
	}
	vblank_subs[A1].func = A2;
	vblank_subs[A1].arg = A3;
	RET(0);
}
void hle_sceKernelReleaseSubIntrHandler(CPU *c) {
	if (A0 == VBLANK_INT && A1 < 16) memset(&vblank_subs[A1], 0, sizeof vblank_subs[A1]);
	RET(0);
}
void hle_sceKernelEnableSubIntr(CPU *c) {
	if (A0 == VBLANK_INT && A1 < 16) vblank_subs[A1].enabled = true;
	RET(0);
}
void hle_sceKernelDisableSubIntr(CPU *c) {
	if (A0 == VBLANK_INT && A1 < 16) vblank_subs[A1].enabled = false;
	RET(0);
}

extern volatile unsigned long rt_sched_heartbeat;

// Is a vblank due? By our clock (59.94 Hz), or, when the platform's display refresh paces them
// (plat_vsync: the browser on a 60 Hz display), at each refresh, with our clock as the fallback
// when one is late (next_vblank_us is then that fallback: rt_hle_poll preempts no thread for a
// refresh that can only arrive once the browser gets a turn).
static uint64_t last_vblank_us;
static bool vblank_due(uint64_t now) {
	// A stretch skipped (rt_turbo): every 2 ms of real work the next vblank comes at once, the clock
	// moved on to it (every thread still gets its turn: a frame of work in a fraction of the time)
	static uint64_t turbo_real;
	if (rt_turbo_on() && now < next_vblank_us && rt_real_us() - turbo_real >= 2000) {
		rt_warp(next_vblank_us - now);
		now = next_vblank_us;
		turbo_real = rt_real_us();
	}
	int v = rt_turbo_on() ? -1 : plat_vsync();
	if (v < 0) {
		if (now < next_vblank_us) return false;
		next_vblank_us += VBLANK_US;
		if (now > next_vblank_us + 4 * VBLANK_US) {
			next_vblank_us = now + VBLANK_US;  // we fell behind (long-running thread); resync
		}
	}
	else {
		if (!v && now < next_vblank_us) return false;
		next_vblank_us = now + VBLANK_US + VBLANK_US / 2;
	}
	last_vblank_us = now;
	plat_vsync_taken();
	return true;
}

// The display's current line (sceDisplayGetCurrentHcount): 286 a frame, counted from the last
// vblank. Games time their frames by it: a game reads it after its drawing and, at line 264 or
// later (the frame nearly gone), drops to 30 frames a second for good.
uint32_t rt_current_hcount(void) {
	uint64_t now = rt_now_us(), since = now > last_vblank_us ? now - last_vblank_us : 0;
	uint64_t line = since * 286 / VBLANK_US;
	return (uint32_t)(line < 285 ? line : 285);
}

void kernel_run(void) {
	coro_init_main(&sched_co);
	for (;;) {
		rt_sched_heartbeat++;
		uint64_t now = rt_now_us();
		if (vblank_due(now)) {
			vblanks++;
			platform_vblank();
			run_vblank_handlers();
		}
		check_timers(now);
		next_timer_us = UINT64_MAX;
		for (int i = 0; i < MAX_THREADS; i++) {
			if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wake_us && threads[i].wake_us < next_timer_us) {
				next_timer_us = threads[i].wake_us;
			}
		}
		Thread *t = best_ready();
		if (!t) {
			uint64_t next = next_vblank_us;
			for (int i = 0; i < MAX_THREADS; i++) {
				if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wake_us && threads[i].wake_us < next) {
					next = threads[i].wake_us;
				}
			}
			now = rt_now_us();
			if (next > now) {
				platform_idle(next - now);
			}
			continue;
		}
		cur = t;
		t->status = TS_RUNNING;
		coro_switch(&sched_co, &t->co);
		while (main_call_fn) {  // the thread asked to run something here (rt_call_on_main)
			void (*fn)(void *) = main_call_fn;
			main_call_fn = NULL;
			fn(main_call_arg);
			coro_switch(&sched_co, &t->co);
		}
		cur = NULL;
	}
}

// Debug: one line per thread (called from the watchdog signal handler; uses fprintf on purpose,
// this is only for diagnosing hangs).
void kernel_dump_threads(void) {
	static const char *wt[] = {"-", "delay", "vblank", "evf", "thread-end", "vpl", "time", "sema", "sleep", "mutex", "fpl"};
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid) continue;
		fprintf(stderr, "  %-14s prio=%02x status=%2d wait=%-10s sp=%08x ra=%08x%s\n", t->name, t->prio, t->status,
		        t->status & TS_WAIT ? wt[t->wait_type] : "", t->cpu.r[29], t->cpu.r[31], t == cur ? "  <- running" : "");
		// The guest's callers, guessed from its stack: words that point just past a jal/jalr in the code
		// (return addresses the functions saved), the nearest first.
		if (getenv("DBZ_STATUS_STACK")) {
			fprintf(stderr, "    stack:");
			int n = 0;
			for (uint32_t a = t->cpu.r[29] & ~3u; a < t->cpu.r[29] + 0x800 && n < 16; a += 4) {
				if (!rt_valid(a, 4)) break;
				uint32_t v = M32(a);
				if (v < 0x08804008 || v >= META_IMAGE_END || (v & 3)) continue;
				uint32_t op = M32(v - 8);
				if (op >> 26 == 3 || (op >> 26 == 0 && (op & 0x3F) == 9)) fprintf(stderr, " %08x", v - 8), n++;
			}
			fprintf(stderr, "\n");
		}
	}
}

void rt_call_guest(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) {
	CPU *c = &cur->cpu;
	CPU saved = *c;
	c->r[4] = a0;
	c->r[5] = a1;
	c->r[6] = a2;
	c->r[31] = 0;
	rt_call(c, func);
	*c = saved;
}

// =====================================================================================
// HLE: ThreadManForUser
// =====================================================================================

void hle_sceKernelCreateThread(CPU *c) {
	const char *name = rt_str(A0);
	Thread *t = thread_create(name, A1, A2, A3, A4);
	TRACE("sceKernelCreateThread(%s, entry=%08x, prio=%x, stack=%x, attr=%x) -> %08x", name, A1, A2, A3, A4,
	      t ? t->uid : 0);
	RET(t ? t->uid : ERR_NO_MEMORY);
}

void hle_sceKernelStartThread(CPU *c) {
	Thread *t = thread_get(A0);
	TRACE("sceKernelStartThread(%08x %s, len=%x, argp=%08x)", A0, t ? t->name : "?", A1, A2);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status != TS_DORMANT) {
		RET(ERR_NOT_DORMANT);
		return;
	}
	thread_start(t, A1, A2);
	RET(0);
	rt_resched();
}

void hle_sceKernelExitThread(CPU *c) { thread_exit(cur, A0); }

void hle_sceKernelDeleteThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status != TS_DORMANT) {
		RET(ERR_NOT_DORMANT);
		return;
	}
	rt_free_user(t->stack_addr);
	obj_free(t->uid);
	t->uid = 0;
	RET(0);
}

void hle_sceKernelWaitThreadEnd(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status == TS_DORMANT) {
		RET(t->exit_status);
		return;
	}
	cur->timeout_ptr = A1;
	RET(block_current(WT_THREADEND, t, timeout_deadline(A1)));
}
void hle_sceKernelWaitThreadEndCB(CPU *c) {
	kernel_run_callbacks();
	hle_sceKernelWaitThreadEnd(c);
}

void hle_sceKernelSuspendThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_ILLEGAL_ARG);
		return;
	}
	if (t->status & TS_DORMANT) {
		RET(ERR_DORMANT);
		return;
	}
	if (t->status & TS_SUSPEND) {
		RET(0x800201A1u - 0x0);  // already suspended (game logs and ignores)
		return;
	}
	t->status = (t->status == TS_WAIT) ? (TS_WAIT | TS_SUSPEND) : TS_SUSPEND;
	RET(0);
}

void hle_sceKernelResumeThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || !(t->status & TS_SUSPEND)) {
		RET(0x800201A2u);
		return;
	}
	if (t->status == (TS_WAIT | TS_SUSPEND)) {
		t->status = TS_WAIT;
	}
	else {
		make_ready(t, false);
	}
	RET(0);
	rt_resched();
}

void hle_sceKernelGetThreadId(CPU *c) { RET(cur->uid); }
void hle_sceKernelGetThreadCurrentPriority(CPU *c) { RET(cur->prio); }
void hle_sceKernelChangeCurrentThreadAttr(CPU *c) {
	cur->attr = (cur->attr & ~A0) | A1;
	RET(0);
}

void hle_sceKernelReferThreadStatus(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	uint32_t p = A1;
	uint32_t size = M32(p);
	uint8_t info[0x68] = {0};
	uint32_t *w = (uint32_t *)info;
	w[0] = size;
	memcpy(info + 4, t->name, 32);
	w[9] = t->attr;
	w[10] = t->status == TS_RUNNING ? 1 : t->status;
	w[11] = t->entry;
	w[12] = t->stack_addr;
	w[13] = t->stack_size;
	w[14] = 0;  // gp
	w[15] = t->init_prio;
	w[16] = t->prio;
	w[17] = t->status == TS_WAIT ? (t->wait_type == WT_DELAY ? 2 : 1) : 0;
	w[20] = t->exit_status;
	memcpy(MP(p), info, size < sizeof info ? size : sizeof info);
	RET(0);
}

void hle_sceKernelDelayThread(CPU *c) {
	uint32_t us = A0 < 200 ? 200 : A0;  // the PSP rounds tiny delays up
	RET(0);
	rt_wait_until(rt_now_us() + us);
	RET(0);
}
void hle_sceKernelDelayThreadCB(CPU *c) {
	kernel_run_callbacks();
	hle_sceKernelDelayThread(c);
}

// -- sleep / wakeup: a wakeup sent before the sleep is kept, and lets the next sleep through
void hle_sceKernelSleepThread(CPU *c) {
	if (cur->wakeups) {
		cur->wakeups--;
		RET(0);
		return;
	}
	RET(block_current(WT_SLEEP, NULL, 0));
}
void hle_sceKernelSleepThreadCB(CPU *c) {
	kernel_run_callbacks();
	hle_sceKernelSleepThread(c);
}

void hle_sceKernelWakeupThread(CPU *c) {
	Thread *t = A0 ? obj_get(A0, OBJ_THREAD) : cur;
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status == TS_WAIT && t->wait_type == WT_SLEEP) wake(t, 0);
	else t->wakeups++;
	RET(0);
	rt_resched();
}

// While a GE callback runs late (display.c), the system clock says when the GE finished.
uint64_t rt_clock_at_us;
void hle_sceKernelGetSystemTimeLow(CPU *c) { RET((uint32_t)(rt_clock_at_us ? rt_clock_at_us : rt_now_us())); }
// A clock that advances with the vblanks: rt_vblank_time_us(n) is vblank n's time, 16.683 ms
// apart (from the real time at the first call). While rt_clock_vblanks is set (mods/frameclock.c:
// the game's frame clock), sceKernelGetSystemTimeWide answers vblank rt_clock_vblank's.
bool rt_clock_vblanks;
uint64_t rt_clock_vblank;
uint64_t rt_vblank_time_us(uint64_t n) {
	static uint64_t base, n0;
	static bool init;
	if (!init) {
		init = true;
		base = rt_now_us();
		n0 = n;
	}
	return base + (uint64_t)((int64_t)(n - n0) * (int64_t)VBLANK_US);
}

void hle_sceKernelGetSystemTimeWide(CPU *c) {
	RET64(rt_clock_at_us ? rt_clock_at_us : rt_clock_vblanks ? rt_vblank_time_us(rt_clock_vblank) : rt_now_us());
}

// -- event flags
void hle_sceKernelCreateEventFlag(CPU *c) {
	EventFlag *e = calloc(1, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", rt_str(A0));
	e->attr = A1;
	e->init = e->pattern = A2;
	e->uid = obj_new(OBJ_EVF, e);
	TRACE("sceKernelCreateEventFlag(%s, attr=%x, init=%x) -> %08x", e->name, A1, A2, e->uid);
	RET(e->uid);
}

void hle_sceKernelDeleteEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	for (int i = 0; i < MAX_THREADS; i++) {
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_EVF && threads[i].wait_obj == e) {
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
		}
	}
	obj_free(e->uid);
	free(e);
	RET(0);
	rt_resched();
}

void hle_sceKernelSetEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	e->pattern |= A1;
	evf_wake_waiters(e);
	RET(0);
	rt_resched();
}

void hle_sceKernelClearEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	e->pattern &= A1;
	RET(0);
}

void hle_sceKernelPollEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	if (A1 == 0) {
		RET(ERR_EVF_ILPAT);
		return;
	}
	if (evf_match(e, A1, A2)) {
		evf_consume(e, A1, A2, A3);
		RET(0);
	}
	else {
		if (A3) {
			M32(A3) = e->pattern;
		}
		RET(ERR_EVF_COND);
	}
}

void hle_sceKernelWaitEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	if (A1 == 0) {
		RET(ERR_EVF_ILPAT);
		return;
	}
	if (evf_match(e, A1, A2)) {
		evf_consume(e, A1, A2, A3);
		RET(0);
		return;
	}
	if (!(e->attr & 0x200) && evf_waiters(e) > 0) {  // PSP_EVENT_WAITMULTIPLE
		RET(ERR_EVF_MULTI);
		return;
	}
	cur->wait_bits = A1;
	cur->wait_mode = A2;
	cur->wait_out = A3;
	cur->timeout_ptr = A4;
	RET(block_current(WT_EVF, e, timeout_deadline(A4)));
}

// -- semaphores
void hle_sceKernelCreateSema(CPU *c) {
	if ((int32_t)A2 < 0 || (int32_t)A3 <= 0 || (int32_t)A2 > (int32_t)A3) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	Sema *s = calloc(1, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", rt_str(A0));
	s->attr = A1;
	s->init = s->count = (int32_t)A2;
	s->max = (int32_t)A3;
	s->uid = obj_new(OBJ_SEMA, s);
	TRACE("sceKernelCreateSema(%s, attr=%x, init=%d, max=%d) -> %08x", s->name, A1, (int32_t)A2, (int32_t)A3, s->uid);
	RET(s->uid);
}

void hle_sceKernelDeleteSema(CPU *c) {
	Sema *s = obj_get(A0, OBJ_SEMA);
	if (!s) {
		RET(ERR_UNKNOWN_SEMID);
		return;
	}
	for (int i = 0; i < MAX_THREADS; i++)
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_SEMA && threads[i].wait_obj == s)
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
	obj_free(s->uid);
	free(s);
	RET(0);
	rt_resched();
}

void hle_sceKernelSignalSema(CPU *c) {
	Sema *s = obj_get(A0, OBJ_SEMA);
	if (!s) {
		RET(ERR_UNKNOWN_SEMID);
		return;
	}
	int32_t n = (int32_t)A1;
	if (n < 0 || (int64_t)s->count + n - sema_waiters(s) > s->max) {  // the waiting threads take some of it first
		RET(ERR_SEMA_OVF);
		return;
	}
	s->count += n;
	sema_wake_waiters(s);
	RET(0);
	rt_resched();
}

static void sema_wait(CPU *c) {
	Sema *s = obj_get(A0, OBJ_SEMA);
	if (!s) {
		RET(ERR_UNKNOWN_SEMID);
		return;
	}
	int32_t need = (int32_t)A1;
	if (need <= 0 || need > s->max) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	if (s->count >= need && !sema_first(s)) {
		s->count -= need;
		RET(0);
		return;
	}
	cur->wait_bits = (uint32_t)need;
	cur->wait_mode = ++sema_queue;
	cur->timeout_ptr = A2;
	RET(block_current(WT_SEMA, s, timeout_deadline(A2)));
}
void hle_sceKernelWaitSema(CPU *c) { sema_wait(c); }
void hle_sceKernelWaitSemaCB(CPU *c) {
	kernel_run_callbacks();
	sema_wait(c);
}

void hle_sceKernelPollSema(CPU *c) {
	Sema *s = obj_get(A0, OBJ_SEMA);
	if (!s) {
		RET(ERR_UNKNOWN_SEMID);
		return;
	}
	int32_t need = (int32_t)A1;
	if (need <= 0) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	if (s->count >= need && !sema_first(s)) {
		s->count -= need;
		RET(0);
	}
	else RET(ERR_SEMA_ZERO);
}

void hle_sceKernelReferSemaStatus(CPU *c) {
	Sema *s = obj_get(A0, OBJ_SEMA);
	if (!s) {
		RET(ERR_UNKNOWN_SEMID);
		return;
	}
	uint32_t p = A1;
	M32(p + 0) = 0x38;
	rt_write_cstr(p + 4, 32, s->name);
	M32(p + 36) = s->attr;
	M32(p + 40) = (uint32_t)s->init;
	M32(p + 44) = (uint32_t)s->count;
	M32(p + 48) = (uint32_t)s->max;
	M32(p + 52) = (uint32_t)sema_waiters(s);
	RET(0);
}

void hle_sceKernelReferEventFlagStatus(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	uint32_t p = A1;
	M32(p + 0) = 0x34;
	rt_write_cstr(p + 4, 32, e->name);
	M32(p + 36) = e->attr;
	M32(p + 40) = e->init;
	M32(p + 44) = e->pattern;
	M32(p + 48) = (uint32_t)evf_waiters(e);
	RET(0);
}

// -- variable pools
void hle_sceKernelCreateVpl(CPU *c) {
	uint32_t size = A3;
	Vpl *v = calloc(1, sizeof *v);
	snprintf(v->name, sizeof v->name, "%s", rt_str(A0));
	v->attr = A2;
	v->mem = block_alloc(size, (A2 & 0x4000) ? 1 : 0, 0, v->name);
	if (!v->mem) {
		free(v);
		RET(ERR_NO_MEMORY);
		return;
	}
	v->base = v->mem->addr;
	v->size = size;
	v->uid = obj_new(OBJ_VPL, v);
	TRACE("sceKernelCreateVpl(%s, part=%d, attr=%x, size=%x) -> %08x at %08x", v->name, A1, A2, size, v->uid, v->base);
	RET(v->uid);
}

void hle_sceKernelDeleteVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	while (v->allocs) {
		VplAlloc *n = v->allocs->next;
		free(v->allocs);
		v->allocs = n;
	}
	block_free(v->mem);
	obj_free(v->uid);
	free(v);
	RET(0);
}

void hle_sceKernelAllocateVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t a = vpl_try_alloc(v, A1);
	if (a) {
		M32(A2) = a;
		RET(0);
		return;
	}
	TRACE("sceKernelAllocateVpl(%s, %x): pool full, waiting", v->name, A1);
	cur->wait_bits = A1;
	cur->wait_out = A2;
	cur->timeout_ptr = A3;
	RET(block_current(WT_VPL, v, timeout_deadline(A3)));
}

void hle_sceKernelFreeVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t hdr = A1 - 8;
	for (VplAlloc **pp = &v->allocs; *pp; pp = &(*pp)->next) {
		if ((*pp)->addr == hdr) {
			VplAlloc *a = *pp;
			*pp = a->next;
			free(a);
			vpl_wake_waiters(v);
			RET(0);
			rt_resched();
			return;
		}
	}
	RET(ERR_ILLEGAL_MEMBLOCK);
}

void hle_sceKernelReferVplStatus(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t p = A1;
	M32(p) = 0x34;
	rt_write_cstr(p + 4, 32, v->name);
	M32(p + 36) = v->attr;
	M32(p + 40) = v->size;
	M32(p + 44) = vpl_free_bytes(v);
	M32(p + 48) = 0;
	RET(0);
}

// -- callbacks
void hle_sceKernelCreateCallback(CPU *c) {
	Callback *cb = calloc(1, sizeof *cb);
	snprintf(cb->name, sizeof cb->name, "%s", rt_str(A0));
	cb->func = A1;
	cb->arg = A2;
	cb->owner = cur;
	cb->uid = obj_new(OBJ_CALLBACK, cb);
	TRACE("sceKernelCreateCallback(%s, %08x, %08x) -> %08x", cb->name, A1, A2, cb->uid);
	RET(cb->uid);
}

void hle_sceKernelDeleteCallback(CPU *c) {
	Callback *cb = obj_get(A0, OBJ_CALLBACK);
	if (!cb) {
		RET(ERR_UNKNOWN_CBID);
		return;
	}
	obj_free(cb->uid);
	free(cb);
	RET(0);
}

static int pending_callbacks;

// A system event for a callback (the UMD drive's state...): it runs on its owner thread at that
// thread's next sceKernelCheckCallback or ...CB wait, with the count of notifications and the
// last argument.
void kernel_notify_callback(uint32_t uid, uint32_t arg) {
	Callback *cb = obj_get(uid, OBJ_CALLBACK);
	if (!cb) return;
	if (!cb->notify_count++) pending_callbacks++;
	cb->notify_arg = arg;
	TRACE("callback %s notified (%08x)", cb->name, arg);
}

// The current thread's notified callbacks, run now; how many ran.
static int run_callbacks(void) {
	int ran = 0;
	for (int i = 1; pending_callbacks && i < MAX_OBJS; i++) {
		if (objs[i].type == OBJ_CALLBACK) {
			Callback *cb = objs[i].p;
			if (cb->owner == cur && cb->notify_count) {
				int n = cb->notify_count;
				cb->notify_count = 0;
				pending_callbacks--;
				rt_call_guest(cb->func, (uint32_t)n, cb->notify_arg, cb->arg);
				ran++;
			}
		}
	}
	return ran;
}
void kernel_run_callbacks(void) {
	if (pending_callbacks) run_callbacks();
}

void hle_sceKernelCheckCallback(CPU *c) {
	if (!pending_callbacks) {
		// Games spin on this while another thread works; nothing can change before the next
		// timer or vblank, so sleep until then instead of burning the CPU.
		uint64_t vb = last_vblank_us + VBLANK_US < next_vblank_us ? last_vblank_us + VBLANK_US : next_vblank_us;
		uint64_t next = vb < next_timer_us ? vb : next_timer_us;
		uint64_t now = rt_now_us();
		rt_wait_until(next > now + 100 ? next : now + 100);
		RET(0);
		return;
	}
	int ran = run_callbacks();
	RET(ran);
}

// =====================================================================================
// HLE: SysMemUserForUser, Kernel_Library, LoadExec, ModuleMgr, Stdio, Utils, Suspend...
// =====================================================================================

void hle_sceKernelAllocPartitionMemory(CPU *c) {
	const char *name = rt_str(A1);
	Block *b = block_alloc(A3, (int)A2, A4, name);
	TRACE("sceKernelAllocPartitionMemory(part=%d, %s, type=%d, size=%x, addr=%x) -> %08x", A0, name, A2, A3, A4,
	      b ? b->addr : 0);
	RET(b ? b->uid : ERR_NO_MEMORY);
}

void hle_sceKernelFreePartitionMemory(CPU *c) {
	Block *b = obj_get(A0, OBJ_BLOCK);
	if (!b) {
		RET(ERR_UNKNOWN_UID);
		return;
	}
	block_free(b);
	RET(0);
}

void hle_sceKernelGetBlockHeadAddr(CPU *c) {
	Block *b = obj_get(A0, OBJ_BLOCK);
	RET(b ? b->addr : 0);
}

void hle_sceKernelTotalFreeMemSize(CPU *c) { RET(rt_user_free_total()); }
void hle_sceKernelSetCompilerVersion(CPU *c) { RET(0); }
void hle_sceKernelSetCompiledSdkVersion395(CPU *c) { RET(0); }
void hle_sceKernelSetCompiledSdkVersion500_505(CPU *c) { RET(0); }

// Minimal printf for guest varargs (integers and strings only), args in a1..a3, t0..t3.
static void guest_format(CPU *c, uint32_t fmt_addr, int first_reg, char *out, size_t cap) {
	static const int regs[] = {4, 5, 6, 7, 8, 9, 10, 11};
	int ri = first_reg;
	const char *f = rt_str(fmt_addr);
	size_t n = 0;
	while (*f && n + 1 < cap) {
		if (*f != '%') {
			out[n++] = *f++;
			continue;
		}
		char spec[16];
		int sl = 0;
		spec[sl++] = *f++;
		while (*f && strchr("-+ #0123456789.lh", *f) && sl < 14) {
			if (*f != 'l' && *f != 'h') {
				spec[sl++] = *f;
			}
			f++;
		}
		char conv = *f ? *f++ : 0;
		spec[sl++] = conv;
		spec[sl] = 0;
		uint32_t arg = (conv != '%' && ri < 8) ? c->r[regs[ri++]] : 0;
		char tmp[512];
		switch (conv) {
		case 's': snprintf(tmp, sizeof tmp, spec, rt_valid(arg, 1) ? rt_str(arg) : "(bad)"); break;
		case '%': snprintf(tmp, sizeof tmp, "%%"); break;
		case 'f': case 'g': case 'e': snprintf(tmp, sizeof tmp, "<float>"); break;
		default: snprintf(tmp, sizeof tmp, spec, arg); break;
		}
		for (char *p = tmp; *p && n + 1 < cap; p++) {
			out[n++] = *p;
		}
	}
	out[n] = 0;
}

void hle_sceKernelPrintf(CPU *c) {
	char buf[1024];
	guest_format(c, A0, 1, buf, sizeof buf);
	rt_log("printf: %s", buf);
	RET(0);
}

void hle_sceKernelCpuSuspendIntr(CPU *c) { RET(1); }
void hle_sceKernelCpuResumeIntr(CPU *c) { RET(0); }

void hle_sceKernelExitGame(CPU *c) {
	rt_log("sceKernelExitGame");
	exit(0);
}
void hle_sceKernelRegisterExitCallback(CPU *c) { RET(0); }

static uint32_t main_module_uid;
static uint32_t fake_module(void) { return obj_new(OBJ_MODULE, NULL); }

// Modules are not loaded (the system's are HLE): a disc module that exists gets an id. As on a
// retail PSP, a module on the memory stick is refused (a game from the disc may not load one) and
// a missing file is an error: games check it. a game tries ms0:/MEMSTICK.IND, and a success is
// what a modded console with plugins answers: it then breaks its own file checks (endless loading).
bool io_disc_lookup(const char *path, uint64_t *off, uint32_t *size);
void hle_sceKernelLoadModule(CPU *c) {
	const char *path = rt_str(A0);
	const char *colon = strchr(path, ':');
	char dev[16] = {0};
	if (colon && colon - path < (int)sizeof dev)
		for (int i = 0; i < colon - path; i++) dev[i] = (char)tolower((unsigned char)path[i]);
	if (!strcmp(dev, "ms0") || !strcmp(dev, "fatms0") || !strcmp(dev, "ef0")) {
		rt_log("sceKernelLoadModule(%s): refused (memory stick), as on a retail PSP", path);
		RET(0x80020146u);  // SCE_KERNEL_ERROR_PROHIBIT_LOADMODULE_DEVICE
		return;
	}
	uint64_t off;
	uint32_t size;
	if (colon && !io_disc_lookup(path, &off, &size)) {
		rt_log("sceKernelLoadModule(%s): no such file", path);
		RET(0x80010002u);  // SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND
		return;
	}
	uint32_t uid = fake_module();
	rt_log("sceKernelLoadModule(%s) -> %08x (HLE, not loaded)", path, uid);
	RET(uid);
}
void hle_sceKernelStartModule(CPU *c) {
	if (A3) {
		M32(A3) = 0;
	}
	RET(A0);
}
void hle_sceKernelStopModule(CPU *c) {
	if (A3) {
		M32(A3) = 0;
	}
	RET(0);
}
void hle_sceKernelUnloadModule(CPU *c) { RET(0); }
void hle_sceKernelGetModuleId(CPU *c) {
	if (!main_module_uid) {
		main_module_uid = fake_module();
	}
	RET(main_module_uid);
}
void hle_sceKernelGetModuleIdByAddress(CPU *c) { hle_sceKernelGetModuleId(c); }
void hle_sceKernelStopUnloadSelfModuleWithStatus(CPU *c) {
	rt_log("sceKernelStopUnloadSelfModuleWithStatus(%x): game is exiting (ra=%08x)", A0, c->r[31]);
	exit((int)A0);
}

void hle_sceKernelStdin(CPU *c) { RET(0); }
void hle_sceKernelStdout(CPU *c) { RET(1); }
void hle_sceKernelStderr(CPU *c) { RET(2); }

// The date and time the game sees. DBZ_FAKE_TIME=<unix seconds> (tests): the clock starts there and
// moves in whole seconds, the same in every run (Arcade draws its stages from the clock: a stage
// can be had again).
void rt_wallclock(struct timeval *tv) {
	gettimeofday(tv, NULL);
	static long long fake = -2, start;
	if (fake == -2) {
		const char *e = getenv("DBZ_FAKE_TIME");
		fake = e ? atoll(e) : -1;
		start = tv->tv_sec;
	}
	if (fake >= 0) tv->tv_sec = (time_t)(fake + (tv->tv_sec - start)), tv->tv_usec = 0;
}
void hle_sceKernelLibcClock(CPU *c) { RET((uint32_t)rt_now_us()); }
void hle_sceKernelLibcTime(CPU *c) {
	struct timeval tv;
	rt_wallclock(&tv);
	uint32_t t = (uint32_t)tv.tv_sec;
	if (A0) {
		M32(A0) = t;
	}
	RET(t);
}
void hle_sceKernelLibcGettimeofday(CPU *c) {
	struct timeval tv;
	rt_wallclock(&tv);
	if (A0) {
		M32(A0) = (uint32_t)tv.tv_sec;
		M32(A0 + 4) = (uint32_t)tv.tv_usec;
	}
	RET(0);
}
void hle_sceKernelDcacheWritebackAll(CPU *c) { RET(0); }
void hle_sceKernelDcacheWritebackInvalidateAll(CPU *c) { RET(0); }
void hle_sceKernelDcacheWritebackRange(CPU *c) { RET(0); }

void hle_sceKernelPowerTick(CPU *c) { RET(0); }
void hle_sceKernelPowerLock(CPU *c) { RET(0); }
void hle_sceKernelPowerUnlock(CPU *c) { RET(0); }
void hle_sceKernelVolatileMemLock(CPU *c) {
	if (A1) {
		M32(A1) = 0x08400000u;
	}
	if (A2) {
		M32(A2) = 0x00400000u;
	}
	RET(0);
}
void hle_sceKernelVolatileMemTryLock(CPU *c) { hle_sceKernelVolatileMemLock(c); }
void hle_sceKernelVolatileMemUnlock(CPU *c) { RET(0); }

void hle_scePowerRegisterCallback(CPU *c) { RET(0); }
void hle_scePowerUnregisterCallback(CPU *c) { RET(0); }
void hle_scePowerIsBatteryExist(CPU *c) { RET(1); }
void hle_scePowerIsPowerOnline(CPU *c) { RET(1); }

void hle_sceImposeSetLanguageMode(CPU *c) { RET(0); }

void hle_sceRtcGetCurrentTick(CPU *c) {
	struct timeval tv;
	rt_wallclock(&tv);
	// PSP ticks: microseconds since 0001-01-01.
	uint64_t ticks = ((uint64_t)tv.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)tv.tv_usec;
	M32(A0) = (uint32_t)ticks;
	M32(A0 + 4) = (uint32_t)(ticks >> 32);
	RET(0);
}

void hle_sceRtcGetCurrentClockLocalTime(CPU *c) {
	struct timeval tv;
	rt_wallclock(&tv);
	struct tm tm;
	time_t s = tv.tv_sec;
	localtime_r(&s, &tm);
	uint32_t p = A0;
	M16(p + 0) = (uint16_t)(tm.tm_year + 1900);
	M16(p + 2) = (uint16_t)(tm.tm_mon + 1);
	M16(p + 4) = (uint16_t)tm.tm_mday;
	M16(p + 6) = (uint16_t)tm.tm_hour;
	M16(p + 8) = (uint16_t)tm.tm_min;
	M16(p + 10) = (uint16_t)tm.tm_sec;
	M32(p + 12) = (uint32_t)tv.tv_usec;
	RET(0);
}

// sceWlanGetEtherAddr: net.c (the ad hoc MAC)
void hle_sceWlanGetSwitchState(CPU *c) { RET(1); }

// ---- more of ThreadManForUser / Kernel_Library
void hle_sceKernelWaitEventFlagCB(CPU *c) {
	kernel_run_callbacks();
	hle_sceKernelWaitEventFlag(c);
}

void hle_sceKernelChangeThreadPriority(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	uint32_t prio = A1 ? A1 : (cur ? cur->prio : t->prio);  // 0: the caller's priority
	if (prio < 8 || prio > 119) {
		RET(ERR_ILLEGAL_PRIORITY);
		return;
	}
	t->prio = prio;
	if (t->status == TS_READY) make_ready(t, false);  // goes to the back of its new level
	RET(0);
	rt_resched();
}

void hle_sceKernelTerminateDeleteThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	TRACE("sceKernelTerminateDeleteThread(%s)", t->name);
	if (t->status != TS_DORMANT) {
		t->status = TS_DORMANT;  // its host stack is simply abandoned; a restart re-creates it
		t->wait_type = WT_NONE;
		t->exit_status = 0x800201ACu;  // SCE_KERNEL_ERROR_THREAD_TERMINATED
		for (int i = 0; i < MAX_THREADS; i++) {
			Thread *w = &threads[i];
			if (w->uid && w->status == TS_WAIT && w->wait_type == WT_THREADEND && w->wait_obj == t) {
				wake(w, (int32_t)t->exit_status);
			}
		}
	}
	rt_free_user(t->stack_addr);
	obj_free(t->uid);
	t->uid = 0;
	RET(0);
}

void hle_sceKernelSuspendDispatchThread(CPU *c) {
	uint32_t was = dispatch_off ? 0 : 1;
	dispatch_off = true;
	RET(was);
}
void hle_sceKernelResumeDispatchThread(CPU *c) {
	dispatch_off = A0 == 0;
	RET(0);
	if (!dispatch_off) rt_resched();
}

void hle_sceKernelIsCpuIntrEnable(CPU *c) { RET(1); }
void hle_sceKernelDcacheWritebackInvalidateRange(CPU *c) { RET(0); }
void hle_sceKernelDcacheInvalidateRange(CPU *c) { RET(0); }

// ---- mutexes (sceKernel*Mutex) and lightweight mutexes (sceKernel*LwMutex) --------------------
// A count owned by one thread. attr 0x200: the owner may lock it again (recursively); attr 0x100:
// the waiters are served by priority instead of in the order they came. A lightweight mutex keeps
// its state in the game's own work area (lock level, owner thread, attr, waiters, uid), which the
// game may read; a waiter asks for wait_bits of the count (its place in the queue: wait_mode).
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr;
	uint32_t work;  // lightweight: its work area; 0 for a mutex
	int32_t count;
	uint32_t owner;  // thread uid; 0 = unlocked
} Mutex;
static uint32_t mutex_queue;

#define ERR_MUTEX_NOT_FOUND 0x800201C3u
#define ERR_ILLEGAL_ATTR 0x80020191u
#define ERR_FPL_UNKNOWN 0x8002019Du
#define ERR_FPL_NO_MEMORY 0x800201AFu  // SCE_KERNEL_ERROR_WAIT_CAN_NOT_WAIT's neighbour: "no free block"
#define ERR_UNKNOWN_VTID 0x800201ACu

static int32_t mx_count(Mutex *m) { return m->work ? (int32_t)M32(m->work) : m->count; }
static uint32_t mx_owner(Mutex *m) { return m->work ? M32(m->work + 4) : m->owner; }
static void mx_set(Mutex *m, int32_t count, uint32_t owner) {
	if (m->work) {
		M32(m->work) = (uint32_t)count;
		M32(m->work + 4) = owner;
	}
	else {
		m->count = count;
		m->owner = owner;
	}
}
static uint32_t mx_err(Mutex *m, uint32_t mutex_err) { return m->work ? mutex_err + 7 : mutex_err; }  // the Lw codes are 7 above

static Thread *mutex_first(Mutex *m) {
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (!w->uid || w->status != TS_WAIT || w->wait_type != WT_MUTEX || w->wait_obj != m) continue;
		if (!best || ((m->attr & 0x100) ? w->prio < best->prio || (w->prio == best->prio && w->wait_mode < best->wait_mode)
		                                : w->wait_mode < best->wait_mode))
			best = w;
	}
	return best;
}
static void mutex_count_waiters(Mutex *m) {
	if (!m->work) return;
	int n = 0;
	for (int i = 0; i < MAX_THREADS; i++)
		n += threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_MUTEX && threads[i].wait_obj == m;
	M32(m->work + 12) = (uint32_t)n;
}

static uint32_t mutex_create(const char *name, uint32_t attr, int32_t init, uint32_t work) {
	Mutex *m = calloc(1, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", name);
	m->attr = attr;
	m->work = work;
	m->uid = obj_new(OBJ_MUTEX, m);
	if (work) {
		memset(MP(work), 0, 32);
		M32(work + 8) = attr;
		M32(work + 16) = m->uid;
	}
	mx_set(m, init, init > 0 ? cur->uid : 0);
	return m->uid;
}

static void mutex_delete(CPU *c, Mutex *m) {
	for (int i = 0; i < MAX_THREADS; i++)
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_MUTEX && threads[i].wait_obj == m)
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
	if (m->work) {
		M32(m->work) = 0;
		M32(m->work + 4) = (uint32_t)-1;
		M32(m->work + 16) = (uint32_t)-1;
	}
	obj_free(m->uid);
	free(m);
	RET(0);
	rt_resched();
}

// Lock: 0 = taken, 1 = must wait, else an error code in *err.
static int mutex_try(Mutex *m, int32_t n, uint32_t *err) {
	if (n <= 0 || (n > 1 && !(m->attr & 0x200))) {
		*err = ERR_ILLEGAL_COUNT;
		return 2;
	}
	int32_t have = mx_count(m);
	if (have == 0 && !mutex_first(m)) {
		mx_set(m, n, cur->uid);
		return 0;
	}
	if (have > 0 && mx_owner(m) == cur->uid) {
		if (!(m->attr & 0x200)) {
			*err = mx_err(m, 0x800201C8u);  // recursion not allowed
			return 2;
		}
		if ((int64_t)have + n > 0x7fffffff) {
			*err = mx_err(m, 0x800201C6u);  // overflow
			return 2;
		}
		mx_set(m, have + n, cur->uid);
		return 0;
	}
	return 1;
}

static void mutex_lock(CPU *c, Mutex *m, int32_t n, uint32_t timeout_ptr, bool cb) {
	if (cb) kernel_run_callbacks();
	uint32_t err = 0;
	int r = mutex_try(m, n, &err);
	if (r == 0) {
		RET(0);
		return;
	}
	if (r == 2) {
		RET(err);
		return;
	}
	cur->wait_bits = (uint32_t)n;
	cur->wait_mode = ++mutex_queue;
	cur->timeout_ptr = timeout_ptr;
	Thread *me = cur;
	(void)me;
	mutex_count_waiters(m);
	int32_t res = block_current(WT_MUTEX, m, timeout_deadline(timeout_ptr));
	RET(res);
}

static void mutex_unlock(CPU *c, Mutex *m, int32_t n) {
	if (n <= 0) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	int32_t have = mx_count(m);
	if (have == 0 || mx_owner(m) != cur->uid) {
		RET(mx_err(m, 0x800201C5u));  // not locked (by this thread)
		return;
	}
	if (n > have) {
		RET(mx_err(m, 0x800201C7u));  // underflow
		return;
	}
	have -= n;
	mx_set(m, have, have ? cur->uid : 0);
	if (!have) {
		Thread *w = mutex_first(m);
		if (w) {
			mx_set(m, (int32_t)w->wait_bits, w->uid);
			wake(w, 0);
		}
		mutex_count_waiters(m);
	}
	RET(0);
	rt_resched();
}

void hle_sceKernelCreateMutex(CPU *c) {
	if (A1 >= 0xC00) {
		RET(ERR_ILLEGAL_ATTR);
		return;
	}
	if ((int32_t)A2 < 0 || ((int32_t)A2 > 1 && !(A1 & 0x200))) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	uint32_t uid = mutex_create(rt_str(A0), A1, (int32_t)A2, 0);
	TRACE("sceKernelCreateMutex(%s, attr=%x, init=%d) -> %08x", rt_str(A0), A1, (int32_t)A2, uid);
	RET(uid);
}
void hle_sceKernelDeleteMutex(CPU *c) {
	Mutex *m = obj_get(A0, OBJ_MUTEX);
	if (!m || m->work) RET(ERR_MUTEX_NOT_FOUND);
	else mutex_delete(c, m);
}
void hle_sceKernelLockMutex(CPU *c) {
	Mutex *m = obj_get(A0, OBJ_MUTEX);
	if (!m || m->work) RET(ERR_MUTEX_NOT_FOUND);
	else mutex_lock(c, m, (int32_t)A1, A2, false);
}
void hle_sceKernelLockMutexCB(CPU *c) {
	Mutex *m = obj_get(A0, OBJ_MUTEX);
	if (!m || m->work) RET(ERR_MUTEX_NOT_FOUND);
	else mutex_lock(c, m, (int32_t)A1, A2, true);
}
void hle_sceKernelTryLockMutex(CPU *c) {
	Mutex *m = obj_get(A0, OBJ_MUTEX);
	uint32_t err = 0;
	if (!m || m->work) RET(ERR_MUTEX_NOT_FOUND);
	else {
		int r = mutex_try(m, (int32_t)A1, &err);
		RET(r == 0 ? 0 : r == 1 ? 0x800201C4u : err);
	}
}
void hle_sceKernelUnlockMutex(CPU *c) {
	Mutex *m = obj_get(A0, OBJ_MUTEX);
	if (!m || m->work) RET(ERR_MUTEX_NOT_FOUND);
	else mutex_unlock(c, m, (int32_t)A1);
}

// sceKernelCreateLwMutex(work area, name, attr, initial count, options)
void hle_sceKernelCreateLwMutex(CPU *c) {
	if (A2 >= 0x400) {
		RET(ERR_ILLEGAL_ATTR);
		return;
	}
	if ((int32_t)A3 < 0 || ((int32_t)A3 > 1 && !(A2 & 0x200))) {
		RET(ERR_ILLEGAL_COUNT);
		return;
	}
	uint32_t uid = mutex_create(rt_str(A1), A2, (int32_t)A3, A0);
	TRACE("sceKernelCreateLwMutex(%08x, %s, attr=%x, init=%d) -> %08x", A0, rt_str(A1), A2, (int32_t)A3, uid);
	RET(0);
}
static Mutex *lw_get(uint32_t work) {
	if (!work || !rt_valid(work, 32)) return NULL;
	Mutex *m = obj_get(M32(work + 16), OBJ_MUTEX);
	return m && m->work == work ? m : NULL;
}
void hle_sceKernelDeleteLwMutex(CPU *c) {
	Mutex *m = lw_get(A0);
	if (!m) RET(0x800201CAu);
	else mutex_delete(c, m);
}
void hle_sceKernelLockLwMutex(CPU *c) {
	Mutex *m = lw_get(A0);
	if (!m) RET(0x800201CAu);
	else mutex_lock(c, m, (int32_t)A1, A2, false);
}
void hle_sceKernelLockLwMutexCB(CPU *c) {
	Mutex *m = lw_get(A0);
	if (!m) RET(0x800201CAu);
	else mutex_lock(c, m, (int32_t)A1, A2, true);
}
void hle_sceKernelTryLockLwMutex(CPU *c) {
	Mutex *m = lw_get(A0);
	uint32_t err = 0;
	if (!m) RET(0x800201CAu);
	else {
		int r = mutex_try(m, (int32_t)A1, &err);
		RET(r == 0 ? 0 : r == 1 ? 0x800201CBu : err);
	}
}
void hle_sceKernelTryLockLwMutex_600(CPU *c) { hle_sceKernelTryLockLwMutex(c); }
void hle_sceKernelUnlockLwMutex(CPU *c) {
	Mutex *m = lw_get(A0);
	if (!m) RET(0x800201CAu);
	else mutex_unlock(c, m, (int32_t)A1);
}

// ---- fixed-size pools (sceKernel*Fpl): blocks of one size, taken and given back one at a time --
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr, block_size, blocks, base;
	Block *mem;
	uint8_t *used;
} Fpl;
static uint32_t fpl_queue;

static int fpl_take(Fpl *f) {
	for (uint32_t i = 0; i < f->blocks; i++)
		if (!f->used[i]) {
			f->used[i] = 1;
			return (int)i;
		}
	return -1;
}
static Thread *fpl_first(Fpl *f) {
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (!w->uid || w->status != TS_WAIT || w->wait_type != WT_FPL || w->wait_obj != f) continue;
		if (!best || ((f->attr & 0x100) ? w->prio < best->prio || (w->prio == best->prio && w->wait_mode < best->wait_mode)
		                                : w->wait_mode < best->wait_mode))
			best = w;
	}
	return best;
}

// sceKernelCreateFpl(name, partition, attr, block size, blocks, options)
void hle_sceKernelCreateFpl(CPU *c) {
	uint32_t bsize = A3, n = A4;
	if (!bsize || !n) {
		RET(0x800201BEu);  // SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE
		return;
	}
	uint32_t align = 4;
	if (A5 && rt_valid(A5, 8) && M32(A5) >= 8 && M32(A5 + 4)) align = M32(A5 + 4);
	uint32_t stride = (bsize + align - 1) & ~(align - 1);
	Fpl *f = calloc(1, sizeof *f);
	snprintf(f->name, sizeof f->name, "%s", rt_str(A0));
	f->attr = A2;
	f->block_size = stride;
	f->blocks = n;
	f->mem = block_alloc(stride * n + align, (A2 & 0x4000) ? 1 : 0, 0, f->name);
	if (!f->mem) {
		free(f);
		RET(ERR_NO_MEMORY);
		return;
	}
	f->base = (f->mem->addr + align - 1) & ~(align - 1);
	f->used = calloc(n, 1);
	f->uid = obj_new(OBJ_FPL, f);
	TRACE("sceKernelCreateFpl(%s, attr=%x, %u x %u) -> %08x at %08x", f->name, A2, n, bsize, f->uid, f->base);
	RET(f->uid);
}
void hle_sceKernelDeleteFpl(CPU *c) {
	Fpl *f = obj_get(A0, OBJ_FPL);
	if (!f) {
		RET(ERR_FPL_UNKNOWN);
		return;
	}
	for (int i = 0; i < MAX_THREADS; i++)
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_FPL && threads[i].wait_obj == f)
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
	block_free(f->mem);
	free(f->used);
	obj_free(f->uid);
	free(f);
	RET(0);
	rt_resched();
}
static void fpl_allocate(CPU *c, bool cb) {
	if (cb) kernel_run_callbacks();
	Fpl *f = obj_get(A0, OBJ_FPL);
	if (!f) {
		RET(ERR_FPL_UNKNOWN);
		return;
	}
	int i = fpl_first(f) ? -1 : fpl_take(f);
	if (i >= 0) {
		M32(A1) = f->base + (uint32_t)i * f->block_size;
		RET(0);
		return;
	}
	cur->wait_out = A1;
	cur->wait_mode = ++fpl_queue;
	cur->timeout_ptr = A2;
	RET(block_current(WT_FPL, f, timeout_deadline(A2)));
}
void hle_sceKernelAllocateFpl(CPU *c) { fpl_allocate(c, false); }
void hle_sceKernelAllocateFplCB(CPU *c) { fpl_allocate(c, true); }
void hle_sceKernelTryAllocateFpl(CPU *c) {
	Fpl *f = obj_get(A0, OBJ_FPL);
	if (!f) {
		RET(ERR_FPL_UNKNOWN);
		return;
	}
	int i = fpl_first(f) ? -1 : fpl_take(f);
	if (i < 0) {
		RET(0x800201AFu);  // SCE_KERNEL_ERROR_NO_MEMORY for a pool: no free block
		return;
	}
	M32(A1) = f->base + (uint32_t)i * f->block_size;
	RET(0);
}
void hle_sceKernelFreeFpl(CPU *c) {
	Fpl *f = obj_get(A0, OBJ_FPL);
	if (!f) {
		RET(ERR_FPL_UNKNOWN);
		return;
	}
	uint32_t a = A1;
	if (a < f->base || a >= f->base + f->blocks * f->block_size || (a - f->base) % f->block_size || !f->used[(a - f->base) / f->block_size]) {
		RET(ERR_ILLEGAL_MEMBLOCK);
		return;
	}
	uint32_t i = (a - f->base) / f->block_size;
	Thread *w = fpl_first(f);
	if (w) {  // straight to the first waiter
		M32(w->wait_out) = a;
		wake(w, 0);
	}
	else f->used[i] = 0;
	RET(0);
	rt_resched();
}
void hle_sceKernelReferFplStatus(CPU *c) {
	Fpl *f = obj_get(A0, OBJ_FPL);
	if (!f) {
		RET(ERR_FPL_UNKNOWN);
		return;
	}
	uint32_t p = A1, free_n = 0;
	for (uint32_t i = 0; i < f->blocks; i++) free_n += !f->used[i];
	int waiting = 0;
	for (int i = 0; i < MAX_THREADS; i++)
		waiting += threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_FPL && threads[i].wait_obj == f;
	M32(p + 0) = 0x38;
	rt_write_cstr(p + 4, 32, f->name);
	M32(p + 36) = f->attr;
	M32(p + 40) = f->block_size;
	M32(p + 44) = f->blocks;
	M32(p + 48) = free_n;
	M32(p + 52) = (uint32_t)waiting;
	RET(0);
}

// ---- virtual timers (sceKernel*VTimer): a clock that runs only while started --------------------
typedef struct {
	uint32_t uid;
	char name[32];
	bool running;
	uint64_t base_us;  // its time when last started (or set)
	uint64_t started_us;
} VTimer;

static uint64_t vtimer_now(VTimer *v) { return v->base_us + (v->running ? rt_now_us() - v->started_us : 0); }

void hle_sceKernelCreateVTimer(CPU *c) {
	VTimer *v = calloc(1, sizeof *v);
	snprintf(v->name, sizeof v->name, "%s", rt_str(A0));
	v->uid = obj_new(OBJ_VTIMER, v);
	RET(v->uid);
}
void hle_sceKernelDeleteVTimer(CPU *c) {
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	if (!v) {
		RET(ERR_UNKNOWN_VTID);
		return;
	}
	obj_free(v->uid);
	free(v);
	RET(0);
}
void hle_sceKernelStartVTimer(CPU *c) {
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	if (!v) {
		RET(ERR_UNKNOWN_VTID);
		return;
	}
	if (v->running) {
		RET(1);
		return;
	}
	v->running = true;
	v->started_us = rt_now_us();
	RET(0);
}
void hle_sceKernelStopVTimer(CPU *c) {
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	if (!v) {
		RET(ERR_UNKNOWN_VTID);
		return;
	}
	if (!v->running) {
		RET(0);
		return;
	}
	v->base_us = vtimer_now(v);
	v->running = false;
	RET(1);
}
void hle_sceKernelGetVTimerTime(CPU *c) {
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	if (!v) {
		RET(ERR_UNKNOWN_VTID);
		return;
	}
	uint64_t t = vtimer_now(v);
	M32(A1) = (uint32_t)t;
	M32(A1 + 4) = (uint32_t)(t >> 32);
	RET(0);
}
void hle_sceKernelGetVTimerTimeWide(CPU *c) {
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	RET64(v ? vtimer_now(v) : (uint64_t)-1);
}
void hle_sceKernelSetVTimerTime(CPU *c) {  // (uid, u64 *time): sets it, the old one comes back in *time
	VTimer *v = obj_get(A0, OBJ_VTIMER);
	if (!v) {
		RET(ERR_UNKNOWN_VTID);
		return;
	}
	uint64_t old = vtimer_now(v), t = M32(A1) | (uint64_t)M32(A1 + 4) << 32;
	v->base_us = t;
	v->started_us = rt_now_us();
	M32(A1) = (uint32_t)old;
	M32(A1 + 4) = (uint32_t)(old >> 32);
	RET(0);
}

// ---- the system clock in other forms -------------------------------------------------------------
void hle_sceKernelGetSystemTime(CPU *c) {  // (SceKernelSysClock *): a u64 of microseconds
	uint64_t t = rt_clock_vblanks ? rt_vblank_time_us(rt_clock_vblank) : rt_now_us();
	M32(A0) = (uint32_t)t;
	M32(A0 + 4) = (uint32_t)(t >> 32);
	RET(0);
}
void hle_sceKernelSysClock2USec(CPU *c) {  // (const SceKernelSysClock *, u32 *sec, u32 *usec)
	uint64_t t = M32(A0) | (uint64_t)M32(A0 + 4) << 32;
	if (A1) M32(A1) = (uint32_t)(t / 1000000);
	if (A2) M32(A2) = (uint32_t)(t % 1000000);
	RET(0);
}
void hle_sceKernelSysClock2USecWide(CPU *c) {  // (u64 clock in a0:a1, u32 *sec, u32 *usec)
	uint64_t t = A0 | (uint64_t)A1 << 32;
	if (A2) M32(A2) = (uint32_t)(t / 1000000);
	if (A3) M32(A3) = (uint32_t)(t % 1000000);
	RET(0);
}

// ---- more of the threads -----------------------------------------------------------------------
void hle_sceKernelExitDeleteThread(CPU *c) {
	// Its stack and id are kept (the thread is running on them): it ends as a dormant thread nobody
	// will start again.
	thread_exit(cur, A0);
}
void hle_sceKernelTerminateThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status != TS_DORMANT) {
		t->status = TS_DORMANT;
		t->wait_type = WT_NONE;
		t->exit_status = 0x800201ACu;  // SCE_KERNEL_ERROR_THREAD_TERMINATED
		for (int i = 0; i < MAX_THREADS; i++) {
			Thread *w = &threads[i];
			if (w->uid && w->status == TS_WAIT && w->wait_type == WT_THREADEND && w->wait_obj == t)
				wake(w, (int32_t)t->exit_status);
		}
	}
	RET(0);
}
void hle_sceKernelGetThreadExitStatus(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) RET(ERR_UNKNOWN_THID);
	else if (t->status != TS_DORMANT) RET(ERR_NOT_DORMANT);
	else RET(t->exit_status);
}
void hle_sceKernelCancelWakeupThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	uint32_t n = t->wakeups;
	t->wakeups = 0;
	RET(n);
}
static uint32_t stack_free(Thread *t) {  // the untouched bytes at the bottom (the stack is filled with 0xFF at creation)
	uint32_t a = t->stack_addr + 4, end = t->stack_addr + t->stack_size;
	while (a < end && M8(a) == 0xFF) a++;
	return a - t->stack_addr;
}
void hle_sceKernelCheckThreadStack(CPU *c) { RET(stack_free(cur)); }
void hle_sceKernelGetThreadStackFreeSize(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) RET(ERR_UNKNOWN_THID);
	else RET(stack_free(t));
}
void hle_sceKernelCpuResumeIntrWithSync(CPU *c) { RET(0); }

// =====================================================================================
// Kernel_Library memory helpers, stack extension, message boxes
// =====================================================================================

void hle_sceKernelMemset(CPU *c) {
	if (A2 && rt_valid(A0, A2)) memset(MP(A0), (int)(A1 & 0xFF), A2);
	RET(A0);
}
void hle_sceKernelMemcpy(CPU *c) {
	if (A2 && rt_valid(A0, A2) && rt_valid(A1, A2)) memmove(MP(A0), MP(A1), A2);
	RET(A0);
}
void hle_sceKernelSetCompiledSdkVersion603_605(CPU *c) { RET(0); }
void hle_sceKernelIcacheInvalidateRange(CPU *c) { RET(0); }

// sceKernelExtendThreadStack(size, func, arg): func(arg) on a fresh stack of that size; its result.
void hle_sceKernelExtendThreadStack(CPU *c) {
	uint32_t size = A0 < 0x200 ? 0x200 : A0, func = A1, arg = A2;
	uint32_t stack = rt_alloc_user(size, true, "extended stack");
	if (!stack) {
		RET(ERR_NO_MEMORY);
		return;
	}
	size = align_up(size, 0x100);
	memset(MP(stack), 0xFF, size);  // the free stack is measured on it while it is the thread's
	Thread *t = cur;
	uint32_t old_addr = t->stack_addr, old_size = t->stack_size;
	t->stack_addr = stack;
	t->stack_size = size;
	CPU saved = *c;
	c->r[4] = arg;
	c->r[29] = (stack + size - 0x10) & ~0xFu;
	c->r[31] = 0;
	rt_call(c, func);
	uint32_t v0 = c->r[2], v1 = c->r[3];
	*c = saved;
	c->r[3] = v1;
	t->stack_addr = old_addr;
	t->stack_size = old_size;
	rt_free_user(stack);
	RET(v0);
}

// Message boxes: the messages are the game's own packets (u32 next, u8 priority...); the box keeps
// their addresses in order (by priority with attribute 0x400, else first in first out), and a
// receiver waiting gets the next one sent straight away.
#define ERR_UNKNOWN_MBXID 0x8002019Bu
#define ERR_MBOX_NOMSG 0x800201B2u
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr;
	uint32_t *msgs;
	int n, cap;
} Mbx;
static uint32_t mbx_queue;

static Thread *mbx_first(Mbx *m) {
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (!w->uid || w->status != TS_WAIT || w->wait_type != WT_MBX || w->wait_obj != m) continue;
		if (!best || ((m->attr & 0x100) ? w->prio < best->prio || (w->prio == best->prio && w->wait_mode < best->wait_mode)
		                                : w->wait_mode < best->wait_mode))
			best = w;
	}
	return best;
}

void hle_sceKernelCreateMbx(CPU *c) {
	Mbx *m = calloc(1, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", rt_str(A0));
	m->attr = A1;
	m->uid = obj_new(OBJ_MBX, m);
	TRACE("sceKernelCreateMbx(%s, attr=%x) -> %08x", m->name, A1, m->uid);
	RET(m->uid);
}

void hle_sceKernelDeleteMbx(CPU *c) {
	Mbx *m = obj_get(A0, OBJ_MBX);
	if (!m) {
		RET(ERR_UNKNOWN_MBXID);
		return;
	}
	for (int i = 0; i < MAX_THREADS; i++)
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_MBX && threads[i].wait_obj == m)
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
	obj_free(m->uid);
	free(m->msgs);
	free(m);
	RET(0);
	rt_resched();
}

void hle_sceKernelSendMbx(CPU *c) {
	Mbx *m = obj_get(A0, OBJ_MBX);
	if (!m) {
		RET(ERR_UNKNOWN_MBXID);
		return;
	}
	uint32_t msg = A1;
	Thread *w = mbx_first(m);
	if (w) {
		if (w->wait_out) M32(w->wait_out) = msg;
		wake(w, 0);
		RET(0);
		rt_resched();
		return;
	}
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 16;
		m->msgs = realloc(m->msgs, m->cap * sizeof *m->msgs);
	}
	int at = m->n;
	if (m->attr & 0x400) {  // by priority: before the first one with a higher number
		uint8_t pr = rt_valid(msg + 4, 1) ? M8(msg + 4) : 0;
		for (at = 0; at < m->n && M8(m->msgs[at] + 4) <= pr; at++) {}
	}
	memmove(m->msgs + at + 1, m->msgs + at, (m->n - at) * sizeof *m->msgs);
	m->msgs[at] = msg;
	m->n++;
	RET(0);
}

static bool mbx_take(Mbx *m, uint32_t out) {
	if (!m->n) return false;
	if (out) M32(out) = m->msgs[0];
	memmove(m->msgs, m->msgs + 1, --m->n * sizeof *m->msgs);
	return true;
}

static void mbx_receive(CPU *c, bool cb) {
	Mbx *m = obj_get(A0, OBJ_MBX);
	if (!m) {
		RET(ERR_UNKNOWN_MBXID);
		return;
	}
	if (cb) kernel_run_callbacks();
	if (mbx_take(m, A1)) {
		RET(0);
		return;
	}
	cur->wait_out = A1;
	cur->wait_mode = ++mbx_queue;
	cur->timeout_ptr = A2;
	RET(block_current(WT_MBX, m, timeout_deadline(A2)));
}
void hle_sceKernelReceiveMbx(CPU *c) { mbx_receive(c, false); }
void hle_sceKernelReceiveMbxCB(CPU *c) { mbx_receive(c, true); }

void hle_sceKernelPollMbx(CPU *c) {
	Mbx *m = obj_get(A0, OBJ_MBX);
	if (!m) {
		RET(ERR_UNKNOWN_MBXID);
		return;
	}
	RET(mbx_take(m, A1) ? 0 : ERR_MBOX_NOMSG);
}
