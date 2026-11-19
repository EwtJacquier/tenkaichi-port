// Code overlays: code the game reads in at run time over one place in memory (several of them,
// one at a time), each recompiled apart (tools/recomp.py --overlay) into generated/ov_<N>.c. A call
// into that place goes to the functions of the overlay whose bytes are there now (rt_lookup).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

typedef struct {
	uint32_t addr;
	RecompFunc fn;
} OverlayFunc;

typedef struct {
	uint32_t number, base, size;
	const uint8_t *sig;  // its first bytes at base (as on the disc)
	uint32_t sig_len;
	const OverlayFunc *table;  // sorted by address
	size_t count;
} Overlay;

extern const Overlay *const rt_overlays[];  // generated/overlays.c, NULL-terminated
