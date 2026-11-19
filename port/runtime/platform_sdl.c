// Native platform: SDL2 window with an OpenGL ES 3 context, keyboard/gamepad, SDL audio.
#ifndef __EMSCRIPTEN__
#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>

#include "platform.h"
#include "rt.h"

static SDL_Window *win;
static SDL_GameController *pad;
static SDL_AudioDeviceID adev;

void plat_open(bool hidden) {
	// Under Wayland, SDL2's default (XWayland) ends up on Mesa's software rasterizer.
	if (getenv("WAYLAND_DISPLAY") && !getenv("SDL_VIDEODRIVER")) {
		SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
		rt_fatal("SDL_Init: %s", SDL_GetError());
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	win = SDL_CreateWindow("Dragon Ball Z: Tenkaichi Tag Team", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 544,
	                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
	if (!win) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
	if (!SDL_GL_CreateContext(win)) rt_fatal("GL ES 3.0 context: %s", SDL_GetError());
	SDL_GL_SetSwapInterval(0);  // the guest's vblank timing paces frames
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		if (SDL_IsGameController(i)) {
			pad = SDL_GameControllerOpen(i);
			break;
		}
	}
}

void plat_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) exit(0);
		if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
	}
	const uint8_t *k = SDL_GetKeyboardState(NULL);
	uint32_t b = 0;
	// D-pad on I/J/K/L (helpers up/down, taunts left/right).
	if (k[SDL_SCANCODE_I]) b |= BTN_UP;
	if (k[SDL_SCANCODE_K]) b |= BTN_DOWN;
	if (k[SDL_SCANCODE_J]) b |= BTN_LEFT;
	if (k[SDL_SCANCODE_L]) b |= BTN_RIGHT;
	if (k[SDL_SCANCODE_Z]) b |= BTN_CROSS;
	if (k[SDL_SCANCODE_X]) b |= BTN_CIRCLE;
	if (k[SDL_SCANCODE_A]) b |= BTN_SQUARE;
	if (k[SDL_SCANCODE_S]) b |= BTN_TRIANGLE;
	if (k[SDL_SCANCODE_Q]) b |= BTN_L;
	if (k[SDL_SCANCODE_W]) b |= BTN_R;
	if (k[SDL_SCANCODE_RETURN]) b |= BTN_START;
	if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= BTN_SELECT;
	*ax = *ay = 128;
	// PSP analog stick (walking; the menu mod lets it move through the menus too) on the arrows.
	if (k[SDL_SCANCODE_LEFT]) *ax = 0;
	if (k[SDL_SCANCODE_RIGHT]) *ax = 255;
	if (k[SDL_SCANCODE_UP]) *ay = 0;
	if (k[SDL_SCANCODE_DOWN]) *ay = 255;
	if (pad) {
		static const struct {
			SDL_GameControllerButton sb;
			uint32_t pb;
		} m[] = {{SDL_CONTROLLER_BUTTON_A, BTN_CROSS}, {SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE},
		         {SDL_CONTROLLER_BUTTON_X, BTN_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE},
		         {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R},
		         {SDL_CONTROLLER_BUTTON_START, BTN_START}, {SDL_CONTROLLER_BUTTON_BACK, BTN_SELECT},
		         {SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN},
		         {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT}};
		for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
			if (SDL_GameControllerGetButton(pad, m[i].sb)) b |= m[i].pb;
		}
		int sx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
		int sy = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
		if ((long)sx * sx + (long)sy * sy > 5000L * 5000L) {  // dead zone: a resting stick must not drift
			*ax = (uint8_t)((sx + 32768) >> 8);
			*ay = (uint8_t)((sy + 32768) >> 8);
		}
	}
	*buttons = b;
}

void plat_drawable_size(int *w, int *h) { SDL_GL_GetDrawableSize(win, w, h); }
void plat_swap(void) { SDL_GL_SwapWindow(win); }

void plat_sleep(uint64_t us) {
	if (us > 2000) usleep((useconds_t)(us - 1000));
	else if (us > 100) usleep((useconds_t)us);
}

// The window is presented without waiting for the display (swap interval 0): our clock paces it.
int plat_vsync(void) { return -1; }
void plat_vsync_taken(void) {}
void plat_display_stats(void) {}

