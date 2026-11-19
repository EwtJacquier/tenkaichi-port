// sceAtrac3plus: music and voice streams (ATRAC3 / ATRAC3+ in RIFF WAVE), decoded with FFmpeg.
//
// The PSP API streams the file through a guest buffer that the game refills. When the buffer was
// read straight from a disc file (io_find_source, and the disc's bytes match), the runtime loads
// the whole RIFF from there and tells the game that everything is in memory (remainFrame = -1),
// so it never needs to stream. Otherwise (the data came through the game's own archives) it
// streams: the game is asked for the file's next bytes, one buffer at a time, always written at
// the buffer's start (GetStreamDataInfo / AddStreamData), and the runtime keeps a copy of the
// whole file as it arrives; remainFrame tells the game how many frames it has left.
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

#define ERR_NO_ID 0x80630003u
#define ERR_ALL_DECODED 0x80630024u
#define ERR_BAD_ID 0x80630005u

typedef struct {
	bool used;
	uint8_t *file;      // whole RIFF
	uint32_t file_size;
	uint32_t data_off, data_size, block_align;
	uint32_t fmt_off, fmt_size;
	int channels, codec;  // codec: 0x270 ATRAC3, 0xFFFE ATRAC3+
	int out_channels;     // what DecodeData writes: 2 for sceAtracSetDataAndGetID (mono doubled), as the PSP
	int frame_samples;
	uint32_t total_frames, cur_frame;
	int loop_start_frame, loop_end_frame;  // -1 = no loop info
	int loop_num;                          // -1 infinite
	uint32_t guest_buf, buf_size;
	bool streaming;   // the game feeds the file in pieces
	uint32_t filled;  // how much of the file has arrived
	AVCodecContext *ctx;
	AVFrame *frame;
} Atrac;

static Atrac atracs[8];

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool parse_riff(Atrac *a) {
	const uint8_t *f = a->file;
	if (a->file_size < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4)) return false;
	uint32_t pos = 12;
	a->loop_start_frame = a->loop_end_frame = -1;
	while (pos + 8 <= a->file_size) {
		uint32_t sz = rd32(f + pos + 4);
		const uint8_t *body = f + pos + 8;
		if (!memcmp(f + pos, "fmt ", 4)) {
			a->fmt_off = pos + 8;
			a->fmt_size = sz;
			a->codec = rd16(body);
			a->channels = rd16(body + 2);
			a->block_align = rd16(body + 12);
		}
		else if (!memcmp(f + pos, "smpl", 4) && sz >= 36 + 24 && rd32(body + 28) > 0) {
			uint32_t start = rd32(body + 36 + 8), end = rd32(body + 36 + 12);
			a->loop_start_frame = (int)(start / 2048);
			a->loop_end_frame = (int)(end / 2048);
		}
		else if (!memcmp(f + pos, "data", 4)) {
			a->data_off = pos + 8;
			a->data_size = sz;
			if (a->data_off + a->data_size > a->file_size) a->data_size = a->file_size - a->data_off;
			break;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!a->data_off || !a->block_align) return false;
	a->frame_samples = a->codec == 0x270 ? 1024 : 2048;
	if (a->codec == 0x270) {
		a->loop_start_frame = a->loop_start_frame < 0 ? -1 : a->loop_start_frame * 2;
		a->loop_end_frame = a->loop_end_frame < 0 ? -1 : a->loop_end_frame * 2;
	}
	a->total_frames = a->data_size / a->block_align;
	return true;
}

