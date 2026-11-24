// A movie's HD version drawn in place of the game's frames (see hdmovie.h). One file for every PSP port,
// kept in sync by hand.
#include "hdmovie.h"

#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "texpack.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// The page decodes it (web/hdmovie.js): FFmpeg here took some 20 ms for each 1920x1088 frame on the
// game's thread, the conversion to RGBA and the upload as much again.
EM_JS(int, js_hd_open, (const uint8_t *data, const uint32_t *au, const uint8_t *idr, int count), {
	return Module.hdMovie ? Module.hdMovie.open(data, au, idr, count) : 0;
});
EM_JS(int, js_hd_want, (int id, int k), { return Module.hdMovie.want(id, k); });
EM_JS(int, js_hd_size, (int id), { return Module.hdMovie.size(id); });
EM_JS(int, js_hd_fill, (int id, int k), { return Module.hdMovie.fill(id, k, GLctx); });
EM_JS(void, js_hd_close, (int id), { Module.hdMovie.close(id); });
#endif

static const uint8_t TABLE_UUID[16] = {'B', 'L', 'H', 'D', 'm', 'o', 'v', 'i', 'e', 't', 'a', 'b', 'l', 'e', '0', '1'};

void hdmovie_init(HdMovie *h, const char *name) {
	hdmovie_close(h);
	snprintf(h->name, sizeof h->name, "movies/%s.h264", name);
	h->last = -1;
	hdmovie_open(h);  // now, before its first frame: its file on its way, its first pictures decoding
}

// An H.264 NAL unit's payload without its emulation prevention bytes (00 00 03 -> 00 00).
static size_t unescape(const uint8_t *p, size_t n, uint8_t *out) {
	size_t o = 0, zeros = 0;
	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && p[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = p[i] ? 0 : zeros + 1;
		out[o++] = p[i];
	}
	return o;
}

// The file read: its table if it has one (the first NAL unit) and where each frame's access unit starts.
static bool parse(HdMovie *h) {
	const uint8_t *d = h->data;
	size_t n = h->n;
	// the NAL units' starts (after each 00 00 01)
	size_t cap = 1024, cnt = 0;
	size_t *nal = malloc(cap * sizeof *nal);
	for (size_t i = 0; i + 3 < n; i++)
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			if (cnt == cap) nal = realloc(nal, (cap *= 2) * sizeof *nal);
			nal[cnt++] = i + 3;
			i += 2;
		}
	bool ok = false;
	size_t first = 0;
	if (cnt >= 2 && (d[nal[0]] & 0x1F) == 6) {  // a table: SEI user data unregistered (payload type 5), our UUID
		size_t end = nal[1] - 3;
		while (end > nal[0] && d[end - 1] == 0) end--;  // a 4-byte start code's leading zero
		uint8_t *sei = malloc(end - nal[0]);
		size_t sn = unescape(d + nal[0] + 1, end - nal[0] - 1, sei), i = 0;
		uint32_t type = 0, size = 0;
		while (i < sn && sei[i] == 0xFF) type += 255, i++;
		if (i < sn) type += sei[i++];
		while (i < sn && sei[i] == 0xFF) size += 255, i++;
		if (i < sn) size += sei[i++];
		if (type == 5 && size >= 20 && i + size <= sn && !memcmp(sei + i, TABLE_UUID, 16)) {
			const uint8_t *t = sei + i + 16;
			uint32_t count = (uint32_t)t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
			if (count && 20 + (size_t)count * 8 <= size) {
				h->table = malloc((size_t)count * 8);
				for (uint32_t k = 0; k < count; k++) {
					uint64_t v = 0;
					for (int b = 7; b >= 0; b--) v = v << 8 | t[4 + k * 8 + b];
					h->table[k] = v;
				}
				h->count = (int)count;
				first = 1;
			}
		}
		free(sei);
	}
	// the access units: a frame is one picture (a slice whose first macroblock is 0) with the parameter
	// sets and SEI before it; counted first when there is no table
	for (int pass = h->count ? 1 : 0; pass < 2; pass++) {
		if (pass == 1) {
			if (h->count <= 0 || h->count > 0xFFFF) goto out;
			h->au = malloc(((size_t)h->count + 1) * sizeof *h->au);
			h->idr = calloc((size_t)h->count, 1);
		}
		int k = -1;
		size_t before = 0;  // where the parameter sets / SEI before the next picture start (0: none yet)
		bool any = false;
		for (size_t j = first; j < cnt; j++) {
			int t = d[nal[j]] & 0x1F;
			size_t at = nal[j] - 3;
			if (at > 0 && d[at - 1] == 0) at--;  // its 4-byte start code
			if (t != 1 && t != 5) {
				if (!any) before = at, any = true;
				continue;
			}
			if (!(nal[j] + 1 < n && (d[nal[j] + 1] & 0x80))) continue;  // a later slice of the same picture
			if (++k >= (pass ? h->count : 0x10000)) break;
			if (pass) h->au[k] = (uint32_t)(any ? before : at), h->idr[k] = t == 5;
			any = false;
		}
		if (pass == 0) h->count = k + 1;
		else if (k + 1 != h->count) {
			rt_log("hdmovie: %s: %d pictures for %d frames in its table, left out", h->name, k + 1, h->count);
			goto out;
		}
	}
	h->au[h->count] = (uint32_t)n;
	ok = h->idr[0];
