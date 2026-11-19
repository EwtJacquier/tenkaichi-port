// Host platform backends: platform_sdl.c (native) and platform_web.c (Emscripten / browser).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
	BTN_SELECT = 0x1, BTN_START = 0x8, BTN_UP = 0x10, BTN_RIGHT = 0x20, BTN_DOWN = 0x40, BTN_LEFT = 0x80,
	BTN_L = 0x100, BTN_R = 0x200, BTN_TRIANGLE = 0x1000, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000, BTN_SQUARE = 0x8000,
};

void plat_open(bool hidden);                                   // window/canvas + GL ES 3 / WebGL2 context
void plat_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay);  // keyboard + gamepad -> PSP buttons
void plat_drawable_size(int *w, int *h);
void plat_swap(void);
void plat_sleep(uint64_t us);
// The display's refresh, when the vblanks follow it (web, a 60 Hz display or a multiple):
// 1 = a refresh has come since the last vblank, 0 = not yet; -1 = the vblanks follow our own clock.
int plat_vsync(void);
void plat_vsync_taken(void);  // a vblank happened (either way)
void plat_display_stats(void);  // DBZ_FPS: log the frames the display showed (web)
void plat_persist(void);  // flush saved files to persistent storage (web: IndexedDB)
// Debug capture (web: F9 key; the files are POSTed to the dev server's /debug-dump).
bool plat_debug_dump_requested(void);
void plat_debug_upload(const char *path, const char *name);
bool plat_poll_settings(int *scale, int *ui_filter);  // settings changed by the user (web menu)
bool plat_poll_text(int *strings, int *ui);  // text drawing switched in the web menu (1 on, 0 off)
bool plat_poll_hd(int *on);  // the HD textures (texture pack) switched in the web menu
bool plat_poll_texfilter(int *mode);  // the filter for the game's own textures, from the web menu
// Web: fetch url from the page's server into the file path (path.failed appears if that fails).
// Native: not used (files are read from disk).
void plat_fetch(const char *url, const char *path);

// Audio output, 44.1 kHz stereo s16. The runtime pushes mixed frames as wall-clock time passes;
// the host keeps a short buffer, re-buffers after an underrun and absorbs clock drift.
bool plat_audio_open(void);
void plat_audio_push(const int16_t *frames, int n);
uint64_t plat_audio_latency_us(void);  // queued in the host + device latency

// Ad hoc transport (runtime/net.c builds the PSP's ad hoc network on it). Every running copy of
// the game is a node named by its virtual MAC; messages are delivered in order, reliably.
// Web: LiveKit data messages in a LiveKit room (web/net.js; the room is DBZ_ROOM, "dbztag" by
// default). Native: UDP on 127.0.0.1 between copies on the same machine.
enum { NET_OFF = 0, NET_CONNECTING = 1, NET_UP = 2, NET_FAILED = -1 };
void plat_net_start(const uint8_t mac[6], const char *room);  // join (again after a failure)
void plat_net_stop(void);
int plat_net_state(void);
void plat_net_send(const uint8_t *to_mac, const void *data, int len);  // to_mac NULL: everyone
// The next message: its length (cut to cap) and sender, 0 if none, -1 when node `from` left.
int plat_net_recv(uint8_t from[6], void *buf, int cap);
