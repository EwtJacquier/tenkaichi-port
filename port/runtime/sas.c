// sceSasCore: the PSP software mixer used for sound effects. 32 voices of VAG (PSP ADPCM)
// data with pitch, loop points, the ADSR envelope (libsas curve types and rates) and per-voice
// volume.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

#define MAX_VOICES 32
#define ENV_MAX 0x40000000u

typedef enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvPhase;
// sceSasSetADSRmode curve types.
enum { CURVE_LIN_INC, CURVE_LIN_DEC, CURVE_LIN_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };

typedef struct {
	int type;
	int64_t rate;  // per-sample step in envelope units (ENV_MAX = full)
} Curve;

typedef struct {
	bool playing, paused, noise;
	bool pcm;                // 16-bit PCM data (sceSasSetVoicePCM) instead of VAG
	int pcm_loop;            // PCM loop start in samples, -1 = no loop
	uint32_t pcm_pos;
	uint32_t vag, size;      // VAG data and its length in bytes
	bool loop_enabled;
	// sceSasSetVoice on a voice that is playing: the new sample waits for the next KeyOn (the
	// decoder goes on through the old one, as PPSSPP's keeps its own read pointer)
	bool next_set, next_pcm;
	uint32_t next_vag, next_size;
	int next_loop;
	int pitch;               // 0x1000 = 1.0
	int lvol, rvol;          // 0..0x1000
	// ADPCM decoder
	uint32_t block;          // byte offset of the current 16-byte block
	uint32_t loop_block;
	int16_t samples[28];
	int sample_idx;
	int hist1, hist2;
	bool end_after_block, finished;
	double frac;             // fractional position between samples (for pitch)
	int16_t prev, cur;       // interpolation pair
	// envelope: height 0..ENV_MAX, one step per output sample
	EnvPhase phase;
	int64_t height;
	Curve attack, decay, sustain, release;
	int64_t sustain_level;
	uint32_t noise_seed;
} Voice;

static Voice voices[MAX_VOICES];
static uint32_t grain = 256, outmode;

