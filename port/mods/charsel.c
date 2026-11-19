// Mod: the character select's Custom step (Default / Customize 1..3, after the color) is never
// shown: the color chosen, the character is chosen with Default, as if the player had confirmed
// Default on that panel (no panel, no sound of its own, nothing skipped at speed).
//
// Every mode's character select keeps, for each player, an object of its steps (vtable 0x08a73e18):
// +120 the step it is on (1 the characters, 2 Select Form, 3 Select Color, 4 Custom, 5 chosen),
// +124 the step it goes to next (FUN_08a064b0 sets it). FUN_08a0ab08(select, action, steps) moves
// a player on: action 0 confirm, 1 back. On step 3 a confirm sets 4 and opens the Custom panel
// (select+0x98); on step 4 a confirm closes it, sets 5, keeps the custom choice (Default, where its
// cursor starts) and opens what follows. Here a confirm on step 3 is followed at once by that
// confirm on step 4 (the panel opened and closed in the same frame, never drawn); and a step back
// that would land on Custom (from 5) is followed by its back to the color.
// DBZ_MOD_CUSTOM=0: the step as the game has it.
#include <stdlib.h>

#include "funcs.h"
#include "rt.h"

#define STEP 120
#define NEXT 124
#define STEP_COLOR 3
#define STEP_CUSTOM 4
#define STEP_CHOSEN 5

static bool on(void) {
	static int v = -1;
	if (v < 0) v = !getenv("DBZ_MOD_CUSTOM") || atoi(getenv("DBZ_MOD_CUSTOM")) != 0;
	return v;
}

static void call(CPU *c, uint32_t select, uint32_t action, uint32_t steps) {
	c->r[4] = select, c->r[5] = action, c->r[6] = steps;
	f_08a0ab08_orig(c);
}

void f_08a0ab08(CPU *c) {
	uint32_t select = c->r[4], action = c->r[5], steps = c->r[6];
	if (!on() || !steps) {
		f_08a0ab08_orig(c);
		return;
	}
	(void)action;
	int32_t step = (int32_t)M32(steps + STEP);
	f_08a0ab08_orig(c);
	int32_t next = (int32_t)M32(steps + NEXT);
	if (next != STEP_CUSTOM || (step != STEP_COLOR && step != STEP_CHOSEN)) return;
	// on the Custom step's own terms: confirmed (Default) going on, its back going back
	M32(steps + STEP) = STEP_CUSTOM;
	call(c, select, step == STEP_COLOR ? 0 : 1, steps);
	M32(steps + STEP) = (uint32_t)step;
	static int logged;
	if (!logged++) rt_log("charsel: the Custom step passed by (Default)");
}
