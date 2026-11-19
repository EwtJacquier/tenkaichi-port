// Tests: DBZ_TEST_KO=A@N or B@N: from vblank N on, the fighters of team A (1P, 2P) or B (3P, 4P) have
// no life left, so a fight ends with the other team's win (to reach and collect the result screens).
// The fighters: [[0x08A7B850]+0x14]+0x74.. (one word each, 1P to 4P), each +0x6C its character,
// whose life is at +0x1A34 (30000 full).
#include <stdlib.h>

#include "rt.h"

void dbz_testko_frame(void) {
	static int team = -1;
	static uint64_t from;
	if (team < 0) {
		const char *e = getenv("DBZ_TEST_KO");
		team = 0;
		if (e && (e[0] == 'A' || e[0] == 'B') && e[1] == '@') team = e[0] == 'A' ? 1 : 2, from = strtoull(e + 2, NULL, 10);
	}
	if (!team || rt_vblank_count() < from) return;
	uint32_t p1 = M32(0x08A7B850u);
	if (p1 < 0x08800000u || p1 >= 0x0a000000u) return;
	uint32_t p2 = M32(p1 + 0x14);
	if (p2 < 0x08800000u || p2 >= 0x0a000000u) return;
	for (int i = 0; i < 2; i++) {
		uint32_t f = M32(p2 + 0x74 + 4 * (uint32_t)(i + (team == 2 ? 2 : 0)));
		if (f < 0x08800000u || f >= 0x0a000000u) continue;
		uint32_t ch = M32(f + 0x6c);
		if (ch >= 0x08800000u && ch < 0x0a000000u && (int32_t)M32(ch + 0x1a34) > 0) M32(ch + 0x1a34) = 0;
	}
}
