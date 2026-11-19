// Mod: straight to the title. The game starts in its boot scene (the autosave notice, five
// publishers' logos, the opening), about 25 seconds before the title. The scene can't be left out
// (the title waits for what it sets up), so it runs unseen and as fast as the CPU goes (rt_turbo),
// its notice answered with X; the title's own opening is skipped with START, as a player would: the
// title shows in under a second.
//
// The scene manager: FUN_089c3294(manager, scene, arg) switches scenes (1 boot, 3 title, 4 main
// menu, 8 multiplayer...), FUN_089c33e0 runs the current one every frame.
// DBZ_MOD_SKIP_INTRO=0 keeps the original sequence; DBZ_BOOT_LOG=1 logs the scenes.
#include <stdlib.h>

#include "funcs.h"
#include "rt.h"

static int scene = -1;

void dbz_menus_scene(int scene);  // mods/menus.c
void dbz_room_frame(CPU *c);       // mods/room.c
void dbz_testko_frame(void);       // mods/testko.c (tests)

static bool skip_on(void) {
	static int skip = -1;
	if (skip < 0) skip = !getenv("DBZ_MOD_SKIP_INTRO") || atoi(getenv("DBZ_MOD_SKIP_INTRO")) != 0;
	return skip;
}

void f_089c3294(CPU *c) {
	static int log = -1;
	if (log < 0) log = getenv("DBZ_BOOT_LOG") != NULL;
	scene = (int)c->r[5];
	if (log) rt_log("boot: scene %d (arg %08x) at vblank %llu", scene, c->r[6], (unsigned long long)rt_vblank_count());
	if (skip_on()) rt_turbo(scene == 1);
	f_089c3294_orig(c);
	dbz_menus_scene(scene);  // mods/menus.c: the main menu's items
}

void f_089c33e0(CPU *c) {
	static int frames, title_frames;
	static bool title_done;
	if (skip_on() && scene == 1 && ++frames % 20 == 0) rt_tap_buttons(0x4000);  // X: past the autosave notice
	// the title's own opening (the sky, the flight, the logo): START a moment in skips it to "Press START"
	if (skip_on() && scene == 3 && !title_done && ++title_frames == 40) {
		rt_tap_buttons(0x0008);
		title_done = true;
	}
	// where the textures the game shows are seen first (runtime/ge.c DBZ_TEXCATALOG): the scene and
	// the code overlay that runs it (each menu and mode has its own)
	rt_scene = (uint32_t)scene << 16 | (rt_overlay() & 0xffff);
	dbz_room_frame(c);  // mods/room.c: Create New Room answered
	dbz_testko_frame();
	f_089c33e0_orig(c);
}

int dbz_scene(void) { return scene; }