out:
	free(nal);
	return ok;
}

bool hdmovie_open(HdMovie *h) {
	if (!h->name[0] || h->off) return false;
	if (h->ctx || h->js) return true;
#ifndef __EMSCRIPTEN__
	if (h->state == 2) {  // not there: looked for again every few seconds
		uint64_t now = rt_now_us();
		if (now - h->tried_us < 5000000) return false;
		h->tried_us = now, h->state = 0;
	}
#endif
	uint8_t *d = texpack_file(h->name, &h->n, &h->state);
	if (!d) return false;
	h->data = realloc(d, h->n + AV_INPUT_BUFFER_PADDING_SIZE);
	memset(h->data + h->n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	if (!parse(h)) {
		rt_log("hdmovie: %s is not an HD movie this port can use (not H.264 from an IDR picture, or its table and pictures differ)", h->name);
		goto off;
	}
#ifdef __EMSCRIPTEN__
	h->js = js_hd_open(h->data, h->au, h->idr, h->count);
	if (!h->js) {
		rt_log("hdmovie: %s: this browser has no H.264 decoder for the page (WebCodecs): the game's own frames", h->name);
		goto off;
	}
	rt_log("hdmovie: %s: %d frames, %zu bytes, decoded by the browser", h->name, h->count, h->n);
	return true;
#else
	const AVCodec *vc = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!vc || !(h->ctx = avcodec_alloc_context3(vc))) goto off;
	h->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;  // no B-frames: each picture comes out as it is decoded
	if (avcodec_open2(h->ctx, vc, NULL) < 0) {
		avcodec_free_context(&h->ctx);
		goto off;
	}
	h->frame = av_frame_alloc();
	h->next = 0;
	rt_log("hdmovie: %s: %d frames, %zu bytes", h->name, h->count, h->n);
	return true;
#endif
off:
	free(h->data), h->data = NULL;
	h->off = true;
	return false;
}

int hdmovie_find(HdMovie *h, uint64_t fp) {
	if (!h->table) return -1;
	if (h->last + 1 < h->count && h->table[h->last + 1] == fp) return h->last + 1;
	if (h->last >= 0 && h->table[h->last] == fp) return h->last;
	for (int k = 0; k < h->count; k++)
		if (h->table[k] == fp) return k;
	return -1;
}

#ifdef __EMSCRIPTEN__
// ge.c's fill (when the game draws the buffer): the picture of frame arg & 0xFFFF of the page's movie arg >> 16
static bool fill(uintptr_t arg) { return js_hd_fill((int)(arg >> 16), (int)(arg & 0xFFFF)) != 0; }

