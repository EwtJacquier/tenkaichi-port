// Mod: the online battles over the internet: no record lost to the empty ones, and input delay, so
// the latency doesn't slow the game.
//
// The game's battles over ad hoc are in lockstep: each frame, every copy needs every player's input
// record of the previous frame (FUN_08815610 / FUN_08814718 look them up in per-player rings, FUN_088174ac;
// FUN_0881548c waits for the missing ones, FUN_088155c0 checks the records' sync values agree).
// With one frame of slack a copy waits for the others whenever a record takes longer than a frame
// (33 ms) to arrive: over the internet the whole battle ran at half speed or less.
//
// Here those lookups ask for the records of DELAY frames back instead of one, for every player, the
// copy's own included: every copy applies the same inputs on the same frame, as before, only a few
// frames later; a record has DELAY frames to arrive before anyone waits for it. The first frames
// of each phase run as the game has them (WARMUP).
//
// DBZ_NET_INPUT_DELAY=n frames (1 = the game as it was; default 3, ~100 ms of input delay: up to
// about 100 ms of one-way latency costs no speed); DBZ_MOD_NETDELAY=0 turns it off.
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"

static int delay_frames(void) {
	static int d = -1;
	if (d < 0) {
		const char *off = getenv("DBZ_MOD_NETDELAY");
		const char *e = getenv("DBZ_NET_INPUT_DELAY");
		d = off && atoi(off) == 0 ? 1 : e ? atoi(e) : 3;
		if (d < 1) d = 1;
		if (d > 7) d = 7;  // the rings keep 10 records
		if (d > 1) rt_log("netdelay: online battles apply inputs %d frames late", d);
	}
	return d;
}

#define WARMUP 60

// FUN_088174ac(rings, slot, frame): the record of that frame in the player's ring, or 0.
void f_088174ac(CPU *c) {
	uint32_t ra = c->r[31];
	int d = delay_frames();
	// the four lookups of each of the two battle steps (FUN_08815610 and FUN_08814718: one for the
	// copy that made the room, one for the others)
	bool step = ra == 0x0881571Cu || ra == 0x0881573Cu || ra == 0x0881575Cu || ra == 0x0881577Cu ||
	            ra == 0x08814824u || ra == 0x08814844u || ra == 0x08814864u || ra == 0x08814884u;
	if (d > 1 && step) {
		int32_t frame = (int32_t)c->r[6];  // the battle's frame - 1
		// The first WARMUP frames of each phase (the battle's start, its round) as the game has them:
		// the copies' first handshake stalled when a copy waited long for a record with the delay on.
		// From then on the records DELAY frames back; the switch applies a few records again, the same
		// on every copy.
		if (frame >= WARMUP) c->r[6] = (uint32_t)(frame - (d - 1));
	}
	uint32_t rings = c->r[4], slot = c->r[5], want = c->r[6];
	f_088174ac_orig(c);
	static int log = -1;  // DBZ_NETDELAY_LOG=1 (tests): the step's lookups that found nothing, with the ring
	if (log < 0) log = getenv("DBZ_NETDELAY_LOG") != NULL;
	if (log && step && !c->r[2]) {
		static uint64_t last;
		if (rt_vblank_count() - last >= 30) {
			last = rt_vblank_count();
			uint32_t base = rings + slot * 164 + 0x29ac;
			char s[160];
			int n = 0;
			for (int i = 0; i < 10; i++) n += snprintf(s + n, sizeof s - (size_t)n, " %d", (int32_t)M32(base + 4 + 16 * (uint32_t)i));
			rt_log("netdelay: slot %u has no record %d (ring idx %u:%s)", slot, (int32_t)want, M32(base), s);
		}
	}
}

// FUN_088174fc(rings, slot, record): keeps a received (or the copy's own) record in the player's ring
// of 10. A copy that is waiting still sends a packet every frame, its records of the current frame,
// the one before and the one before that; the current one not made yet goes as frame -1. Those
// empty records were kept too, each pushing a real one out of the ring: with the internet's latency
// a copy waits often, the records it needed were gone before it looked for them, and the battle
// stalled or stopped. Here records with no frame are not kept. (DBZ_MOD_NETDELAY=0: as the game.)
void f_088174fc(CPU *c) {
	static int on = -1;
	if (on < 0) on = !getenv("DBZ_MOD_NETDELAY") || atoi(getenv("DBZ_MOD_NETDELAY")) != 0;
	if (on && c->r[6] && (int32_t)M32(c->r[6]) < 0) return;
	f_088174fc_orig(c);
}
