// A movie's HD version from the texture pack (movies/<name>.h264), drawn in place of the game's own
// frames: decoded here with FFmpeg natively, and by the browser itself in the page (web/hdmovie.js:
// WebCodecs, off the game's thread, each picture straight into its texture). One file for every PSP
// port (with hdmovie.c and web/hdmovie.js), kept in sync by hand; the movie players use it
// (runtime/psmf.c for scePsmfPlayer, Burnout's runtime/mpeg.c for low-level sceMpeg).
//
// The stream is H.264 Annex B, a picture per frame of the game's movie (the same frames, at a whole
// multiple of their size), no B-frames. It may start with a table of the game's frames' fingerprints
// (an SEI message, user data unregistered with the UUID "BLHDmovietable01": a u32 count, then u64s),
// for a player that tells frames by their contents (hdmovie_find); otherwise frame k is the movie's
// k-th.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVCodecContext;
struct AVFrame;

typedef struct {
	char name[96];      // movies/<name>.h264
	int state;          // texpack_file's (the web fetches it while the movie starts)
	bool off;           // none, unusable, or it stopped decoding
	uint64_t tried_us;  // natively looked for again now and then (the texture lab may be making it)
	uint8_t *data;      // the whole file, padded
	size_t n;
	uint64_t *table;    // the game's frames' fingerprints, one per frame, or NULL
	int count;          // frames
	uint32_t *au;       // the frames' access units: bytes au[k] .. au[k + 1] of data
	uint8_t *idr;       // ... whether frame k starts afresh (an IDR picture: decoding can begin there)
	struct AVCodecContext *ctx;
	struct AVFrame *frame;
	int js;             // in the browser: the page's decoder of it, 0 none
	int next;           // natively: the frame the decoder takes next
	int last;           // the frame shown last (-1: none)
	int w, h;
	uint32_t *ring[4];  // natively: the last four frames in RGBA (the game draws a buffer written a frame or two ago)
	int ring_k[4];
	uint32_t dst[4];    // the game's buffers given its pictures (ge.c is told when it ends)
	uint32_t shown;     // frames drawn from its pictures (the log)
} HdMovie;

// A movie starts: its HD version is looked for (the page starts fetching it). name: the movie file's
// name without its folder and extension (movies/<name>.h264).
void hdmovie_init(HdMovie *h, const char *name);
// It is there and usable (opened the first time it is; false while the page is still fetching it).
bool hdmovie_open(HdMovie *h);
// The frame whose fingerprint (from the stream's table) is fp, searched from the last one shown; -1: none.
int hdmovie_find(HdMovie *h, uint64_t fp);
// Frame k (past the last: the last) drawn in place of the game's frame in its buffer at addr (the
// game's iw x ih picture there); false: not (none, not decoded yet, or not a whole multiple of it).
bool hdmovie_show(HdMovie *h, int k, uint32_t addr, int iw, int ih);
// The buffer at addr no longer shows this movie's pictures; true if it did (the caller tells ge.c).
bool hdmovie_forget(HdMovie *h, uint32_t addr);
// The movie is over: its buffers back to the game's own frames, everything freed.
void hdmovie_close(HdMovie *h);

// ge.c (declared here too: not every port's rt.h has them)
void ge_movie_hd(uint32_t addr, const uint32_t *px, int w, int h, int iw, int ih);
// ge.c: the HD picture put into its texture by fill(arg) when the game draws it (the browser's).
void ge_movie_hd_fill(uint32_t addr, bool (*fill)(uintptr_t arg), uintptr_t arg, int w, int h, int iw, int ih);