void plat_persist(void) {}
bool plat_debug_dump_requested(void) { return false; }
void plat_debug_upload(const char *path, const char *name) { (void)path, (void)name; }
bool plat_poll_settings(int *scale, int *ui_filter) { (void)scale, (void)ui_filter; return false; }
bool plat_poll_text(int *strings, int *ui) { (void)strings, (void)ui; return false; }
bool plat_poll_hd(int *on) { (void)on; return false; }
bool plat_poll_texfilter(int *mode) { (void)mode; return false; }
void plat_fetch(const char *url, const char *path) { (void)url, (void)path; }

// Audio goes through SDL's queue: the runtime pushes frames as wall-clock time passes and the
// device pulls them. The queue is kept near `a_target` frames: after it runs dry it is primed
// with silence, and while its average level drifts away (sound card clock vs system clock) one
// frame is dropped or repeated at most every 50 ms.
static uint32_t a_target, a_device;
static double a_level;
static uint64_t a_last_fix;
static const int16_t a_zero[8192 * 2];

bool plat_audio_open(void) {
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
	SDL_AudioSpec want = {0}, have;
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!adev) return false;
	const char *t = getenv("DBZ_AUDIO_BUFFER");  // frames
	a_target = t ? (uint32_t)atoi(t) : have.samples * 2u + 768;
	a_device = have.samples;
	if (a_target > 8192) a_target = 8192;
	SDL_PauseAudioDevice(adev, 0);
	return true;
}

uint64_t plat_audio_latency_us(void) {
	return adev ? (uint64_t)((a_level > 0 ? a_level : a_target) + a_device) * 1000000ull / 44100 : 0;
}

void plat_audio_push(const int16_t *s, int n) {
	if (!adev || n <= 0) return;
	uint32_t q = SDL_GetQueuedAudioSize(adev) / 4;
	if (q == 0 || q > a_target * 4) {  // start, ran dry, or far behind after a stall: re-prime
		if (q) SDL_ClearQueuedAudio(adev);
		SDL_QueueAudio(adev, a_zero, a_target * 4);
		q = a_target;
		a_level = a_target;
	}
	a_level += (q - a_level) * 0.05;
	uint64_t now = rt_now_us();
	bool fix = now - a_last_fix > 50000;
	if (fix && a_level > a_target + 256 && n > 1) {
		n--;  // drop one frame
		a_last_fix = now;
	}
	SDL_QueueAudio(adev, s, (Uint32)n * 4);
	if (fix && a_level + 256 < a_target) {
		SDL_QueueAudio(adev, s + (n - 1) * 2, 4);  // repeat one frame
		a_last_fix = now;
	}
}

// Ad hoc transport: UDP datagrams on 127.0.0.1 between copies of the game on this machine. Each
// copy binds the first free port of NET_PORT0..+31 and sends every message to all of them; the
// header names sender and receiver (all FF: everyone), so the others drop what isn't theirs.
#define NET_PORT0 47380
#define NET_NODES 32  // copies on this machine (a 16-person match and spare)
static int net_fd = -1;
static bool net_tried;
static uint8_t net_mac[6];

static void net_sendto_all(const uint8_t *pkt, size_t n) {
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	for (int i = 0; i < NET_NODES; i++) {
		a.sin_port = htons(NET_PORT0 + i);
		sendto(net_fd, pkt, n, 0, (struct sockaddr *)&a, sizeof a);
	}
}

void plat_net_start(const uint8_t mac[6], const char *room) {
	(void)room;
	if (net_fd >= 0) return;
	net_tried = true;
	memcpy(net_mac, mac, 6);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	for (int i = 0; i < NET_NODES; i++) {
		a.sin_port = htons(NET_PORT0 + i);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0) {
			fcntl(fd, F_SETFL, O_NONBLOCK);
			net_fd = fd;
			rt_log("net: ad hoc node on udp 127.0.0.1:%d", NET_PORT0 + i);
			return;
		}
	}
	close(fd);
	rt_log("net: no free udp port in %d..%d", NET_PORT0, NET_PORT0 + NET_NODES - 1);
}

