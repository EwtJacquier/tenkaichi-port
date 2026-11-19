#ifndef __EMSCRIPTEN__
// Sampling profiler (DBZ_PROFILE=1): SIGPROF records the interrupted instruction pointer;
// at exit the hottest functions are printed (names via dladdr; needs -rdynamic).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#include "rt.h"

#define MAX_SAMPLES (1 << 20)
static void *samples[MAX_SAMPLES], *callers[MAX_SAMPLES];
static volatile int nsamples;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
	(void)sig;
	(void)si;
	ucontext_t *uc = uc_;
	static long from = -2;  // DBZ_PROFILE_FROM=N: samples only from vblank N on
	if (from == -2) from = getenv("DBZ_PROFILE_FROM") ? atol(getenv("DBZ_PROFILE_FROM")) : -1;
	if (from >= 0 && rt_vblank_count() < (uint64_t)from) return;
	if (nsamples < MAX_SAMPLES) {
		// the word on top of the stack: the caller of a leaf function (memcpy, memset...)
		callers[nsamples] = *(void **)uc->uc_mcontext.gregs[REG_RSP];
		samples[nsamples++] = (void *)uc->uc_mcontext.gregs[REG_RIP];
	}
}

typedef struct {
	const char *name;
	int count;
} Hit;

static int by_count(const void *a, const void *b) { return ((const Hit *)b)->count - ((const Hit *)a)->count; }

static void report(void) {
	Hit *hits = calloc((size_t)nsamples + 1, sizeof(Hit));
	int nh = 0;
	for (int i = 0; i < nsamples; i++) {
		Dl_info info;
		const char *name = dladdr(samples[i], &info) && info.dli_sname ? info.dli_sname : (info.dli_fname ? info.dli_fname : "?");
		int j;
		for (j = 0; j < nh; j++) {
			if (hits[j].name == name || !strcmp(hits[j].name, name)) break;
		}
		if (j == nh) hits[nh++] = (Hit){name, 0};
		hits[j].count++;
	}
	qsort(hits, (size_t)nh, sizeof(Hit), by_count);
	// Addresses without a dynamic symbol (static functions): offsets for addr2line.
	FILE *raw = fopen("build/profile_unnamed.txt", "w");
	for (int i = 0; raw && i < nsamples; i++) {
		Dl_info info;
		if (dladdr(samples[i], &info) && !info.dli_sname && info.dli_fname && strstr(info.dli_fname, "dbztag"))
			fprintf(raw, "%lx\n", (unsigned long)((char *)samples[i] - (char *)info.dli_fbase));
	}
	if (raw) fclose(raw);
	// Every sample in our binary (offsets for addr2line: where inside a big function the time goes).
	FILE *all = fopen("build/profile_all.txt", "w");
	for (int i = 0; all && i < nsamples; i++) {
		Dl_info info;
		if (dladdr(samples[i], &info) && info.dli_fname && strstr(info.dli_fname, "dbztag"))
			fprintf(all, "%lx\n", (unsigned long)((char *)samples[i] - (char *)info.dli_fbase));
	}
	if (all) fclose(all);
	// Samples inside the C library: who called it (offsets in our binary, for addr2line).
	FILE *lc = fopen("build/profile_libc_callers.txt", "w");
	for (int i = 0; lc && i < nsamples; i++) {
		Dl_info info, ci;
		if (dladdr(samples[i], &info) && info.dli_fname && strstr(info.dli_fname, "libc.so") && dladdr(callers[i], &ci) &&
		    ci.dli_fname && strstr(ci.dli_fname, "dbztag"))
			fprintf(lc, "%lx %s\n", (unsigned long)((char *)callers[i] - (char *)ci.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	}
	if (lc) fclose(lc);
	fprintf(stderr, "PROFILE: %d samples\n", nsamples);
	for (int i = 0; i < nh && i < 30; i++) {
		fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * hits[i].count / nsamples, hits[i].name);
	}
}

__attribute__((constructor)) static void profile_init(void) {
	if (!getenv("DBZ_PROFILE")) return;
	struct sigaction sa = {0};
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval it = {{0, 2000}, {0, 2000}};
	setitimer(ITIMER_PROF, &it, NULL);
	atexit(report);
}
#endif
