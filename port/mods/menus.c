// Mod: the title's menu (New Game / Load Game / Install Data) is never shown: START on the title
// starts the game, loading the save when there is one (the site gives every player one, a copy of
// the main save with their name), else a new game. When the title menu opens (overlay 12,
// FUN_08a827cc(menu): cursor +148, 0 New Game, 1 Load Game, 2 Install Data) it gets the cursor set
// and the game's confirm button pressed, and its few frames run unseen. DBZ_MOD_MENUS=0: the menu
// as the game has it.
#include <stdlib.h>
#include <sys/stat.h>

#include "funcs.h"
#include "rt.h"

void o12_08a827cc(CPU *c);
void o12_08a827cc_orig(CPU *c);

#define PAD_PRESSED 0x08B4E9C0u   // the game's pad: buttons pressed this frame (its own bits)
#define CONFIRM_MASK 0x08A793D4u  // the game's confirm button, in those bits (X or O, by region)

static bool on(void) {
	static int v = -1;
	if (v < 0) v = !getenv("DBZ_MOD_MENUS") || atoi(getenv("DBZ_MOD_MENUS")) != 0;
	return v;
}

static bool save_exists(void) {
	char path[512];
	const char *ms0 = getenv("DBZ_MS0");
	snprintf(path, sizeof path, "%s/PSP/SAVEDATA/ULUS10537DAT0/DATA.BIN", ms0 ? ms0 : "build/ms0");
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

int dbz_scene(void);  // mods/boot.c: the current scene

// FUN_08a827cc(menu): the title menu's step, every frame it shows; the cursor set and the confirm
// button pressed before it looks at the pad.
void o12_08a827cc(CPU *c) {
	if (on()) {
		static int logged_in = -1;
		bool load = save_exists();
		if (logged_in != dbz_scene()) {
			logged_in = dbz_scene();
			rt_log("menus: the title menu skipped: %s", load ? "Load Game" : "New Game (no save yet)");
		}
		M32(c->r[4] + 148) = load ? 1 : 0;
		M32(PAD_PRESSED) |= M32(CONFIRM_MASK);
		// the menu opening, its choice flashing and closing, the title fading: run unseen at full
		// speed (nothing is shown in turbo) until the next scene (mods/boot.c turns it off there)
		rt_turbo(true);
	}
	o12_08a827cc_orig(c);
}

// The Option menu (overlay 20) is a list of 7 items (Volume Settings, Controller, Camera, Install
// Data, Change Player Name, Auto Save, Save) shown 4 rows at a time: its list object has the cursor
// row at +112, the item in each row at +116.. (5 rows, the last one hidden), the items at +136..
// (+21: the item is drawn). FUN_08a8bc08(list, 1 up / 0 down) moves the cursor and turns the rows at
// the edges. Here the rows never turn and the cursor goes round the first 3 items, the others never
// drawn: install data means nothing in the port, the name is the account's, and the game saves by
// itself (Auto Save stays on, the game's default).
void o20_08a8bc08(CPU *c);
void o20_08a8bc08_orig(CPU *c);
void o20_08a8c338(CPU *c);
void o20_08a8c338_orig(CPU *c);

#define OPTION_ITEMS 3

static void option_rows(uint32_t list) {
	for (uint32_t i = OPTION_ITEMS; i < 7; i++) {
		uint32_t item = M32(list + 136 + 4 * i);
		if (item) M8(item + 21) = 0;
	}
}

void o20_08a8bc08(CPU *c) {
	uint32_t list = c->r[4];
	if (on() && M32(list + 116) == 0) {  // the rows as they start (never turned)
		int32_t row = (int32_t)M32(list + 112);
		if (c->r[5] == 0 && row >= OPTION_ITEMS - 1) M32(list + 112) = (uint32_t)-1;  // down on the last: the first
		if (c->r[5] == 1 && row <= 0) M32(list + 112) = OPTION_ITEMS;               // up on the first: the last
	}
	o20_08a8bc08_orig(c);
	if (on()) option_rows(list);
}

// FUN_08a8c338(list): the item under the cursor, asked every frame the menu shows (its help line).
void o20_08a8c338(CPU *c) {
	if (on()) option_rows(c->r[4]);
	o20_08a8c338_orig(c);
}

// The main menu (overlay 16) keeps Free Battle and Multiplayer only: Dragon Walker, Battle
// Selection, Customize, Training and Option are gone. Its list is the Option menu's kind (7 items
// shown 4 rows at a time, a fifth row hidden: cursor row +112, the item of each row +116.., the
// items +136.., +21 drawn), round: FUN_08a83508(list, n) turns its rows by n (the first row's item
// +116 goes round 0..6), FUN_08a843c8(list, 1 up / 0 down) moves the cursor and turns the rows at
// the edges. As the menu is made it turns its rows to the item of the mode the player comes back
// from (the scene manager's previous scene, *(0x08a7ea50)+8: 6 Free Battle -> 2, 8 Multiplayer -> 3,
// anything but a mode: no turn); here it always turns them to Free Battle (rows: Free Battle,
// Multiplayer, then the 3 after them, never drawn), with the cursor on Multiplayer when the player
// comes back from it, and the cursor goes round the 2 rows without turning them.
void o16_08a83508(CPU *c);
void o16_08a83508_orig(CPU *c);
void o16_08a843c8(CPU *c);
void o16_08a843c8_orig(CPU *c);

#define SCENES 0x08A7EA50u  // the scene manager: its previous scene at +8
#define MAIN_FREE_BATTLE 2  // the list's items: Free Battle, then Multiplayer
#define MAIN_ITEMS 2

static uint32_t main_list;  // the main menu's list, once made (0: not yet, or another screen)
static bool main_making;    // the main menu is being made: its first turn is the one to Free Battle
static int main_from;       // the previous scene the player came back from

// mods/boot.c: a new scene. The main menu (scene 4) is made in its first frames, after this.
void dbz_menus_scene(int scene) {
	main_list = 0;
	main_making = false;
	if (!on() || scene != 4) return;
	uint32_t sm = M32(SCENES);
	if (!sm) return;
	main_from = (int)M32(sm + 8);
	M32(sm + 8) = 6;  // as if back from Free Battle: the menu turns its rows to it
	main_making = true;
}

static void main_hide_rest(uint32_t list) {
	for (uint32_t i = 0; i < 7; i++) {
		uint32_t item = M32(list + 136 + 4 * i);
		if (item && (i < MAIN_FREE_BATTLE || i >= MAIN_FREE_BATTLE + MAIN_ITEMS)) M8(item + 21) = 0;
	}
}

void o16_08a83508(CPU *c) {
	if (main_making) {
		main_making = false;
		main_list = c->r[4];
		c->r[5] = MAIN_FREE_BATTLE;
		o16_08a83508_orig(c);
		M32(main_list + 112) = main_from == 8 ? 1 : 0;
		main_hide_rest(main_list);
		return;
	}
	o16_08a83508_orig(c);
}

void o16_08a843c8(CPU *c) {
	uint32_t list = c->r[4];
	if (on() && list && list == main_list) {
		int32_t row = (int32_t)M32(list + 112);
		if (c->r[5] == 0 && row >= MAIN_ITEMS - 1) M32(list + 112) = (uint32_t)-1;  // down on the last: the first
		if (c->r[5] == 1 && row <= 0) M32(list + 112) = MAIN_ITEMS;                // up on the first: the last
	}
	o16_08a843c8_orig(c);
	if (on() && list && list == main_list) main_hide_rest(list);
}
