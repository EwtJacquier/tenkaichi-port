// scePsmfPlayer: PSP movies (.pmf = 0x800-byte PSMF header + MPEG-PS with H.264 video and
// ATRAC3+ audio in private stream 1). Own demuxer reading the disc in chunks, FFmpeg for the
// H.264 and ATRAC3+ decoding. Video frames are written as RGBA8888 into the buffer the game
// passes to GetVideoData; audio is handed out in 2048-sample stereo blocks. A movie with an HD
// version in the texture pack (movies/<name>.h264: H.264 at a whole multiple of 480x272, the same
// frames) has each frame's HD picture decoded too and drawn in its place (ge_movie_hd).
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"
#include "texpack.h"

enum { ST_NONE = 0, ST_INIT = 1, ST_STANDBY = 2, ST_PLAYING = 4, ST_FINISHED = 0x200 };
#define ERR_NO_DATA 0x8061600Cu

typedef struct {
	uint8_t *data;
	size_t len, cap;
} Buf;

// FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end of its input (parser,
// bitstream readers), so the data is always followed by that many zero bytes. Without them it
// reads stale heap contents, which differ from run to run (corrupted movie frames).
static void buf_pad(Buf *b) {
	if (b->data) memset(b->data + b->len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
}
static void buf_append(Buf *b, const uint8_t *p, size_t n) {
	if (b->len + n + AV_INPUT_BUFFER_PADDING_SIZE > b->cap) {
		b->cap = (b->len + n) * 2 + 4096 + AV_INPUT_BUFFER_PADDING_SIZE;
		b->data = realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
	buf_pad(b);
}
static void buf_consume(Buf *b, size_t n) {
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
	buf_pad(b);
}

typedef struct {
	uint32_t addr;  // guest player handle
	int state;
	uint64_t file_off;  // ISO offset of the MPEG-PS data
	uint64_t pos, end;
	uint8_t chunk[65536];
	size_t chunk_len, chunk_pos;
	bool eof;
	uint32_t first_ts, last_ts;
	// video
	AVCodecContext *vctx;
	AVCodecParserContext *parser;
	AVFrame *vframe;
	bool have_frame;
	int64_t frame_pts;
	uint32_t frame_index;  // frames come out in display order at a constant 29.97 fps
	Buf video_es;
	int64_t video_pts;  // PTS of the data currently in video_es
	// audio
	AVCodecContext *actx;
	AVFrame *aframe;
	Buf audio_es;
	int64_t audio_pts0;     // PTS of the first audio frame (-1 until seen)
	uint64_t audio_frames;  // ATRAC3+ frames (2048 samples) handed to the game
	uint64_t audio_t0_us;   // estimated time the first audio frame is heard
	// clock
	uint64_t start_us;
	bool video_done;
	bool skip;  // ends as soon as it starts (rt_psmf_skip_next)
	bool hide;  // plays black and silent (rt_psmf_hide_next)
	uint64_t log_us;
	// the HD version (texture pack: movies/<name>.h264)
	char hd_name[96];
	int hd_state;       // texpack_file's (the web fetches it while the movie starts)
	bool hd_off;        // none, or it stopped decoding
	uint8_t *hd_es;     // the whole stream (H.264 Annex B), padded
	size_t hd_n, hd_pos;
	AVCodecContext *hd_ctx;
	AVCodecParserContext *hd_parser;
	AVFrame *hd_frame;
	uint32_t hd_count;  // HD frames decoded
	uint32_t *hd_rgba;  // the last HD frame decoded (one of hd_ring)
	uint32_t *hd_ring[4];  // the last four: the game draws a frame written up to two before (its three buffers in turn)
	int hd_w, hd_h;
	uint32_t hd_dst[4];  // its buffers, to tell ge.c when the movie is over
} Player;

static Player players[4];

static Player *find(uint32_t addr) {
	for (int i = 0; i < 4; i++) {
		if (players[i].addr == addr) return &players[i];
	}
	return NULL;
}

static Player *alloc(uint32_t addr) {
	Player *p = find(addr);
	if (p) return p;
	for (int i = 0; i < 4; i++) {
		if (!players[i].addr) {
			memset(&players[i], 0, sizeof players[i]);
			players[i].addr = addr;
			return &players[i];
		}
	}
	return &players[0];
}

static void hd_close(Player *p) {
	for (int i = 0; i < 4; i++) {
		if (p->hd_dst[i]) ge_movie_hd(p->hd_dst[i], NULL, 0, 0, 0, 0);
		free(p->hd_ring[i]);
		p->hd_dst[i] = 0, p->hd_ring[i] = NULL;
	}
	if (p->hd_ctx) avcodec_free_context(&p->hd_ctx);
	if (p->hd_parser) av_parser_close(p->hd_parser);
	if (p->hd_frame) av_frame_free(&p->hd_frame);
	free(p->hd_es);
	p->hd_es = NULL, p->hd_rgba = NULL, p->hd_parser = NULL, p->hd_count = 0, p->hd_pos = 0;
}

static void player_close(Player *p) {
	hd_close(p);
	if (p->vctx) avcodec_free_context(&p->vctx);
	if (p->actx) avcodec_free_context(&p->actx);
	if (p->parser) av_parser_close(p->parser);
	if (p->vframe) av_frame_free(&p->vframe);
	if (p->aframe) av_frame_free(&p->aframe);
	free(p->video_es.data);
	free(p->audio_es.data);
	uint32_t addr = p->addr;
	memset(p, 0, sizeof *p);
	p->addr = addr;
}

// ---- MPEG-PS reading ------------------------------------------------------------------------
static int rd_byte(Player *p) {
	if (p->chunk_pos >= p->chunk_len) {
		if (p->pos >= p->end) {
			p->eof = true;
			return -1;
		}
		size_t n = sizeof p->chunk;
		if (p->pos + n > p->end) n = (size_t)(p->end - p->pos);
		io_iso_read(p->file_off + p->pos, p->chunk, n);
		p->pos += n;
		p->chunk_len = n;
		p->chunk_pos = 0;
	}
	return p->chunk[p->chunk_pos++];
}
static void rd_skip(Player *p, size_t n) {
	while (n-- && !p->eof) rd_byte(p);
}
static void rd_bytes(Player *p, uint8_t *out, size_t n) {
	for (size_t i = 0; i < n; i++) {
		int b = rd_byte(p);
		if (b < 0) return;
		out[i] = (uint8_t)b;
	}
}

// Reads PES packets until one video or audio payload has been appended. Returns false at EOF.
static bool demux_one(Player *p) {
	uint32_t code = 0xFFFFFFFFu;
	for (;;) {
		int b = rd_byte(p);
		if (b < 0) return false;
		code = (code << 8) | (uint32_t)b;
		if ((code & 0xFFFFFF00u) != 0x00000100u) continue;
		uint32_t id = code & 0xFF;
		if (id == 0xBA) {  // pack header (MPEG-2)
			uint8_t h[10];
			rd_bytes(p, h, 10);
			rd_skip(p, h[9] & 7);
			continue;
		}
		if (id == 0xB9) return false;  // program end
		if (id < 0xBB) continue;
		int len = rd_byte(p) << 8;
		len |= rd_byte(p);
		if (id != 0xE0 && id != 0xBD) {
			rd_skip(p, (size_t)len);
			continue;
		}
		uint8_t f[3];
		rd_bytes(p, f, 3);
		int hdr = f[2];
		int64_t pts = -1;
		if (f[1] & 0x80) {
			uint8_t t[5];
			rd_bytes(p, t, 5);
			pts = ((int64_t)(t[0] & 0x0E) << 29) | (t[1] << 22) | ((t[2] & 0xFE) << 14) | (t[3] << 7) | (t[4] >> 1);
			rd_skip(p, (size_t)(hdr - 5));
		}
		else {
			rd_skip(p, (size_t)hdr);
		}
		int payload = len - 3 - hdr;
		if (payload <= 0) continue;
		uint8_t *tmp = calloc(1, (size_t)payload);  // zeros if the stream ends early
		rd_bytes(p, tmp, (size_t)payload);
		if (id == 0xE0) {
			if (pts >= 0 && p->video_es.len == 0) p->video_pts = pts;
			buf_append(&p->video_es, tmp, (size_t)payload);
		}
		else if (payload > 4 && tmp[0] == 0x00) {  // ATRAC3+ channel 0: skip 4-byte channel header
			if (pts >= 0 && p->audio_pts0 < 0) p->audio_pts0 = pts;
			buf_append(&p->audio_es, tmp + 4, (size_t)payload - 4);
		}
		free(tmp);
		return true;
	}
}

// ---- decoding -------------------------------------------------------------------------------
static bool open_codecs(Player *p) {
	static bool reg;
	if (!reg) {
		avcodec_register_all();
		av_log_set_level(AV_LOG_QUIET);
		reg = true;
	}
	AVCodec *vc = avcodec_find_decoder(AV_CODEC_ID_H264);
	p->vctx = avcodec_alloc_context3(vc);
	if (avcodec_open2(p->vctx, vc, NULL) < 0) return false;
	p->parser = av_parser_init(AV_CODEC_ID_H264);
	p->vframe = av_frame_alloc();
	p->aframe = av_frame_alloc();
	return true;
}

// Decodes the next video frame; false when the stream is over.
static bool next_video_frame(Player *p) {
	for (;;) {
		while (p->video_es.len > 0) {
			uint8_t *out = NULL;
			int out_size = 0;
			int used = av_parser_parse2(p->parser, p->vctx, &out, &out_size, p->video_es.data, (int)p->video_es.len,
			                            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			bool got_frame = false;
			if (out_size > 0) {
				// `out` may point into video_es itself: copy it (zero-padded, as FFmpeg requires)
				// before the buffer is shifted.
				static uint8_t *pkt_buf;
				static int pkt_cap;
				if (out_size + AV_INPUT_BUFFER_PADDING_SIZE > pkt_cap) {
					pkt_cap = (out_size + AV_INPUT_BUFFER_PADDING_SIZE) * 2;
					pkt_buf = realloc(pkt_buf, (size_t)pkt_cap);
				}
				memcpy(pkt_buf, out, (size_t)out_size);
				memset(pkt_buf + out_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
				AVPacket pkt;
				av_init_packet(&pkt);
				pkt.data = pkt_buf;
				pkt.size = out_size;
				int got = 0;
				if (avcodec_decode_video2(p->vctx, p->vframe, &got, &pkt) >= 0 && got) {
					p->frame_pts = (int64_t)p->first_ts + (int64_t)p->frame_index++ * 3003;
					p->have_frame = true;
					TRACE("psmf: frame pts %lld (%dx%d)", (long long)p->frame_pts, p->vframe->width, p->vframe->height);
					got_frame = true;
				}
			}
			buf_consume(&p->video_es, (size_t)used);
			if (got_frame) return true;
			if (used == 0) break;
		}
		if (!demux_one(p)) {
			// Flush the parser/decoder at the end of the stream.
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = NULL;
			pkt.size = 0;
			int got = 0;
			if (avcodec_decode_video2(p->vctx, p->vframe, &got, &pkt) >= 0 && got) {
				p->frame_pts = (int64_t)p->first_ts + (int64_t)p->frame_index++ * 3003;
				p->have_frame = true;
				return true;
			}
			return false;
		}
	}
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Where movie frames went lately (rt_psmf_frame_at: the texture catalog leaves them out).
static struct { uint32_t addr, bytes; uint64_t us; } recent[4];
bool rt_psmf_frame_at(uint32_t addr) {
	uint64_t now = rt_now_us();
	addr &= 0x0FFFFFFFu;
	for (int i = 0; i < 4; i++)
		if (recent[i].bytes && now - recent[i].us < 3000000 && addr - recent[i].addr < recent[i].bytes) return true;
	return false;
}

static bool hd_frame_for(Player *p, uint32_t n);

static void write_frame(Player *p, uint32_t dst, int stride) {
	AVFrame *f = p->vframe;
	int w = f->width < 480 ? f->width : 480, h = f->height < 272 ? f->height : 272;
	if (!rt_valid(dst, (uint32_t)(stride * h * 4))) return;
	{
		int i = 0;
		while (i < 3 && recent[i].addr != (dst & 0x0FFFFFFFu)) i++;
		if (recent[i].addr != (dst & 0x0FFFFFFFu)) memmove(recent + 1, recent, sizeof recent[0] * 3), i = 0;
		recent[i].addr = dst & 0x0FFFFFFFu, recent[i].bytes = (uint32_t)(stride * h * 4), recent[i].us = rt_now_us();
	}
	uint32_t *o = (uint32_t *)MP(dst);
	if (p->hide) {
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) o[y * stride + x] = 0xFF000000u;
		return;
	}
	for (int y = 0; y < h; y++) {
		const uint8_t *Y = f->data[0] + y * f->linesize[0];
		const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
		const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
		for (int x = 0; x < w; x++) {
			int c = (Y[x] - 16) * 298, d = U[x / 2] - 128, e = V[x / 2] - 128;
			uint8_t r = clamp8((c + 409 * e + 128) >> 8);
			uint8_t g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
			uint8_t b = clamp8((c + 516 * d + 128) >> 8);
			o[y * stride + x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
		}
	}
	// its HD picture (a whole multiple of the frame's size), drawn in its place
	if (w > 0 && h > 0 && p->frame_index && hd_frame_for(p, p->frame_index - 1) && p->hd_w % w == 0 && p->hd_w / w == p->hd_h / h) {
		uint32_t a = dst & 0x0FFFFFFFu;
		int k = 0;
		while (k < 4 && p->hd_dst[k] != a) k++;
		if (k == 4) {  // a buffer not seen yet
			k = 0;
			while (k < 4 && p->hd_dst[k]) k++;
			if (k < 4) p->hd_dst[k] = a;
		}
		ge_movie_hd(a, p->hd_rgba, p->hd_w, p->hd_h, w, h);  // its picture stays in the ring for four frames
	}
}

// The HD frame that goes with the game's frame number n (0-based): decoded on from the last one
// (a stream that arrived late, on the web, catches up), converted to RGBA. False: none.
static bool hd_frame_for(Player *p, uint32_t n) {
	if (p->hd_off) return false;
	if (!p->hd_es) {
		uint8_t *d = texpack_file(p->hd_name, &p->hd_n, &p->hd_state);
		if (!d || !p->hd_n) {  // not there (or no pack; the page's server answers an empty file)
			free(d);
			if (p->hd_state != 1 || d) p->hd_off = true;
			return false;
		}
		p->hd_es = realloc(d, p->hd_n + AV_INPUT_BUFFER_PADDING_SIZE);
		memset(p->hd_es + p->hd_n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		AVCodec *vc = avcodec_find_decoder(AV_CODEC_ID_H264);
		p->hd_ctx = avcodec_alloc_context3(vc);
		if (avcodec_open2(p->hd_ctx, vc, NULL) < 0) {
			p->hd_off = true;
			return false;
		}
		p->hd_parser = av_parser_init(AV_CODEC_ID_H264);
		p->hd_frame = av_frame_alloc();
		rt_log("psmf: HD version %s (%zu bytes)", p->hd_name, p->hd_n);
	}
	bool fresh = false;
	while (p->hd_count <= n) {
		uint8_t *out = NULL;
		int out_size = 0, got = 0;
		int used = av_parser_parse2(p->hd_parser, p->hd_ctx, &out, &out_size, p->hd_es + p->hd_pos, (int)(p->hd_n - p->hd_pos),
		                            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		p->hd_pos += (size_t)used;
		AVPacket pkt;
		av_init_packet(&pkt);
		if (out_size > 0) pkt.data = out, pkt.size = out_size;
		else if (p->hd_pos >= p->hd_n) pkt.data = NULL, pkt.size = 0;  // flush the last ones
		else continue;
		if (avcodec_decode_video2(p->hd_ctx, p->hd_frame, &got, &pkt) >= 0 && got) p->hd_count++, fresh = true;
		else if (!pkt.data) break;  // the stream is over: its last frame stays
	}
	if (!p->hd_count) return false;
	AVFrame *f = p->hd_frame;
	if (fresh) {
		if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
			rt_log("psmf: %s: not 4:2:0 YUV, left out", p->hd_name);
			p->hd_off = true;
			return false;
		}
		if (p->hd_w != f->width || p->hd_h != f->height) {
			for (int i = 0; i < 4; i++) free(p->hd_ring[i]), p->hd_ring[i] = NULL;
			p->hd_w = f->width, p->hd_h = f->height;
		}
		uint32_t **slot = &p->hd_ring[p->hd_count % 4];
		if (!*slot) *slot = malloc((size_t)p->hd_w * p->hd_h * 4);
		p->hd_rgba = *slot;
		for (int y = 0; y < p->hd_h; y++) {
			const uint8_t *Y = f->data[0] + y * f->linesize[0];
			const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
			const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
			uint32_t *o = p->hd_rgba + (size_t)y * p->hd_w;
			for (int x = 0; x < p->hd_w; x++) {
				int c = (Y[x] - 16) * 298, d = U[x / 2] - 128, e = V[x / 2] - 128;
				o[x] = 0xFF000000u | (uint32_t)clamp8((c + 516 * d + 128) >> 8) << 16 | (uint32_t)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8 |
				       clamp8((c + 409 * e + 128) >> 8);
			}
		}
	}
	return true;
}

// Playback clock (90 kHz). Audio is the master, as on the PSP: once the game fetches audio, the
// clock is the PTS of the sample being heard now, so the picture follows the sound whatever the
// output latency. Before that (or for a movie without audio) it runs from scePsmfPlayerStart.
#define AUDIO_FRAME_US (2048 * 1000000ull / 44100)
static uint64_t clock_pts(Player *p) {
	uint64_t now = rt_now_us();
	if (p->audio_frames) {
		if (now < p->audio_t0_us) return (uint64_t)p->audio_pts0;
		return (uint64_t)p->audio_pts0 + (now - p->audio_t0_us) * 9 / 100;
	}
	if (now - p->start_us < 500000) return p->first_ts;  // wait a moment for the audio to start
	return (uint64_t)p->first_ts + (now - p->start_us - 500000) * 9 / 100;
}

// A frame was handed to the game: it is heard after the frames before it, or (if the game was
// late and the channel ran dry) after the host's output latency from now.
static void audio_delivered(Player *p) {
	uint64_t now = rt_now_us(), lat = audio_output_latency_us();
	if (p->audio_pts0 < 0) p->audio_pts0 = p->first_ts;
	uint64_t due = p->audio_t0_us + p->audio_frames * AUDIO_FRAME_US;
	if (!p->audio_frames || due < now + lat) p->audio_t0_us = now + lat - p->audio_frames * AUDIO_FRAME_US;
	p->audio_frames++;
	if (rt_trace || getenv("DBZ_FPS")) {
		if (now - p->log_us >= 2000000) {
			p->log_us = now;
			rt_log("psmf: video pts %lld, audio heard %lld (output latency %llu ms)", (long long)p->frame_pts,
			       (long long)clock_pts(p), (unsigned long long)(lat / 1000));
		}
	}
}

// ---- HLE ----------------------------------------------------------------------------------
// Mods can have the next movie end as soon as it starts (the opening at boot).
static bool skip_next, hide_next;
void rt_psmf_skip_next(void) { skip_next = true; }
void rt_psmf_hide_next(void) { hide_next = true; }
bool rt_psmf_hidden_playing(void) {
	for (int i = 0; i < 4; i++)
		if (players[i].addr && players[i].hide && players[i].state == ST_PLAYING) return true;
	return false;
}

void hle_scePsmfPlayerCreate(CPU *c) {
	Player *p = alloc(A0);
	player_close(p);
	p->state = ST_INIT;
	RET(0);
}

void hle_scePsmfPlayerSetPsmf(CPU *c) {
	Player *p = alloc(A0);
	const char *path = rt_str(A1);
	uint64_t off;
	uint32_t size;
	if (!io_disc_lookup(path, &off, &size)) {
		rt_log("scePsmfPlayerSetPsmf(%s): not found", path);
		RET(0x80616001u);
		return;
	}
	uint8_t hdr[0x800];
	io_iso_read(off, hdr, sizeof hdr);
	uint32_t data_off = (uint32_t)hdr[8] << 24 | hdr[9] << 16 | hdr[10] << 8 | hdr[11];
	// First/last timestamps are 48-bit big-endian (90 kHz).
	p->first_ts = (uint32_t)hdr[0x56] << 24 | hdr[0x57] << 16 | hdr[0x58] << 8 | hdr[0x59];
	p->last_ts = (uint32_t)hdr[0x5C] << 24 | hdr[0x5D] << 16 | hdr[0x5E] << 8 | hdr[0x5F];
	p->file_off = off + data_off;
	p->end = size - data_off;
	p->audio_pts0 = -1;
	if (!open_codecs(p)) {
		RET(0x80616001u);
		return;
	}
	p->state = ST_STANDBY;
	{  // its HD version's name: movies/<the file's name without .pmf>.h264
		const char *b = strrchr(path, '/');
		b = b ? b + 1 : path;
		int n = (int)strcspn(b, ".");
		snprintf(p->hd_name, sizeof p->hd_name, "movies/%.*s.h264", n, b);
		p->hd_state = 0, p->hd_off = false;
		size_t dummy;
		uint8_t *d = texpack_file(p->hd_name, &dummy, &p->hd_state);  // the web starts fetching it now
		free(d);
	}
	p->skip = skip_next;
	p->hide = hide_next;
	skip_next = hide_next = false;
	rt_log("scePsmfPlayerSetPsmf(%s): %u bytes, %.1f s%s", path, size, (p->last_ts - p->first_ts) / 90000.0,
	       p->skip ? " (skipped)" : "");
	RET(0);
}

void hle_scePsmfPlayerGetPsmfInfo(CPU *c) {
	Player *p = find(A0);
	uint32_t i = A1;
	M32(i + 0) = p ? p->last_ts : 0;
	M32(i + 4) = 1;
	M32(i + 8) = 1;
	M32(i + 12) = 0;
	M32(i + 16) = 1;
	RET(0);
}

void hle_scePsmfPlayerStart(CPU *c) {
	Player *p = find(A0);
	if (!p) {
		RET(0x80616001u);
		return;
	}
	p->state = ST_PLAYING;
	p->start_us = rt_now_us();
	p->audio_frames = 0;
	RET(0);
}

void hle_scePsmfPlayerGetCurrentStatus(CPU *c) {
	Player *p = find(A0);
	if (!p) {
		RET(ST_NONE);
		return;
	}
	if (p->state == ST_PLAYING && (p->video_done || p->skip)) p->state = ST_FINISHED;
	RET(p->state);
}

void hle_scePsmfPlayerUpdate(CPU *c) { RET(0); }

// Struct passed by the game: { int frameWidth; u32 displaybuf; u32 displaypts; }
void hle_scePsmfPlayerGetVideoData(CPU *c) {
	Player *p = find(A0);
	uint32_t vd = A1;
	if (!p || p->state != ST_PLAYING) {
		RET(ERR_NO_DATA);
		return;
	}
	uint64_t now = clock_pts(p);
	bool updated = false;
	while (!p->video_done && (!p->have_frame || (uint64_t)p->frame_pts < now)) {
		if (!next_video_frame(p)) {
			p->video_done = true;
			break;
		}
		updated = true;
		if ((uint64_t)p->frame_pts >= now) break;
	}
	if (p->have_frame && (updated || true)) {
		int stride = (int)M32(vd);
		if (stride <= 0) stride = 512;
		TRACE("psmf: video -> %08x stride %d", M32(vd + 4), stride);
		write_frame(p, M32(vd + 4), stride);
		M32(vd + 8) = (uint32_t)p->frame_pts;
	}
	RET(p->have_frame ? 0 : ERR_NO_DATA);
}

void hle_scePsmfPlayerGetAudioOutSize(CPU *c) { RET(0x2000); }

void hle_scePsmfPlayerGetAudioData(CPU *c) {
	Player *p = find(A0);
	uint32_t out = A1;
	if (out) memset(MP(out), 0, 0x2000);
	if (!p || p->state != ST_PLAYING) {
		RET(ERR_NO_DATA);
		return;
	}
	// Make sure a complete ATRAC3+ frame (0F D0 header + payload) is buffered.
	for (int guard = 0; guard < 64; guard++) {
		if (p->audio_es.len >= 8 && p->audio_es.data[0] == 0x0F && p->audio_es.data[1] == 0xD0) {
			size_t fsize = ((size_t)(p->audio_es.data[2] & 3) << 8 | (size_t)p->audio_es.data[3] * 8) + 0x10;
			if (p->audio_es.len >= fsize) {
				if (!p->actx) {
					AVCodec *ac = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
					p->actx = avcodec_alloc_context3(ac);
					p->actx->channels = 2;
					p->actx->channel_layout = AV_CH_LAYOUT_STEREO;
					p->actx->sample_rate = 44100;
					p->actx->block_align = (int)fsize - 8;
					avcodec_open2(p->actx, ac, NULL);
				}
				AVPacket pkt;
				av_init_packet(&pkt);
				pkt.data = p->audio_es.data + 8;
				pkt.size = (int)fsize - 8;
				int got = 0;
				if (avcodec_decode_audio4(p->actx, p->aframe, &got, &pkt) >= 0 && got && out) {
					int16_t *o = (int16_t *)MP(out);
					int n = p->aframe->nb_samples > 2048 ? 2048 : p->aframe->nb_samples;
					int ch = p->aframe->channels;
					for (int i = 0; i < n; i++) {
						for (int k = 0; k < 2; k++) {
							float v = ((float *)p->aframe->extended_data[ch > 1 ? k : 0])[i] * 32767.f;
							o[i * 2 + k] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
						}
					}
				}
				buf_consume(&p->audio_es, fsize);
				if (p->hide && out) memset(MP(out), 0, 0x2000);
				audio_delivered(p);
				RET(0);
				return;
			}
		}
		else if (p->audio_es.len >= 2) {  // resync to the next frame header
			size_t i = 1;
			while (i + 1 < p->audio_es.len && !(p->audio_es.data[i] == 0x0F && p->audio_es.data[i + 1] == 0xD0)) i++;
			buf_consume(&p->audio_es, i);
			continue;
		}
		if (!demux_one(p)) break;
	}
	RET(ERR_NO_DATA);
}

void hle_scePsmfPlayerStop(CPU *c) {
	Player *p = find(A0);
	if (p) p->state = ST_STANDBY;
	RET(0);
}
void hle_scePsmfPlayerBreak(CPU *c) { RET(0); }
void hle_scePsmfPlayerDelete(CPU *c) {
	Player *p = find(A0);
	if (p) {
		player_close(p);
		p->addr = 0;
	}
	RET(0);
}
void hle_scePsmfPlayerReleasePsmf(CPU *c) {
	Player *p = find(A0);
	if (p) {
		uint32_t a = p->addr;
		player_close(p);
		p->addr = a;
		p->state = ST_INIT;
	}
	RET(0);
}
void hle_scePsmfPlayerChangePlayMode(CPU *c) { RET(0); }
void hle_scePsmfPlayerConfigPlayer(CPU *c) { RET(0); }
void hle_sceMpegInit(CPU *c) { RET(0); }
void hle_sceMpegFinish(CPU *c) { RET(0); }

void hle_scePsmfPlayerSetPsmfCB(CPU *c) { hle_scePsmfPlayerSetPsmf(c); }

// (player, u64 *pts): the shown video frame's time stamp (90 kHz)
void hle_scePsmfPlayerGetCurrentPts(CPU *c) {
	Player *p = find(A0);
	if (!p || p->state < ST_STANDBY) {
		RET(0x80616001u);  // SCE_PSMFPLAYER_ERROR_INVALID_STATUS
		return;
	}
	if (!p->have_frame) {
		RET(ERR_NO_DATA);
		return;
	}
	M32(A1) = (uint32_t)p->frame_pts;
	M32(A1 + 4) = (uint32_t)((uint64_t)p->frame_pts >> 32);
	RET(0);
}
