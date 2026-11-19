// sceAudio: the 8 PSP output channels, mixed on a wall-clock "DMA" and pushed to the host.
//
// On the PSP every channel plays at 44.1 kHz and sceAudioOutputBlocking blocks while the channel
// already holds a buffer besides the one playing, which paces the game's sound threads. Here a
// virtual DMA consumes every channel at 44.1 kHz of wall-clock time (advance) and mixes into a
// FIFO that audio_pump hands to the host (SDL queue / browser AudioWorklet). The host keeps its
// own short buffer and absorbs clock drift, so the guest never depends on when or in which chunk
// sizes the host asks for audio.
//
// Each channel may run `slack` frames further ahead than on hardware: a sound thread woken a few
// milliseconds late (browser timers, a long display list) then doesn't leave a gap. The game sees
// the hardware view: blocking and sceAudioGetChannelRestLength subtract the slack.
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"

#define RATE 44100
#define RING_FRAMES 16384   // per channel, stereo frames
#define FIFO_FRAMES 16384   // mixed frames waiting for audio_pump
#define STALL_FRAMES 4096   // a longer gap between advances means the whole runtime stalled

typedef struct {
	bool reserved;
	uint32_t samples, format;  // format 0 = stereo, 0x10 = mono
	int16_t ring[RING_FRAMES * 2];
	uint32_t rd, wr;           // frame indices (wr - rd = queued)
	uint64_t dry_at;           // DMA position where the ring ran empty (0 = not dry)
} AudioCh;

#define OUT2 8  // sceAudioOutput2 (the "SRC" channel), after the 8 normal ones
static AudioCh ach[9];
static bool dev;                  // a host audio output is open
static uint32_t slack;            // extra frames a channel may hold beyond the hardware's two buffers
static uint64_t dma_t0, dma_pos;  // DMA clock origin (us) and frames played since then
static int16_t fifo[FIFO_FRAMES * 2];
static uint32_t fifo_n;
static uint64_t stat_underrun, stat_mixed;  // DBZ_FPS statistics
static uint32_t stat_min_margin = UINT32_MAX;  // fewest frames a busy channel had left when refilled

// DBZ_WAVOUT=file.wav: also write everything mixed to a WAV at exit (works headless).
static int16_t *wav_buf;
static uint64_t wav_cap, wav_used;
static const char *wav_path;

static void wav_write(void) {
	FILE *f = fopen(wav_path, "wb");
	if (!f) return;
	uint32_t data = (uint32_t)(wav_used * 4);
	uint8_t h[44] = "RIFF....WAVEfmt ";
	uint32_t v;
	v = 36 + data; memcpy(h + 4, &v, 4);
	v = 16; memcpy(h + 16, &v, 4);
	uint16_t s16 = 1; memcpy(h + 20, &s16, 2);
	s16 = 2; memcpy(h + 22, &s16, 2);
	v = RATE; memcpy(h + 24, &v, 4);
	v = RATE * 4; memcpy(h + 28, &v, 4);
	s16 = 4; memcpy(h + 32, &s16, 2);
	s16 = 16; memcpy(h + 34, &s16, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &data, 4);
	fwrite(h, 1, 44, f);
	fwrite(wav_buf, 4, wav_used, f);
	fclose(f);
}

static uint32_t queued(const AudioCh *c) { return c->wr - c->rd; }
static uint64_t frames_us(uint64_t n) { return n * 1000000ull / RATE; }

