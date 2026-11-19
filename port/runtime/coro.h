// Coroutines for PSP threads: ucontext natively, Emscripten fibers (Asyncify) on the web.
#pragma once
#include <stddef.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/fiber.h>
typedef struct {
	emscripten_fiber_t f;
	void *cstack, *astack;
} Coro;
#define CORO_CSTACK (1u << 20)
#define CORO_ASTACK (512u << 10)

// The C stacks must be 16-byte aligned: the compiler assumes an aligned stack pointer, and
// malloc only guarantees 8 bytes here. A stack ending at an address = 8 (mod 16) silently
// corrupted locals of the game threads (exploded 3D, broken movie frames), depending on where
// the heap happened to place it (even the length of the page URL changed that).
static inline void *coro_alloc(size_t n) { return aligned_alloc(16, n); }
static inline void coro_init_main(Coro *c) {
	c->astack = coro_alloc(CORO_ASTACK);
	emscripten_fiber_init_from_current_context(&c->f, c->astack, CORO_ASTACK);
}
// (Re)initializes c to run fn(arg) on its own stack when switched to.
static inline void coro_init(Coro *c, void (*fn)(void *), void *arg) {
	if (!c->cstack) c->cstack = coro_alloc(CORO_CSTACK);
	if (!c->astack) c->astack = coro_alloc(CORO_ASTACK);
	emscripten_fiber_init(&c->f, fn, arg, c->cstack, CORO_CSTACK, c->astack, CORO_ASTACK);
}
static inline void coro_switch(Coro *from, Coro *to) { emscripten_fiber_swap(&from->f, &to->f); }

#else
#include <sys/mman.h>
#include <ucontext.h>
typedef struct {
	ucontext_t ctx;
	void *stack;
} Coro;
#define CORO_STACK (4u << 20)

static inline void coro_init_main(Coro *c) { (void)c; }
static void coro_entry_trampoline(int hi, int lo, int ahi, int alo) {
	void (*fn)(void *) = (void (*)(void *))(((uintptr_t)(unsigned)hi << 32) | (unsigned)lo);
	void *arg = (void *)(((uintptr_t)(unsigned)ahi << 32) | (unsigned)alo);
	fn(arg);
}
static inline void coro_init(Coro *c, void (*fn)(void *), void *arg) {
	if (!c->stack) {
		c->stack = mmap(NULL, CORO_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
		mprotect(c->stack, 4096, PROT_NONE);  // guard page
	}
	getcontext(&c->ctx);
	c->ctx.uc_stack.ss_sp = c->stack;
	c->ctx.uc_stack.ss_size = CORO_STACK;
	c->ctx.uc_link = NULL;
	uintptr_t f = (uintptr_t)fn, a = (uintptr_t)arg;
	makecontext(&c->ctx, (void (*)(void))coro_entry_trampoline, 4, (int)(f >> 32), (int)(f & 0xFFFFFFFFu),
	            (int)(a >> 32), (int)(a & 0xFFFFFFFFu));
}
static inline void coro_switch(Coro *from, Coro *to) { swapcontext(&from->ctx, &to->ctx); }
#endif
