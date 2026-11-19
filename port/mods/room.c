// Mod: Multiplayer's "Create New Room" makes the room at once, Free Battle without custom characters:
// the panel of its choices (Mode: Battle Selection / Free Battle, Use Custom Character: Yes / No) is
// never answered by the player. The panel (overlay 55, made by FUN_08ab0b30, vtable 0x08abcbc8 at
// +0x40) keeps the cursor row at +0xa0, its choices at +0xa4 (0 Free Battle, 1 Battle Selection) and
// +0xa5 (custom characters), +0xa6 set while it shows; its START makes the room with
// FUN_088115f8(FUN_08811a58(), 4 Free Battle / 2 Battle Selection, custom), plays the confirm sound
// and sets +0x9c to 3, which the Select Room screen takes as the room made (2: cancelled). Here, as
// soon as it opens, the same is done for the player (before the frame it would show in is drawn).
// DBZ_MOD_ROOM=0: the panel as the game has it.
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"

void o55_08ab0b30(CPU *c);
void o55_08ab0b30_orig(CPU *c);

#define PANEL_VTABLE 0x08abcbc8u
#define MODE_FREE_BATTLE 4

static uint32_t panel;  // the Create New Room panel, once made
static bool answered;   // this opening of it answered

static bool on(void) {
	static int v = -1;
	if (v < 0) v = !getenv("DBZ_MOD_ROOM") || atoi(getenv("DBZ_MOD_ROOM")) != 0;
	return v;
}

void o55_08ab0b30(CPU *c) {
	uint32_t obj = c->r[4];
	o55_08ab0b30_orig(c);
	panel = obj, answered = false;
	if (getenv("DBZ_ROOM_LOG")) rt_log("room: panel made at %08x (vtable %08x)", obj, M32(obj + 0x40));
}

// The panel looked for by its vtable in the heap (its maker is reached through a pointer the hook
// above does not see), every 15 frames while overlay 55 (Multiplayer) is loaded.
static void find_panel(void) {
	static uint64_t last;
	uint64_t now = rt_vblank_count();
	if (now - last < 15) return;
	last = now;
	for (uint32_t a = 0x08c00000u; a < 0x09f00000u; a += 4)
		if (M32(a) == PANEL_VTABLE && M32(a + 4) == 0x08abd450u) {  // +0x40 and +0x44 of the panel
			panel = a - 0x40, answered = false;
			return;
		}
}

// Every frame, before the scene runs (mods/boot.c).
void dbz_room_frame(CPU *c) {
	if (!on()) return;
	if (!panel && (rt_overlay() & 0xffff) == 55) find_panel();
	if (!panel) return;
	static int dbg = -1;
	if (dbg < 0) dbg = getenv("DBZ_ROOM_LOG") != NULL;
	if (dbg && rt_vblank_count() % 30 == 0) rt_log("room: vt %08x a6 %d 9c %d a0 %d state %d", M32(panel + 0x40), M8(panel + 0xa6), M32(panel + 0x9c), M32(panel + 0xa0), M32(panel + 0x48));
	if (M32(panel + 0x40) != PANEL_VTABLE) {  // gone
		panel = 0;
		return;
	}
	if (!M8(panel + 0xa6)) {  // closed: the next opening is answered again
		answered = false;
		return;
	}
	// its opening (state 0, +0x48; +0x50 the next one) is passed by: the next frame takes buttons
	if (!answered && M32(panel + 0x48) == 0 && M32(panel + 0x50) == 0) M32(panel + 0x50) = 1;
	// its own state (+0x48: 0 opening, 1 taking the player's buttons): its opening clears +0x9c
	if (answered || M32(panel + 0x9c) != 0 || M32(panel + 0x48) != 1) return;
	answered = true;
	static CPU saved;
	memcpy(&saved, c, sizeof saved);
	M32(panel + 0xa0) = 0, M8(panel + 0xa4) = 0, M8(panel + 0xa5) = 0;
	f_08811a58(c);
	c->r[4] = c->r[2], c->r[5] = MODE_FREE_BATTLE, c->r[6] = 0;
	f_088115f8(c);
	bool made = c->r[2] != 0;
	if (made) {
		c->r[4] = 1, c->r[5] = 6;  // the confirm sound
		f_0881b170(c);
		M32(panel + 0x9c) = 3;
	}
	memcpy(c, &saved, sizeof saved);
	rt_log("room: Create New Room answered (Free Battle, no custom characters)%s", made ? "" : ": the room was not made");
}
