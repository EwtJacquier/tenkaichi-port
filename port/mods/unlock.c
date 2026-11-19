// Mod: every character and every map can be picked from the start. The save (DATA.BIN) keeps what
// is unlocked as bits: the characters at +0x184 (9 bytes: a new game's save has 44 of them set, and
// the character select shows the others as "?"), the maps at +0x21a4 (a word, one bit each of the
// 10: a new game has the first 5, Wasteland to Planet Namek; the map select leaves the others out).
// As the save is read they are all set, and the game keeps them in its next save.
// DBZ_MOD_UNLOCK=0: the game's own unlocks.
//
// The battle camera is always Camera Type C (the farthest): the save keeps the Option menu's
// choice at +0x52c6 (3 Type A, 4 Type B, 5 Type C), and the main menu no longer has Option.
// DBZ_MOD_CAMERA=0: the save's own camera.
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define UNLOCK_AT 0x184
#define UNLOCK_BYTES 9
#define MAPS_AT 0x21a4
#define MAPS_ALL 0x3ffu
#define CAMERA_AT 0x52c6
#define CAMERA_C 5

void dbz_unlock_save(const char *name, uint32_t buf, uint32_t size) {
	if (strcmp(name, "DATA.BIN") || size < CAMERA_AT + 1) return;
	const char *cam = getenv("DBZ_MOD_CAMERA");
	if (!cam || atoi(cam) != 0) M8(buf + CAMERA_AT) = CAMERA_C;
	const char *off = getenv("DBZ_MOD_UNLOCK");
	if (off && atoi(off) == 0) return;
	for (uint32_t i = 0; i < UNLOCK_BYTES; i++) M8(buf + UNLOCK_AT + i) = 0xff;
	M32(buf + MAPS_AT) |= MAPS_ALL;
	rt_log("unlock: every character and map unlocked");
}