static const int vag_coef[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static void decode_block(Voice *v) {
	if (v->block + 16 > v->size) {
		v->finished = true;
		return;
	}
	const uint8_t *b = MP(v->vag + v->block);
	int shift = b[0] & 0xF, pred = (b[0] >> 4) & 0xF, flags = b[1];
	if (pred > 4) pred = 0;
	if (flags == 7) {  // end marker block
		v->finished = true;
		return;
	}
	if (flags & 4) v->loop_block = v->block;
	for (int i = 0; i < 28; i++) {
		int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
		int s = (int16_t)(nib << 12) >> shift;
		s += (v->hist1 * vag_coef[pred][0] + v->hist2 * vag_coef[pred][1]) >> 6;
		s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
		v->hist2 = v->hist1;
		v->hist1 = s;
		v->samples[i] = (int16_t)s;
	}
	v->sample_idx = 0;
	v->block += 16;
	if (flags & 1) {  // end of the sample (loop end when flag 2 is set)
		if ((flags & 2) && v->loop_enabled) v->block = v->loop_block;
		else v->end_after_block = true;
	}
}

static int16_t next_sample(Voice *v) {
	if (v->noise) {
		v->noise_seed = v->noise_seed * 1103515245u + 12345u;
		return (int16_t)(v->noise_seed >> 16);
	}
	if (v->pcm) {  // size counts samples
		if (v->pcm_pos >= v->size) {
			if (v->pcm_loop < 0 || (uint32_t)v->pcm_loop >= v->size) {
				v->finished = true;
				return 0;
			}
			v->pcm_pos = (uint32_t)v->pcm_loop;
		}
		return (int16_t)M16(v->vag + 2 * v->pcm_pos++);
	}
	if (v->sample_idx >= 28) {
		if (v->end_after_block) {
			v->finished = true;
			return 0;
		}
		decode_block(v);
		if (v->finished) return 0;
	}
	return v->samples[v->sample_idx++];
}

// Envelope rates. sceSasSetADSR takes raw per-sample rates; sceSasSetSimpleADSR packs 7/4/5-bit
// codes into two words (the libsas bit layout), converted here to the same raw rates.
static int64_t simple_rate(int n, int shift) {
	n &= 0x7F;
	if (n == 0x7F) return 0;
	int64_t r = ((int64_t)(7 - (n & 3)) << shift) >> (n >> 2);
	return r ? r : 1;
}

static void set_simple_adsr(Voice *v, uint32_t e1, uint32_t e2) {
	v->attack.type = (e1 & 0x8000) ? CURVE_LIN_BENT : CURVE_LIN_INC;
	v->attack.rate = simple_rate((int)(e1 >> 8), 26);
	int dn = (int)((e1 >> 4) & 0xF);
	v->decay.type = CURVE_EXP_DEC;
	v->decay.rate = dn ? (int64_t)(0x80000000u >> dn) : 0x7FFFFFFF;
	v->sustain_level = (int64_t)((e1 & 0xF) + 1) << 26;
	static const int sustain_types[8] = {CURVE_LIN_INC, CURVE_LIN_INC, CURVE_LIN_DEC, CURVE_LIN_DEC,
	                                     CURVE_LIN_BENT, CURVE_LIN_BENT, CURVE_EXP_DEC, CURVE_EXP_DEC};
	v->sustain.type = sustain_types[(e2 >> 13) & 7];
	v->sustain.rate = simple_rate((int)(e2 >> 6), v->sustain.type == CURVE_EXP_DEC ? 24 : 26);
	int rn = (int)(e2 & 0x1F);
	v->release.type = (e2 & 0x20) ? CURVE_EXP_DEC : CURVE_LIN_DEC;
	if (rn == 31) v->release.rate = 0;
	else if (v->release.type == CURVE_LIN_DEC) v->release.rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (0x10000000 >> rn);
	else v->release.rate = rn ? (int64_t)(0x80000000u >> rn) : 0x7FFFFFFF;
}

static void walk(Voice *v, const Curve *c) {
	switch (c->type) {
	case CURVE_LIN_INC: v->height += c->rate; break;
	case CURVE_LIN_DEC: v->height -= c->rate; break;
	case CURVE_LIN_BENT: v->height += v->height <= (int64_t)ENV_MAX * 3 / 4 ? c->rate : c->rate / 4; break;
	case CURVE_EXP_DEC:  // falls by a fraction of the height each sample (rate / 2^32)
		v->height -= ((v->height * c->rate) >> 32) + 1;
		break;
	case CURVE_EXP_INC:  // rises by a fraction of the distance to the top
		v->height += ((((int64_t)ENV_MAX - v->height) * c->rate) >> 32) + 1;
		break;
	case CURVE_DIRECT: v->height = c->rate; break;
	}
}

static void env_tick(Voice *v) {
	switch (v->phase) {
	case ENV_ATTACK:
		walk(v, &v->attack);
		if (v->height >= (int64_t)ENV_MAX || v->height < 0) v->height = ENV_MAX, v->phase = ENV_DECAY;
		break;
	case ENV_DECAY:
		walk(v, &v->decay);
		if (v->height < v->sustain_level) v->height = v->sustain_level, v->phase = ENV_SUSTAIN;
		break;
	case ENV_SUSTAIN:
		walk(v, &v->sustain);
		if (v->height > (int64_t)ENV_MAX) v->height = ENV_MAX;
		if (v->height <= 0) v->height = 0, v->phase = ENV_RELEASE;
		break;
	case ENV_RELEASE:
		walk(v, &v->release);
		if (v->height <= 0) v->height = 0, v->phase = ENV_OFF, v->playing = false;
		break;
	default: break;
	}
}

static void mix(int32_t *acc, uint32_t frames) {
	for (int vi = 0; vi < MAX_VOICES; vi++) {
		Voice *v = &voices[vi];
		if (!v->playing || v->paused) continue;
		double step = v->pitch / 4096.0;
		for (uint32_t i = 0; i < frames && v->playing; i++) {
			v->frac += step;
			while (v->frac >= 1.0) {
				v->frac -= 1.0;
				v->prev = v->cur;
				v->cur = next_sample(v);
			}
			if (v->finished && v->phase != ENV_RELEASE) {
				v->playing = false;
				break;
			}
			double s = v->prev + (v->cur - v->prev) * v->frac;
			s *= (double)v->height / ENV_MAX;
			acc[i * 2] += (int32_t)(s * v->lvol / 4096.0);
			acc[i * 2 + 1] += (int32_t)(s * v->rvol / 4096.0);
			env_tick(v);
		}
	}
}

static void write_out(uint32_t out, const int32_t *acc, uint32_t frames, bool add, int lvol, int rvol) {
	int16_t *o = (int16_t *)MP(out);
	for (uint32_t i = 0; i < frames * 2; i++) {
		int32_t s = acc[i];
		if (add) s += (o[i] * ((i & 1) ? rvol : lvol)) >> 12;
		o[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
}

void hle___sceSasInit(CPU *c) {
	memset(voices, 0, sizeof voices);
	grain = A1;
	outmode = A3;
	for (int i = 0; i < MAX_VOICES; i++) {
		voices[i].pitch = 0x1000;
		voices[i].noise_seed = 0x1234u + (uint32_t)i;
		// Until the game sets an envelope: instant attack, hold, fast release.
		voices[i].attack = (Curve){CURVE_LIN_INC, ENV_MAX};
		voices[i].decay = (Curve){CURVE_LIN_DEC, 0};
		voices[i].sustain = (Curve){CURVE_LIN_DEC, 0};
		voices[i].release = (Curve){CURVE_LIN_DEC, ENV_MAX / 256};
		voices[i].sustain_level = ENV_MAX;
	}
	TRACE("__sceSasInit(grain=%d, voices=%d, outmode=%d, rate=%d)", A1, A2, A3, A4);
	RET(0);
}

void hle___sceSasCore(CPU *c) {
	static int32_t acc[4096 * 2];
	uint32_t n = grain > 4096 ? 4096 : grain;
	memset(acc, 0, sizeof(int32_t) * n * 2);
	mix(acc, n);
	write_out(A1, acc, n, false, 0, 0);
	RET(0);
}

void hle___sceSasCoreWithMix(CPU *c) {
	static int32_t acc[4096 * 2];
	uint32_t n = grain > 4096 ? 4096 : grain;
	memset(acc, 0, sizeof(int32_t) * n * 2);
	mix(acc, n);
	static int solo = -1;  // DBZ_SAS_SOLO=1: drop the game's own input (BGM), keep only the voices
	if (solo < 0) solo = getenv("DBZ_SAS_SOLO") != NULL;
	write_out(A1, acc, n, !solo, (int)A2, (int)A3);
	RET(0);
}

static Voice *voice(uint32_t i) { return i < MAX_VOICES ? &voices[i] : NULL; }

static int sas_log = -1;  // DBZ_SAS_LOG=1 (diagnostics): voices given new data while they play
void hle___sceSasSetVoice(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	if (sas_log < 0) sas_log = getenv("DBZ_SAS_LOG") != NULL;
	if (sas_log && v->playing && !v->finished)
		rt_log("SAS voice %u: SetVoice while playing: %08x+%x -> %08x size %x loop %d (block %x, pitch %x)", A1, v->vag, v->size, A2, A3, A4, v->block, v->pitch);
	if (v->playing && !v->finished && !v->noise && !v->pcm) {
		v->next_set = true, v->next_pcm = false;
		v->next_vag = A2, v->next_size = A3, v->next_loop = A4 != 0;
		RET(0);
		return;
	}
	v->next_set = false;
	v->vag = A2;
	v->size = A3;
	v->loop_enabled = A4 != 0;
	v->noise = false;
	v->pcm = false;
	RET(0);
}
// (core, voice, samples address, length in samples, loop start or -1)
void hle___sceSasSetVoicePCM(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	if (sas_log < 0) sas_log = getenv("DBZ_SAS_LOG") != NULL;
	if (sas_log && v->playing && !v->finished)
		rt_log("SAS voice %u: SetVoicePCM while playing: %08x+%x -> %08x samples %x loop %d (pos %x)", A1, v->vag, v->size, A2, A3, (int)A4, v->pcm_pos);
	v->next_set = false;
	v->vag = A2;
	v->size = A3;
	v->pcm_loop = (int)A4;
	v->noise = false;
	v->pcm = true;
	RET(0);
}
void hle___sceSasSetNoise(CPU *c) {
	Voice *v = voice(A1);
	if (v) v->noise = true, v->next_set = false;
	RET(0);
}
void hle___sceSasSetKeyOn(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	// A stretch a mod skips (rt_turbo: unseen, at full speed) is not heard either: its sounds never
	// start (a menu's cursor and confirm while it is answered for the player). DBZ_TURBO_SOUND=1: heard.
	static int turbo_sound = -1;
	if (turbo_sound < 0) turbo_sound = getenv("DBZ_TURBO_SOUND") && atoi(getenv("DBZ_TURBO_SOUND")) != 0;
	if (rt_turbo_on() && !turbo_sound) {
		v->playing = false;
		v->next_set = false;
		RET(0);
		return;
	}
	if (v->next_set) {  // the sample given while the voice still played
		v->next_set = false;
		v->vag = v->next_vag;
		v->size = v->next_size;
		v->loop_enabled = v->next_loop;
		v->noise = false;
		v->pcm = v->next_pcm;
	}
	v->playing = true;
	v->paused = false;
	v->block = 0;
	v->loop_block = 0;
	v->sample_idx = 28;
	v->hist1 = v->hist2 = 0;
	v->end_after_block = v->finished = false;
	v->pcm_pos = 0;
	v->frac = 0;
	v->prev = v->cur = 0;
	v->phase = ENV_ATTACK;
	v->height = 0;
	RET(0);
}
void hle___sceSasSetKeyOff(CPU *c) {
	Voice *v = voice(A1);
	if (v && v->playing) v->phase = ENV_RELEASE;
	RET(0);
}
void hle___sceSasSetPitch(CPU *c) {
	Voice *v = voice(A1);
	if (v) v->pitch = (int)A2;
	RET(0);
}
void hle___sceSasSetVolume(CPU *c) {
	Voice *v = voice(A1);
	if (v) v->lvol = (int)A2, v->rvol = (int)A3;
	RET(0);
}
void hle___sceSasSetPause(CPU *c) {
	for (int i = 0; i < MAX_VOICES; i++) {
		if (A1 & (1u << i)) voices[i].paused = A2 != 0;
	}
	RET(0);
}
// flags select which of the 4 curves are updated (bit0 attack, bit1 decay, bit2 sustain, bit3 release)
void hle___sceSasSetADSR(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	uint32_t flags = A2;
	if (flags & 1) v->attack.rate = (int32_t)A3;
	if (flags & 2) v->decay.rate = (int32_t)A4;
	if (flags & 4) v->sustain.rate = (int32_t)A5;
	if (flags & 8) v->release.rate = (int32_t)A6;
	TRACE("__sceSasSetADSR(voice %d, flags %x, %08x %08x %08x %08x)", A1, flags, A3, A4, A5, A6);
	RET(0);
}
void hle___sceSasSetADSRmode(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	uint32_t flags = A2;
	if (flags & 1) v->attack.type = (int)A3;
	if (flags & 2) v->decay.type = (int)A4;
	if (flags & 4) v->sustain.type = (int)A5;
	if (flags & 8) v->release.type = (int)A6;
	TRACE("__sceSasSetADSRmode(voice %d, flags %x, %d %d %d %d)", A1, flags, A3, A4, A5, A6);
	RET(0);
}
void hle___sceSasSetSL(CPU *c) {
	Voice *v = voice(A1);
	if (v) v->sustain_level = (int32_t)A2;
	RET(0);
}
void hle___sceSasSetSimpleADSR(CPU *c) {
	Voice *v = voice(A1);
	if (!v) { RET(0x80420010u); return; }
	set_simple_adsr(v, A2, A3);
	RET(0);
}
void hle___sceSasGetEndFlag(CPU *c) {
	uint32_t f = 0;
	for (int i = 0; i < MAX_VOICES; i++) {
		if (!voices[i].playing) f |= 1u << i;
	}
	RET(f);
}
void hle___sceSasGetPauseFlag(CPU *c) {
	uint32_t f = 0;
	for (int i = 0; i < MAX_VOICES; i++) {
		if (voices[i].paused) f |= 1u << i;
	}
	RET(f);
}
void hle___sceSasGetEnvelopeHeight(CPU *c) {
	Voice *v = voice(A1);
	RET(v && v->playing ? (uint32_t)v->height : 0);
}
void hle___sceSasGetAllEnvelopeHeights(CPU *c) {
	for (int i = 0; i < MAX_VOICES; i++) {
		M32(A1 + (uint32_t)i * 4) = voices[i].playing ? (uint32_t)voices[i].height : 0;
	}
	RET(0);
}
void hle___sceSasGetGrain(CPU *c) { RET(grain); }
void hle___sceSasSetGrain(CPU *c) {
	grain = A1;
	RET(0);
}
void hle___sceSasGetOutputmode(CPU *c) { RET(outmode); }
void hle___sceSasSetOutputmode(CPU *c) {
	outmode = A1;
	RET(0);
}
void hle___sceSasRevType(CPU *c) { RET(0); }
void hle___sceSasRevParam(CPU *c) { RET(0); }
void hle___sceSasRevEVOL(CPU *c) { RET(0); }
void hle___sceSasRevVON(CPU *c) { RET(0); }