// Frame k's picture is here (the page decodes a few frames ahead of the game).
static bool ready(HdMovie *h, int k) {
	int r = js_hd_want(h->js, k);
	if (r < 0) {
		rt_log("hdmovie: %s: the browser's decoder failed: the game's own frames", h->name);
		h->off = true;
	}
	if (r <= 0) return false;
	int s = js_hd_size(h->js);
	h->w = s >> 16, h->h = s & 0xFFFF;
	return h->w && h->h;
}
#else
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Frame k in RGBA (one of the ring's), decoded on from the last one, or from the IDR picture before
// it when it is behind or far ahead. NULL: none.
static const uint32_t *frame_rgba(HdMovie *h, int k) {
	for (int i = 0; i < 4; i++)
		if (h->ring[i] && h->ring_k[i] == k) return h->ring[i];
	if (k < h->next || k - h->next > 30) {
		int j = k;
		while (j > 0 && !h->idr[j]) j--;
		if (j > h->next || k < h->next) avcodec_flush_buffers(h->ctx), h->next = j;
	}
	bool got = false;
	while (h->next <= k) {
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = h->data + h->au[h->next];
		pkt.size = (int)(h->au[h->next + 1] - h->au[h->next]);
		int g = 0;
		if (avcodec_decode_video2(h->ctx, h->frame, &g, &pkt) < 0) g = 0;
		got = g && h->next == k;
		h->next++;
	}
	if (!got) return NULL;
	AVFrame *f = h->frame;
	if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
		rt_log("hdmovie: %s: not 4:2:0 YUV, left out", h->name);
		h->off = true;
		return NULL;
	}
	if (h->w != f->width || h->h != f->height) {
		for (int i = 0; i < 4; i++) free(h->ring[i]), h->ring[i] = NULL;
		h->w = f->width, h->h = f->height;
	}
	int r = k % 4;
	if (!h->ring[r]) h->ring[r] = malloc((size_t)h->w * h->h * 4);
	h->ring_k[r] = k;
	uint32_t *o = h->ring[r];
	for (int y = 0; y < h->h; y++, o += h->w) {  // as the game's own conversion (sceMpegAvcCsc, sceVideocodec)
		const uint8_t *Y = f->data[0] + y * f->linesize[0];
		const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
		const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
		for (int x = 0; x < h->w; x++) {
			int c = (Y[x] - 16) * 298, d = U[x / 2] - 128, e = V[x / 2] - 128;
			o[x] = 0xFF000000u | (uint32_t)clamp8((c + 516 * d + 128) >> 8) << 16 | (uint32_t)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8 |
			       clamp8((c + 409 * e + 128) >> 8);
		}
	}
	return h->ring[r];
}
#endif

bool hdmovie_show(HdMovie *h, int k, uint32_t addr, int iw, int ih) {
	if (k < 0 || iw <= 0 || ih <= 0 || !hdmovie_open(h)) return false;
	if (k >= h->count) k = h->count - 1;  // past its end: its last frame stays
	h->last = k;
#ifdef __EMSCRIPTEN__
	if (!ready(h, k)) return false;
#else
	const uint32_t *px = frame_rgba(h, k);
	if (!px) return false;
#endif
	if (h->w % iw || h->w / iw != h->h / ih) return false;  // not a whole multiple of the game's frame
	int i = 0;
	while (i < 4 && h->dst[i] != addr) i++;
	if (i == 4) {  // a buffer not seen yet: a free place, else the first
		i = 0;
		while (i < 3 && h->dst[i]) i++;
		h->dst[i] = addr;
	}
	if (!h->shown++) rt_log("hdmovie: %s: its frame %d (and on) drawn from its HD picture, %dx%d, at %08x", h->name, k, h->w, h->h, addr);
#ifdef __EMSCRIPTEN__
	ge_movie_hd_fill(addr, fill, (uintptr_t)h->js << 16 | (uintptr_t)k, h->w, h->h, iw, ih);
#else
	ge_movie_hd(addr, px, h->w, h->h, iw, ih);  // its picture stays in the ring for four frames
#endif
	return true;
}

bool hdmovie_forget(HdMovie *h, uint32_t addr) {
	bool was = false;
	for (int i = 0; i < 4; i++)
		if (h->dst[i] == addr) h->dst[i] = 0, was = true;
	return was;
}

void hdmovie_close(HdMovie *h) {
	if (h->shown) rt_log("hdmovie: %s: %u frames drawn from their HD pictures", h->name, h->shown);
	for (int i = 0; i < 4; i++) {
		if (h->dst[i]) ge_movie_hd(h->dst[i], NULL, 0, 0, 0, 0);
		free(h->ring[i]);
	}
	if (h->ctx) avcodec_free_context(&h->ctx);
	if (h->frame) av_frame_free(&h->frame);
#ifdef __EMSCRIPTEN__
	if (h->js) js_hd_close(h->js);  // before its file goes
#endif
	free(h->data);
	free(h->table);
	free(h->au);
	free(h->idr);
	memset(h, 0, sizeof *h);
	h->last = -1;
}