static bool open_decoder(Atrac *a) {
	static bool registered;
	if (!registered) {
		avcodec_register_all();
		av_log_set_level(AV_LOG_QUIET);
		registered = true;
	}
	AVCodec *codec = avcodec_find_decoder(a->codec == 0x270 ? AV_CODEC_ID_ATRAC3 : AV_CODEC_ID_ATRAC3P);
	if (!codec) return false;
	a->ctx = avcodec_alloc_context3(codec);
	a->ctx->channels = a->channels;
	a->ctx->channel_layout = a->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
	a->ctx->sample_rate = 44100;
	a->ctx->block_align = (int)a->block_align;
	if (a->codec == 0x270) {  // ATRAC3 needs the 14-byte codec extradata that follows cbSize in fmt
		static const uint8_t fallback[14] = {1, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
		const uint8_t *extra = a->fmt_size >= 18 + 14 ? a->file + a->fmt_off + 18 : fallback;
		a->ctx->extradata = av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(a->ctx->extradata, extra, 14);
		a->ctx->extradata_size = 14;
	}
	if (avcodec_open2(a->ctx, codec, NULL) < 0) return false;
	a->frame = av_frame_alloc();
	return true;
}

static void atrac_free(Atrac *a) {
	if (a->ctx) avcodec_free_context(&a->ctx);
	if (a->frame) av_frame_free(&a->frame);
	free(a->file);
	memset(a, 0, sizeof *a);
}

void hle_sceAtracSetDataAndGetID(CPU *c) {
	uint32_t buf = A0, bufsize = A1;
	int id = -1;
	for (int i = 0; i < 8; i++) {
		if (!atracs[i].used) {
			id = i;
			break;
		}
	}
	if (id < 0) {
		RET(ERR_NO_ID);
		return;
	}
	Atrac *a = &atracs[id];
	memset(a, 0, sizeof *a);
	if (!rt_valid(buf, 12) || memcmp(MP(buf), "RIFF", 4)) {
		rt_log("sceAtracSetDataAndGetID(%08x): not a RIFF buffer", buf);
		RET(0x80630011u);
		return;
	}
	uint32_t riff_size = rd32(MP(buf + 4)) + 8;
	a->file = calloc(1, (size_t)riff_size + AV_INPUT_BUFFER_PADDING_SIZE);  // FFmpeg reads past the last frame
	a->file_size = riff_size;
	uint64_t iso_off;
	a->filled = riff_size;
	if (riff_size <= bufsize) {
		memcpy(a->file, MP(buf), riff_size);
	}
	else {
		uint32_t check = bufsize < 2048 ? bufsize : 2048;
		if (!io_find_source(buf, &iso_off) || (io_iso_read(iso_off, a->file, riff_size), memcmp(a->file, MP(buf), check))) {
			memset(a->file, 0, riff_size);  // not a plain disc file: the game streams it
			memcpy(a->file, MP(buf), bufsize);
			a->streaming = true;
			a->filled = bufsize;
		}
	}
	a->guest_buf = buf;
	a->buf_size = bufsize;
	a->loop_num = 0;
	// sceAtracSetDataAndGetID decodes to stereo whatever the stream has (PPSSPP: outputChannels 2;
	// only the sceAtracSetMOut* calls give mono). Mono streams written as mono were read as stereo,
	// at twice the speed with old data in between.
	a->out_channels = 2;
	if (!parse_riff(a) || !open_decoder(a)) {
		rt_log("sceAtracSetDataAndGetID(%08x, %x): unsupported stream (codec %x, RIFF %x bytes, first %02x%02x%02x%02x %02x%02x%02x%02x)", buf, bufsize,
		       a->codec, riff_size, a->file[12], a->file[13], a->file[14], a->file[15], MP(buf)[12], MP(buf)[13], MP(buf)[14], MP(buf)[15]);
		atrac_free(a);
		RET(0x80630011u);
		return;
	}
	a->used = true;
	TRACE("sceAtracSetDataAndGetID(%08x, %x) -> %d: codec %x ch %d frames %u loop %d-%d", buf, bufsize, id, a->codec,
	      a->channels, a->total_frames, a->loop_start_frame, a->loop_end_frame);
	RET(id);
}

static Atrac *get(uint32_t id) { return id < 8 && atracs[id].used ? &atracs[id] : NULL; }

void hle_sceAtracReleaseAtracID(CPU *c) {
	Atrac *a = get(A0);
	if (a) atrac_free(a);
	RET(a ? 0 : ERR_BAD_ID);
}

// Frames the game has supplied beyond the next one to decode; -1 once the whole file is here.
static uint32_t remain_frames(Atrac *a) {
	if (!a->streaming || a->filled >= a->file_size) return 0xFFFFFFFFu;
	uint32_t have = a->filled > a->data_off ? (a->filled - a->data_off) / a->block_align : 0;
	return have > a->cur_frame ? have - a->cur_frame : 0;
}

void hle_sceAtracDecodeData(CPU *c) {
	Atrac *a = get(A0);
	uint32_t out = A1, nsamp = A2, finish = A3, remain = A4;
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	if (a->loop_end_frame >= 0 && a->loop_num != 0 && (int)a->cur_frame > a->loop_end_frame) {
		a->cur_frame = (uint32_t)a->loop_start_frame;
		if (a->loop_num > 0) a->loop_num--;
	}
	if (a->cur_frame >= a->total_frames) {
		if (nsamp) M32(nsamp) = 0;
		if (finish) M32(finish) = 1;
		if (remain) M32(remain) = 0xFFFFFFFFu;
		RET(ERR_ALL_DECODED);
		return;
	}
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = a->file + a->data_off + a->cur_frame * a->block_align;
	pkt.size = (int)a->block_align;
	int got = 0;
	int samples = 0;
	if (avcodec_decode_audio4(a->ctx, a->frame, &got, &pkt) >= 0 && got) {
		samples = a->frame->nb_samples;
		int16_t *o = out ? (int16_t *)MP(out) : NULL;
		int ch = a->channels, och = a->out_channels;
		for (int i = 0; o && i < samples; i++) {
			for (int k = 0; k < och; k++) {
				float v = ((float *)a->frame->extended_data[k < ch ? k : ch - 1])[i] * 32767.f;
				o[i * och + k] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
			}
		}
	}
	else if (out) {
		memset(MP(out), 0, (size_t)a->frame_samples * 2 * (size_t)a->out_channels);
		samples = a->frame_samples;
	}
	a->cur_frame++;
	bool more = a->cur_frame < a->total_frames || (a->loop_end_frame >= 0 && a->loop_num != 0);
	if (nsamp) M32(nsamp) = (uint32_t)samples;
	if (finish) M32(finish) = more ? 0 : 1;
	if (remain) M32(remain) = remain_frames(a);
	RET(0);
}

// (id, u32 *write to, u32 *bytes to write, u32 *file offset to read them from)
void hle_sceAtracGetStreamDataInfo(CPU *c) {
	Atrac *a = get(A0);
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	uint32_t left = a->streaming && a->filled < a->file_size ? a->file_size - a->filled : 0;
	if (A1) M32(A1) = a->guest_buf;
	if (A2) M32(A2) = left < a->buf_size ? left : a->buf_size;
	if (A3) M32(A3) = a->streaming ? a->filled : a->file_size;
	RET(0);
}
// (id, bytes): the game wrote the next bytes of the file at the buffer's start
void hle_sceAtracAddStreamData(CPU *c) {
	Atrac *a = get(A0);
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	uint32_t n = A1;
	if (a->streaming && n) {
		if (n > a->buf_size) n = a->buf_size;
		if (a->filled + n > a->file_size) n = a->file_size - a->filled;
		memcpy(a->file + a->filled, MP(a->guest_buf), n);
		a->filled += n;
	}
	RET(0);
}
void hle_sceAtracGetRemainFrame(CPU *c) {
	Atrac *a = get(A0);
	if (A1) M32(A1) = a ? remain_frames(a) : 0;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracSetLoopNum(CPU *c) {
	Atrac *a = get(A0);
	if (a) a->loop_num = (int)A1;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracGetLoopStatus(CPU *c) {
	Atrac *a = get(A0);
	if (A1) M32(A1) = a ? (uint32_t)a->loop_num : 0;
	if (A2) M32(A2) = a && a->loop_end_frame >= 0 ? 1 : 0;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracGetInternalErrorInfo(CPU *c) {
	if (A1) M32(A1) = 0;
	RET(0);
}

// The sample the next sceAtracDecodeData returns.
void hle_sceAtracGetNextDecodePosition(CPU *c) {
	Atrac *a = get(A0);
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	if (a->cur_frame >= a->total_frames) {
		RET(ERR_ALL_DECODED);
		return;
	}
	if (A1) M32(A1) = a->cur_frame * (uint32_t)a->frame_samples;
	RET(0);
}

// Seeking: the game asks where to refill its buffer for a new position, writes that data, then
// resets. What has arrived stays here, so only a position past it asks for more.
void hle_sceAtracGetBufferInfoForResetting(CPU *c) {
	Atrac *a = get(A0);
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	uint32_t p = A2;
	if (p) {
		uint32_t frame = a->frame_samples ? A1 / (uint32_t)a->frame_samples : 0;
		uint32_t pos = a->data_off + frame * a->block_align;
		uint32_t want = 0;  // a position past what has arrived: the next bytes, as when streaming
		if (a->streaming && a->filled < a->file_size && pos + a->block_align > a->filled) {
			want = a->file_size - a->filled < a->buf_size ? a->file_size - a->filled : a->buf_size;
			pos = a->filled;
		}
		M32(p) = a->guest_buf;  // write position (first buffer)
		M32(p + 4) = want;      // writable bytes
		M32(p + 8) = want;      // minimum bytes to write
		M32(p + 12) = pos;      // file offset to read from
		M32(p + 16) = a->guest_buf;
		M32(p + 20) = 0;
		M32(p + 24) = 0;
		M32(p + 28) = pos;
	}
	RET(0);
}
void hle_sceAtracResetPlayPosition(CPU *c) {
	Atrac *a = get(A0);
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	a->cur_frame = a->frame_samples ? A1 / (uint32_t)a->frame_samples : 0;
	if (a->streaming && A2 && a->filled < a->file_size) {  // (id, sample, bytes written, bytes in the second buffer)
		uint32_t n = A2 < a->buf_size ? A2 : a->buf_size;
		if (a->filled + n > a->file_size) n = a->file_size - a->filled;
		memcpy(a->file + a->filled, MP(a->guest_buf), n);
		a->filled += n;
	}
	RET(0);
}

// The file is kept whole here (see the top): a second buffer is never needed.
void hle_sceAtracIsSecondBufferNeeded(CPU *c) { RET(0); }
void hle_sceAtracSetSecondBuffer(CPU *c) { RET(0); }
void hle_sceAtracReinit(CPU *c) { RET(0); }  // (ATRAC3 count, ATRAC3+ count): ids are not pooled here

// Low-level decoding: the game demuxes the frames itself (its own sound streams) and hands them
// over one at a time. sceAtracGetAtracID(codec type: 0x1000 ATRAC3+, 0x1001 ATRAC3) takes an id;
// sceAtracLowLevelInitDecoder(id, {channels, output channels, bytes per frame}) opens it.
void hle_sceAtracGetAtracID(CPU *c) {
	for (int i = 0; i < (int)(sizeof atracs / sizeof atracs[0]); i++) {
		if (!atracs[i].used) {
			memset(&atracs[i], 0, sizeof atracs[i]);
			atracs[i].used = true;
			atracs[i].codec = A0 == 0x1001 ? 0x270 : 0xFFFE;
			RET((uint32_t)i);
			return;
		}
	}
	RET(ERR_NO_ID);
}

void hle_sceAtracLowLevelInitDecoder(CPU *c) {
	Atrac *a = get(A0);
	if (!a || !rt_valid(A1, 12)) {
		RET(ERR_BAD_ID);
		return;
	}
	a->channels = (int)M32(A1);
	a->out_channels = (int)M32(A1 + 4);
	a->block_align = M32(A1 + 8);
	a->frame_samples = a->codec == 0x270 ? 1024 : 2048;
	if (a->channels < 1 || a->channels > 2) a->channels = 2;
	if (a->out_channels < 1 || a->out_channels > 2) a->out_channels = 2;
	if (a->ctx) avcodec_free_context(&a->ctx);
	if (a->frame) av_frame_free(&a->frame);
	RET(open_decoder(a) ? 0 : ERR_BAD_ID);
}

// (id, frame, u32 *bytes used, s16 *samples out, u32 *bytes written)
void hle_sceAtracLowLevelDecode(CPU *c) {
	Atrac *a = get(A0);
	uint32_t src = A1, used = A2, out = A3, written = A4;
	if (!a || !a->ctx) {
		RET(ERR_BAD_ID);
		return;
	}
	int samples = a->frame_samples, och = a->out_channels, ch = a->channels;
	int16_t *o = out ? (int16_t *)MP(out) : NULL;
	bool ok = false;
	if (src && rt_valid(src, a->block_align)) {
		uint8_t *buf = malloc(a->block_align + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(buf, MP(src), a->block_align);
		memset(buf + a->block_align, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = buf;
		pkt.size = (int)a->block_align;
		int got = 0;
		if (avcodec_decode_audio4(a->ctx, a->frame, &got, &pkt) >= 0 && got) {
			ok = true;
			samples = a->frame->nb_samples;
			int fch = a->frame->channels > 0 ? a->frame->channels : ch;
			for (int i = 0; o && i < samples; i++) {
				for (int k = 0; k < och; k++) {
					float v = ((float *)a->frame->extended_data[k < fch ? k : fch - 1])[i] * 32767.f;
					o[i * och + k] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
				}
			}
		}
		free(buf);
	}
	if (!ok && o) memset(o, 0, (size_t)samples * 2 * (size_t)och);
	if (used) M32(used) = a->block_align;
	if (written) M32(written) = (uint32_t)(samples * och * 2);
	RET(0);
}