void plat_net_stop(void) {
	if (net_fd < 0) return;
	uint8_t pkt[13];
	memcpy(pkt, net_mac, 6);
	memset(pkt + 6, 0xff, 6);
	pkt[12] = 'B';  // bye
	net_sendto_all(pkt, sizeof pkt);
	close(net_fd);
	net_fd = -1;
	net_tried = false;
}

int plat_net_state(void) { return net_fd >= 0 ? NET_UP : net_tried ? NET_FAILED : NET_OFF; }

void plat_net_send(const uint8_t *to_mac, const void *data, int len) {
	uint8_t pkt[13 + 4096];
	if (net_fd < 0 || len < 0 || len > 4096) return;
	memcpy(pkt, net_mac, 6);
	if (to_mac) memcpy(pkt + 6, to_mac, 6);
	else memset(pkt + 6, 0xff, 6);
	pkt[12] = 'M';
	memcpy(pkt + 13, data, (size_t)len);
	net_sendto_all(pkt, 13 + (size_t)len);
}

// Tests of a poor connection: DBZ_NET_DELAY_MS (one way), DBZ_NET_JITTER_MS (up to that much
// more, in order) and DBZ_NET_LOSS (percent of the messages lost) on what this copy receives.
typedef struct {
	uint64_t at;
	int n;
	uint8_t pkt[13 + 4096];
} Delayed;
static Delayed *dq;
static int dq_first, dq_len, dq_cap;
static int sim_delay = -1, sim_jitter, sim_loss;

static int take(uint8_t from[6], void *buf, int cap, const uint8_t *pkt, int n) {
	memcpy(from, pkt, 6);
	if (pkt[12] == 'B') return -1;
	int len = n - 13 < cap ? n - 13 : cap;
	memcpy(buf, pkt + 13, (size_t)len);
	return len;
}

int plat_net_recv(uint8_t from[6], void *buf, int cap) {
	static const uint8_t all[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	if (sim_delay < 0) {
		sim_delay = getenv("DBZ_NET_DELAY_MS") ? atoi(getenv("DBZ_NET_DELAY_MS")) : 0;
		sim_jitter = getenv("DBZ_NET_JITTER_MS") ? atoi(getenv("DBZ_NET_JITTER_MS")) : 0;
		sim_loss = getenv("DBZ_NET_LOSS") ? atoi(getenv("DBZ_NET_LOSS")) : 0;
	}
	bool sim = sim_delay > 0 || sim_jitter > 0 || sim_loss > 0;
	uint8_t pkt[13 + 4096];
	ssize_t n;
	while (net_fd >= 0 && (n = recv(net_fd, pkt, sizeof pkt, 0)) >= 0) {
		if (n < 13 || !memcmp(pkt, net_mac, 6)) continue;
		if (memcmp(pkt + 6, all, 6) && memcmp(pkt + 6, net_mac, 6)) continue;
		if (!sim) return take(from, buf, cap, pkt, (int)n);
		if (pkt[12] != 'B' && sim_loss > 0 && rand() % 100 < sim_loss) continue;
		if (dq_len == dq_cap) {
			int ncap = dq_cap ? dq_cap * 2 : 256;
			Delayed *nq = malloc(sizeof *nq * (size_t)ncap);
			for (int i = 0; i < dq_len; i++) nq[i] = dq[(dq_first + i) % dq_cap];
			free(dq);
			dq = nq, dq_cap = ncap, dq_first = 0;
		}
		Delayed *d = &dq[(dq_first + dq_len) % dq_cap];
		uint64_t at = rt_now_us() + (uint64_t)sim_delay * 1000 + (sim_jitter > 0 ? (uint64_t)(rand() % (sim_jitter + 1)) * 1000 : 0);
		uint64_t last = dq_len ? dq[(dq_first + dq_len - 1) % dq_cap].at : 0;
		d->at = at > last ? at : last;  // in order
		d->n = (int)n;
		memcpy(d->pkt, pkt, (size_t)n);
		dq_len++;
	}
	if (dq_len && dq[dq_first].at <= rt_now_us()) {
		Delayed *d = &dq[dq_first];
		dq_first = (dq_first + 1) % dq_cap;
		dq_len--;
		return take(from, buf, cap, d->pkt, d->n);
	}
	return 0;
}
#endif