static void mix(uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		int32_t l = 0, r = 0;
		for (int c = 0; c <= OUT2; c++) {
			AudioCh *a = &ach[c];
			if (a->wr != a->rd) {
				uint32_t k = (a->rd++ % RING_FRAMES) * 2;
				l += a->ring[k];
				r += a->ring[k + 1];
				if (a->wr == a->rd) a->dry_at = dma_pos + i + 1;
			}
		}
		if (fifo_n < FIFO_FRAMES) {  // full only when nothing pumps (no device); drop then
			fifo[fifo_n * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
			fifo[fifo_n * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
			fifo_n++;
		}
	}
	stat_mixed += n;
}

// Run the DMA up to now.
static void advance(void) {
	uint64_t target = (rt_now_us() - dma_t0) * RATE / 1000000ull;
	if (target <= dma_pos) return;
	uint64_t n = target - dma_pos;
	if (n > STALL_FRAMES) {
		// Everything was stalled (hidden tab, long synchronous load), the sound threads included:
		// play what is queued instead of inventing a long silence, then continue from now.
		uint32_t m = 0;
		for (int c = 0; c <= OUT2; c++) {
			if (queued(&ach[c]) > m) m = queued(&ach[c]);
			ach[c].dry_at = 0;
		}
		if (n > m) n = m;
		dma_pos = target - n;
	}
	mix((uint32_t)n);
	dma_pos = target;
}

void audio_pump(void) {
	advance();
	if (!fifo_n) return;
	if (dev) plat_audio_push(fifo, (int)fifo_n);
	if (wav_buf) {
		uint64_t n = fifo_n;
		if (wav_used + n > wav_cap) n = wav_cap - wav_used;
		memcpy(wav_buf + wav_used * 2, fifo, n * 4);
		wav_used += n;
	}
	fifo_n = 0;
}

uint64_t audio_output_latency_us(void) {
	// Mixed frames wait for the next pump (vblank or idle; ~half a vblank on average), then sit
	// in the host's buffer.
	return dev ? 8000 + plat_audio_latency_us() : 0;
}

// Underruns and mixed frames since the last call (for the DBZ_FPS log).
void audio_stats(uint64_t *underrun, uint64_t *mixed, uint32_t *min_margin) {
	*underrun = stat_underrun;
	*mixed = stat_mixed;
	*min_margin = stat_min_margin == UINT32_MAX ? 0 : stat_min_margin;
	stat_underrun = stat_mixed = 0;
	stat_min_margin = UINT32_MAX;
}

void audio_init(void) {
	dma_t0 = rt_now_us();
	const char *s = getenv("DBZ_AUDIO_SLACK");  // frames
#ifdef __EMSCRIPTEN__
	slack = s ? (uint32_t)atoi(s) : 1024;
#else
	slack = s ? (uint32_t)atoi(s) : 768;
#endif
	if (slack > RING_FRAMES / 2) slack = RING_FRAMES / 2;
	wav_path = getenv("DBZ_WAVOUT");
	if (wav_path) {
		wav_cap = (uint64_t)RATE * 60 * 5;  // up to 5 minutes
		wav_buf = calloc(wav_cap * 2, sizeof(int16_t));
		atexit(wav_write);
	}
	// Headless runs are silent unless DBZ_AUDIO=1 (e.g. with SDL_AUDIODRIVER=dummy).
	if ((getenv("DBZ_HEADLESS") && !getenv("DBZ_AUDIO")) || getenv("DBZ_NOAUDIO")) {
		return;
	}
	dev = plat_audio_open();
	rt_log(dev ? "audio: 44.1 kHz stereo output open" : "audio: no output device; running silent");
}

static void push(AudioCh *c, uint32_t buf, int lvol, int rvol) {
	uint32_t n = c->samples;
	bool mono = c->format == 0x10;
	const int16_t *src = (const int16_t *)MP(buf);
	if (c->dry_at) {
		// The ring ran empty and the thread is back: if that was a short break in a continuous
		// stream (not the end of a sound), count the frames the DMA played without it.
		uint64_t gap = dma_pos - c->dry_at;
		if (gap < RATE / 10) stat_underrun += gap;
		c->dry_at = 0;
	}
	for (uint32_t i = 0; i < n && queued(c) < RING_FRAMES; i++) {
		int32_t l = mono ? src[i] : src[i * 2], r = mono ? src[i] : src[i * 2 + 1];
		uint32_t k = (c->wr % RING_FRAMES) * 2;
		c->ring[k] = (int16_t)((l * lvol) >> 15);
		c->ring[k + 1] = (int16_t)((r * rvol) >> 15);
		c->wr++;
	}
}

static void output(CPU *c, uint32_t ch, int lvol, int rvol, uint32_t buf, bool blocking) {
	if (ch > OUT2 || !ach[ch].reserved) {
		RET(0x80260008u);
		return;
	}
	AudioCh *a = &ach[ch];
	// Hardware: one buffer playing and one queued. Here: plus the slack.
	uint32_t limit = a->samples + slack;
	for (;;) {
		advance();
		uint32_t q = queued(a);
		if (q <= limit) break;
		if (!blocking) {
			RET(0x80260002u);
			return;
		}
		rt_wait_until(rt_now_us() + frames_us(q - limit) + 100);
	}
	if (buf) {
		uint32_t q = queued(a);
		if (q && q < stat_min_margin) stat_min_margin = q;
		push(a, buf, lvol, rvol);
	}
	RET(a->samples);
}

static int vol(uint32_t v) { return v > 0xFFFF ? 0x8000 : (int)v; }

void hle_sceAudioChReserve(CPU *c) {
	int ch = (int)A0;
	if (ch < 0) {
		for (ch = 0; ch < 8 && ach[ch].reserved; ch++) {
		}
	}
	if (ch >= 8 || ach[ch].reserved) {
		RET(0x80260002u);
		return;
	}
	ach[ch].reserved = true;
	ach[ch].samples = A1;
	ach[ch].format = A2;
	ach[ch].rd = ach[ch].wr = 0;
	ach[ch].dry_at = 0;
	TRACE("sceAudioChReserve(%d, %d, %x) -> %d", (int)A0, A1, A2, ch);
	RET(ch);
}

void hle_sceAudioChRelease(CPU *c) {
	if (A0 < 8) {
		ach[A0].reserved = false;
		ach[A0].rd = ach[A0].wr;
		ach[A0].dry_at = 0;
	}
	RET(0);
}

void hle_sceAudioOutputBlocking(CPU *c) { output(c, A0, vol(A1), vol(A1), A2, true); }
void hle_sceAudioOutputPannedBlocking(CPU *c) { output(c, A0, vol(A1), vol(A2), A3, true); }
void hle_sceAudioOutputPanned(CPU *c) { output(c, A0, vol(A1), vol(A2), A3, false); }

void hle_sceAudioGetChannelRestLength(CPU *c) {
	if (A0 >= 8) {
		RET(0x80260008u);
		return;
	}
	advance();
	uint32_t q = queued(&ach[A0]);
	RET(q > slack ? q - slack : 0);
}
void hle_sceAudioChangeChannelConfig(CPU *c) {
	if (A0 < 8) ach[A0].format = A1;
	RET(0);
}
void hle_sceAudioChangeChannelVolume(CPU *c) { RET(0); }
void hle_sceAudioSetChannelDataLen(CPU *c) {
	if (A0 < 8) ach[A0].samples = A1;
	RET(0);
}

// sceAudioOutput2: one more stereo 44.1 kHz stream (SCREAM's "SimpleAudio" mixes into it).
void hle_sceAudioOutput2Reserve(CPU *c) {
	AudioCh *a = &ach[OUT2];
	if (a->reserved) {
		RET(0x80268002u);  // already reserved
		return;
	}
	a->reserved = true;
	a->samples = A0;
	a->format = 0;
	a->rd = a->wr = 0;
	a->dry_at = 0;
	TRACE("sceAudioOutput2Reserve(%d)", A0);
	RET(0);
}
void hle_sceAudioOutput2Release(CPU *c) {
	ach[OUT2].reserved = false;
	ach[OUT2].rd = ach[OUT2].wr;
	RET(0);
}
void hle_sceAudioOutput2ChangeLength(CPU *c) {
	ach[OUT2].samples = A0;
	RET(0);
}
void hle_sceAudioOutput2OutputBlocking(CPU *c) { output(c, OUT2, vol(A0), vol(A0), A1, true); }
void hle_sceAudioOutput2GetRestSample(CPU *c) {
	advance();
	uint32_t q = queued(&ach[OUT2]);
	RET(q > slack ? q - slack : 0);
}
