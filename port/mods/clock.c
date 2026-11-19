// Mod: no clock question in Multiplayer. Entering it, the game asks (on a PSP-2000/3000) whether
// to lower the clock to 222 MHz for PSP-1000 players; the clock means nothing here. The question's
// window (overlay 55, FUN_08aaddfc) is answered "No" the moment it opens: its "done?" check
// (FUN_08a3b278) says yes and its answer (FUN_08a3b2c4) is No (2), so the game goes its own "No"
// way (333 MHz kept, the choice noted). DBZ_MOD_MENUS=0 asks as the game does.
#include <stdlib.h>

#include "funcs.h"
#include "rt.h"

static bool on(void) {
	static int v = -1;
	if (v < 0) v = !getenv("DBZ_MOD_MENUS") || atoi(getenv("DBZ_MOD_MENUS")) != 0;
	return v;
}

// FUN_08a3b278(dialog): the window's "an answer was given" flag (+136), cleared as it is read.
void f_08a3b278(CPU *c) {
	if (on() && c->r[31] == 0x08AADE18u) {  // the clock question
		c->r[2] = 1;
		return;
	}
	f_08a3b278_orig(c);
}

// FUN_08a3b2c4(dialog): the answer (+128): 1 Yes, 2 No.
void f_08a3b2c4(CPU *c) {
	if (on() && c->r[31] == 0x08AADE28u) {
		c->r[2] = 2;
		return;
	}
	f_08a3b2c4_orig(c);
}
