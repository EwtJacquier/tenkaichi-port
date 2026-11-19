// Mod: the player's name in the game is the site's account name (DBZ_NICK, the login). The save
// (DATA.BIN) keeps it as UTF-16 at +64, up to 10 characters (the game's own limit); as the save is
// read, the account's name is written there, so the menus, the rooms and the next save show it.
// A new game's name entry (the on-screen keyboard) is answered with it too (runtime/utility.c).
// DBZ_MOD_NAME=0 keeps the save's own name.
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define NAME_AT 64
#define NAME_MAX 10

void dbz_unlock_save(const char *name, uint32_t buf, uint32_t size);  // mods/unlock.c

// The save as it is read (runtime/utility.c): every character unlocked (mods/unlock.c), the name.
void rt_savedata_loaded(const char *name, uint32_t buf, uint32_t size) {
	dbz_unlock_save(name, buf, size);
	const char *nick = getenv("DBZ_NICK");
	const char *off = getenv("DBZ_MOD_NAME");
	if (!nick || !*nick || (off && atoi(off) == 0) || strcmp(name, "DATA.BIN") || size < NAME_AT + 2 * (NAME_MAX + 1)) return;
	uint32_t p = buf + NAME_AT;
	memset(MP(p), 0, 2 * (NAME_MAX + 1));
	int n = 0;
	for (const unsigned char *s = (const unsigned char *)nick; *s && n < NAME_MAX; s++) {
		if (*s < 0x20) continue;
		uint32_t ch = *s;
		if (ch >= 0x80) {  // UTF-8: decode one character
			int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
			ch &= 0x3F >> k;
			while (k-- && (s[1] & 0xC0) == 0x80) ch = ch << 6 | (*++s & 0x3F);
			if (ch > 0xFFFF) continue;
		}
		M16(p + 2 * (uint32_t)n++) = (uint16_t)ch;
	}
	rt_log("profile: the save's player name is %s", nick);
}
