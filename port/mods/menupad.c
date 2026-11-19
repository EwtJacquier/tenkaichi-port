// Mod: the analog stick moves through every menu too (the pause menu in a battle included).
//
// Each frame the game turns each pad into its own buttons for the menus (FUN_089c7008: the pad at
// 0x08b4e990 + 88 * n; its held buttons +44, those pressed this frame +48, then held-to-repeat),
// asking FUN_0881a1f0(pad, PSP button mask) whether each button is held. Asked from there for a
// d-pad direction, the stick (its position, normalized: +0 x, +4 y, -1..1) answers too, so the
// stick presses, holds and repeats like the d-pad. A battle reads the stick and the d-pad by
// themselves (its only use of the menu buttons: START to pause), so they stay apart there.
// DBZ_MOD_MENUS=0 keeps the original.
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"

#define PADS 0x08B4E990u
#define PAD_SIZE 88u

static bool on(void) {
	static int v = -1;
	if (v < 0) v = !getenv("DBZ_MOD_MENUS") || atoi(getenv("DBZ_MOD_MENUS")) != 0;
	return v;
}

static float pad_f(uint32_t a) {
	float f;
	uint32_t u = M32(a);
	memcpy(&f, &u, 4);
	return f;
}

void f_0881a1f0(CPU *c) {
	uint32_t pad = c->r[4], mask = c->r[5], ra = c->r[31];
	f_0881a1f0_orig(c);
	if (c->r[2] || !on() || ra < 0x089C7008u || ra >= 0x089C7300u || pad > 3) return;
	uint32_t p = PADS + pad * PAD_SIZE;
	float x = pad_f(p), y = pad_f(p + 4);
	const float t = 0.5f;
	bool held = (mask == 0x10 && y < -t) || (mask == 0x40 && y > t) || (mask == 0x80 && x < -t) || (mask == 0x20 && x > t);
	if (held) c->r[2] = 1;
}
