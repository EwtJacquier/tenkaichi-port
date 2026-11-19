// sceNetAdhocMatching: the PSP's library for finding and joining other players over ad hoc, on top
// of the same air as sceNetAdhoc (net.c: LiveKit in the browser, UDP on 127.0.0.1 natively),
// inside the ad hoc group (sceNetAdhocctl) the copies are in.
//
// A context is a parent (it makes a room: says HELLO every hello interval with its "hello" data,
// takes join requests and accepts or refuses them), a child (hears the parents' HELLOs, asks one to
// join, and once accepted knows the parent and its other children) or a peer to peer one. What
// happens arrives at the game as events on its callback (id, event, mac, length, data), called from
// the library's own thread, as on the PSP:
//   1 HELLO (a parent around, its hello data)    2 REQUEST (someone asks to join, its data)
//   3 LEAVE (a member left)    4 DENY (our request refused)    5 CANCEL (a request withdrawn)
//   6 ACCEPT (our request accepted, the parent's data)    7 ESTABLISHED (a member, both sides)
//   8 TIMEOUT (a member silent too long)    10 BYE (a member stopped)    11 DATA (sceNetAdhocMatchingSendData)
//   12 DATA_ACK (ours went)
// Packets: kind, 0, port (u16), then the kind's data. Members are kept alive by PINGs; one silent
// for MEMBER_TIMEOUT_US (generous: the internet, not a room) is a TIMEOUT.
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

const uint8_t *net_my_mac(void);
void net_poll(void);
bool net_peer_in_group(const uint8_t *mac);
void net_matching_send(const uint8_t *to, const void *data, int len);

#define DONE_(v) \
	do { \
		RET(v); \
		return; \
	} while (0)

#define MAX_CTX 4
#define MAX_PEERS 16
#define OPT_MAX 2048
#define QUEUE_MAX 64
#define MEMBER_TIMEOUT_US 15000000ull

#define ERR_NOT_INITIALIZED 0x80410812u
#define ERR_INVALID_ID 0x80410807u
#define ERR_INVALID_ARG 0x80410801u
#define ERR_NOT_RUNNING 0x8041080Cu
#define ERR_UNKNOWN_TARGET 0x80410809u
#define ERR_ALREADY_ESTABLISHED 0x8041080Eu
#define ERR_REQUEST_IN_PROGRESS 0x80410813u
#define ERR_TARGET_NOT_READY 0x80410815u
#define ERR_NOT_ESTABLISHED 0x8041080Fu
#define ERR_EXCEED_MAXNUM 0x80410810u
#define ERR_INVALID_DATALEN 0x80410814u
#define ERR_IS_RUNNING 0x8041080Bu

enum { EV_HELLO = 1, EV_REQUEST, EV_LEAVE, EV_DENY, EV_CANCEL, EV_ACCEPT, EV_ESTABLISHED, EV_TIMEOUT, EV_ERROR, EV_BYE,
       EV_DATA, EV_DATA_ACK, EV_DATA_TIMEOUT };
enum { PS_NONE, PS_OFFER, PS_PARENT, PS_CHILD, PS_P2P, PS_IN_REQ, PS_OUT_REQ };
enum { MODE_PARENT = 1, MODE_CHILD = 2, MODE_P2P = 3 };
enum { PK_HELLO = 1, PK_JOIN, PK_ACCEPT, PK_CANCEL, PK_DATA, PK_BIRTH, PK_DEATH, PK_BYE, PK_PING };
#define PK_HEAD 4

typedef struct {
	bool used;
	uint8_t mac[6];
	int state;
	uint64_t heard;
} MPeer;

typedef struct {
	int ev;
	uint8_t mac[6];
	int len;
	uint8_t *data;
} Event;

typedef struct {
	bool used, running;
	int id, mode, maxnum;
	uint16_t port;
	uint32_t hello_us, keep_us, callback, rxbuflen;
	uint8_t hello[OPT_MAX];
	int hello_len;
	MPeer peers[MAX_PEERS];
	Event q[QUEUE_MAX];
	int qn;
	uint64_t next_hello, next_ping;
	uint32_t buf;  // the game's memory the callback gets the mac (+0) and the data (+8) in
	uint32_t prio;
} Ctx;

static bool inited;
static Ctx ctxs[MAX_CTX];

static void log_ev(Ctx *x, const char *what, const uint8_t *mac) {
	rt_log("matching %d (%s): %s %02x:%02x:%02x:%02x:%02x:%02x", x->id,
	       x->mode == MODE_PARENT ? "parent" : x->mode == MODE_CHILD ? "child" : "p2p", what, mac[0], mac[1], mac[2], mac[3],
	       mac[4], mac[5]);
}

static Ctx *ctx_get(uint32_t id) {
	for (int i = 0; i < MAX_CTX; i++)
		if (ctxs[i].used && ctxs[i].id == (int)id) return &ctxs[i];
	return NULL;
}

static MPeer *peer_find(Ctx *x, const uint8_t *mac) {
	for (int i = 0; i < MAX_PEERS; i++)
		if (x->peers[i].used && !memcmp(x->peers[i].mac, mac, 6)) return &x->peers[i];
	return NULL;
}
static MPeer *peer_add(Ctx *x, const uint8_t *mac) {
	MPeer *p = peer_find(x, mac);
	if (p) return p;
	for (int i = 0; i < MAX_PEERS; i++)
		if (!x->peers[i].used) {
			p = &x->peers[i];
			memset(p, 0, sizeof *p);
			p->used = true;
			memcpy(p->mac, mac, 6);
			p->heard = rt_now_us();
			return p;
		}
	return NULL;
}
static MPeer *find_state(Ctx *x, int state) {
	for (int i = 0; i < MAX_PEERS; i++)
		if (x->peers[i].used && x->peers[i].state == state) return &x->peers[i];
	return NULL;
}
static int count_state(Ctx *x, int state) {
	int n = 0;
	for (int i = 0; i < MAX_PEERS; i++) n += x->peers[i].used && x->peers[i].state == state;
	return n;
}
static bool established(int s) { return s == PS_PARENT || s == PS_CHILD || s == PS_P2P; }

static void event(Ctx *x, int ev, const uint8_t *mac, const void *data, int len) {
	if (x->qn == QUEUE_MAX) return;
	if (len < 0 || len > OPT_MAX) len = 0;
	Event *e = &x->q[x->qn++];
	e->ev = ev;
	memcpy(e->mac, mac, 6);
	e->len = len;
	e->data = len ? malloc((size_t)len) : NULL;
	if (len) memcpy(e->data, data, (size_t)len);
}

static void send_pk(Ctx *x, const uint8_t *to, int kind, const void *data, int len) {
	static uint8_t m[PK_HEAD + OPT_MAX + 8 * MAX_PEERS];
	if (len < 0 || len > (int)sizeof m - PK_HEAD) return;
	m[0] = (uint8_t)kind;
	m[1] = 0;
	m[2] = (uint8_t)x->port;
	m[3] = (uint8_t)(x->port >> 8);
	if (len) memcpy(m + PK_HEAD, data, (size_t)len);
	net_matching_send(to, m, PK_HEAD + len);
}

// ---- packets -----------------------------------------------------------------------------------
static void on_hello(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	if ((x->mode == MODE_CHILD && find_state(x, PS_PARENT)) || (x->mode == MODE_P2P && find_state(x, PS_P2P))) return;
	if (x->mode == MODE_PARENT) return;
	MPeer *p = peer_add(x, from);
	if (!p) return;
	if (p->state == PS_NONE) p->state = PS_OFFER;
	if (p->state == PS_OUT_REQ || p->state == PS_IN_REQ) return;
	event(x, EV_HELLO, from, d, n);
}

static void on_join(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	if (x->mode == MODE_CHILD) return;
	bool room = x->mode == MODE_PARENT ? count_state(x, PS_CHILD) < x->maxnum - 1 : !find_state(x, PS_P2P);
	MPeer *p = room ? peer_add(x, from) : NULL;
	if (!p || established(p->state)) {
		if (!p) send_pk(x, from, PK_CANCEL, NULL, 0);
		return;
	}
	p->state = PS_IN_REQ;
	log_ev(x, "join request from", from);
	event(x, EV_REQUEST, from, d, n);
}

static void on_accept(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	if (x->mode == MODE_PARENT || n < 3) return;
	MPeer *p = peer_find(x, from);
	if (!p || p->state != PS_OUT_REQ) return;
	int optlen = d[0] | d[1] << 8;
	if (optlen < 0 || 2 + optlen + 1 > n) return;
	int nsib = d[2 + optlen];
	if (2 + optlen + 1 + 6 * nsib > n) return;
	p->state = x->mode == MODE_CHILD ? PS_PARENT : PS_P2P;
	for (int i = 0; i < MAX_PEERS; i++)  // the other rooms are of no interest any more
		if (x->peers[i].used && x->peers[i].state == PS_OFFER) x->peers[i].used = false;
	for (int k = 0; x->mode == MODE_CHILD && k < nsib; k++) {
		const uint8_t *mac = d + 3 + optlen + 6 * k;
		if (!memcmp(mac, net_my_mac(), 6)) continue;
		MPeer *s = peer_add(x, mac);
		if (s) s->state = PS_CHILD;
	}
	log_ev(x, "accepted by", from);
	event(x, EV_ACCEPT, from, d + 2, optlen);
	event(x, EV_ESTABLISHED, from, NULL, 0);
}

static void drop_all(Ctx *x, int ev, const void *d, int n) {  // a child's parent is gone: the whole room
	for (int i = 0; i < MAX_PEERS; i++) {
		MPeer *q = &x->peers[i];
		if (q->used && (q->state == PS_CHILD || q->state == PS_PARENT)) event(x, ev, q->mac, d, n);
		q->used = false;
	}
}

static void on_cancel(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	MPeer *p = peer_find(x, from);
	if (!p) return;
	log_ev(x, "cancel from", from);
	if (x->mode == MODE_CHILD) {
		if (p->state == PS_OUT_REQ) event(x, EV_DENY, from, d, n), p->state = PS_OFFER;
		else if (p->state == PS_PARENT) drop_all(x, EV_LEAVE, d, n);
	} else if (x->mode == MODE_PARENT) {
		if (p->state == PS_IN_REQ) event(x, EV_CANCEL, from, d, n), p->used = false;
		else if (p->state == PS_CHILD) {
			event(x, EV_LEAVE, from, d, n);
			p->used = false;
			send_pk(x, NULL, PK_DEATH, from, 6);
		}
	} else {
		if (p->state == PS_OUT_REQ) event(x, EV_DENY, from, d, n), p->state = PS_OFFER;
		else if (p->state == PS_P2P) event(x, EV_LEAVE, from, d, n), p->used = false;
		else if (p->state == PS_IN_REQ) event(x, EV_CANCEL, from, d, n), p->used = false;
	}
}

static void on_data(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	MPeer *p = peer_find(x, from);
	if (!p || !established(p->state) || n <= 0) return;
	event(x, EV_DATA, from, d, n);
	static int logged;
	if (getenv("DBZ_MATCHING_LOG") && logged++ < 200) {
		char hex[64] = {0};
		for (int i = 0; i < n && i < 16; i++) snprintf(hex + 3 * i, 4, "%02x ", d[i]);
		rt_log("matching %d: data in (%d bytes: %s)", x->id, n, hex);
	}
}

static void on_birth(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	MPeer *p = peer_find(x, from);
	if (x->mode != MODE_CHILD || !p || p->state != PS_PARENT || n < 6 || !memcmp(d, net_my_mac(), 6)) return;
	MPeer *s = peer_add(x, d);
	if (s) s->state = PS_CHILD;
}

static void on_death(Ctx *x, const uint8_t *from, const uint8_t *d, int n) {
	MPeer *p = peer_find(x, from);
	if (x->mode != MODE_CHILD || !p || p->state != PS_PARENT || n < 6) return;
	MPeer *s = peer_find(x, d);
	if (!s || s->state != PS_CHILD) return;
	event(x, EV_LEAVE, d, NULL, 0);
	s->used = false;
}

static void on_bye(Ctx *x, const uint8_t *from) {
	MPeer *p = peer_find(x, from);
	if (!p) return;
	log_ev(x, "bye from", from);
	if (x->mode == MODE_CHILD && p->state == PS_PARENT) {
		drop_all(x, EV_BYE, NULL, 0);
		return;
	}
	if (established(p->state) || (x->mode == MODE_P2P && p->state != PS_NONE)) event(x, EV_BYE, from, NULL, 0);
	if (x->mode == MODE_PARENT && p->state == PS_CHILD) send_pk(x, NULL, PK_DEATH, from, 6);
	p->used = false;
}

void matching_on_msg(const uint8_t *from, const uint8_t *m, int n) {
	if (n < PK_HEAD) return;
	uint16_t port = (uint16_t)(m[2] | m[3] << 8);
	for (int i = 0; i < MAX_CTX; i++) {
		Ctx *x = &ctxs[i];
		if (!x->used || !x->running || x->port != port) continue;
		MPeer *p = peer_find(x, from);
		if (p) p->heard = rt_now_us();
		const uint8_t *d = m + PK_HEAD;
		int dn = n - PK_HEAD;
		switch (m[0]) {
		case PK_HELLO: on_hello(x, from, d, dn); break;
		case PK_JOIN: on_join(x, from, d, dn); break;
		case PK_ACCEPT: on_accept(x, from, d, dn); break;
		case PK_CANCEL: on_cancel(x, from, d, dn); break;
		case PK_DATA: on_data(x, from, d, dn); break;
		case PK_BIRTH: on_birth(x, from, d, dn); break;
		case PK_DEATH: on_death(x, from, d, dn); break;
		case PK_BYE: on_bye(x, from); break;
		default: break;  // PK_PING: heard
		}
	}
}

void matching_peer_gone(const uint8_t *mac) {
	for (int i = 0; i < MAX_CTX; i++)
		if (ctxs[i].used && ctxs[i].running && peer_find(&ctxs[i], mac)) on_bye(&ctxs[i], mac);
}

// ---- the library's thread: HELLOs, PINGs, timeouts, and the events to the game -----------------
static void deliver(Ctx *x) {
	while (x->qn) {
		Event e = x->q[0];
		memmove(&x->q[0], &x->q[1], sizeof x->q[0] * (size_t)(--x->qn));
		if (x->callback && x->buf) {
			int len = e.len < (int)x->rxbuflen ? e.len : (int)x->rxbuflen;
			memcpy(MP(x->buf), e.mac, 6);
			if (len) memcpy(MP(x->buf + 8), e.data, (size_t)len);
			uint32_t args[5] = {(uint32_t)x->id, (uint32_t)e.ev, x->buf, (uint32_t)len, len ? x->buf + 8 : 0};
			rt_call_guest_args(x->callback, args, 5);
		}
		free(e.data);
	}
}

static void evth(void *arg) {
	Ctx *x = arg;
	int id = x->id;
	while (x->used && x->id == id && x->running) {
		net_poll();
		uint64_t now = rt_now_us();
		if ((x->mode == MODE_PARENT || (x->mode == MODE_P2P && !find_state(x, PS_P2P))) && now >= x->next_hello) {
			if (x->mode != MODE_PARENT || count_state(x, PS_CHILD) < x->maxnum - 1) send_pk(x, NULL, PK_HELLO, x->hello, x->hello_len);
			x->next_hello = now + x->hello_us;
		}
		if (now >= x->next_ping) {
			for (int i = 0; i < MAX_PEERS; i++)
				if (x->peers[i].used && established(x->peers[i].state)) send_pk(x, x->peers[i].mac, PK_PING, NULL, 0);
			x->next_ping = now + x->keep_us;
		}
		for (int i = 0; i < MAX_PEERS; i++) {
			MPeer *p = &x->peers[i];
			if (!p->used || now - p->heard < MEMBER_TIMEOUT_US) continue;
			if (!net_peer_in_group(p->mac) || established(p->state) || p->state == PS_IN_REQ || p->state == PS_OUT_REQ) {
				log_ev(x, "timeout of", p->mac);
				if (x->mode == MODE_CHILD && p->state == PS_PARENT) drop_all(x, EV_TIMEOUT, NULL, 0);
				else {
					if (established(p->state)) event(x, EV_TIMEOUT, p->mac, NULL, 0);
					if (x->mode == MODE_PARENT && p->state == PS_CHILD) send_pk(x, NULL, PK_DEATH, p->mac, 6);
					p->used = false;
				}
			} else if (p->state == PS_OFFER) {
				p->used = false;  // a room no longer heard from
			}
		}
		deliver(x);
		rt_wait_until(rt_now_us() + 4000);
	}
}

// ---- the API -----------------------------------------------------------------------------------
void hle_sceNetAdhocMatchingInit(CPU *c) {
	inited = true;
	RET(0);
}

static void ctx_stop(Ctx *x) {
	if (!x->running) return;
	send_pk(x, NULL, PK_BYE, NULL, 0);
	x->running = false;
	for (int i = 0; i < x->qn; i++) free(x->q[i].data);
	x->qn = 0;
	memset(x->peers, 0, sizeof x->peers);
}

void hle_sceNetAdhocMatchingTerm(CPU *c) {
	for (int i = 0; i < MAX_CTX; i++)
		if (ctxs[i].used) {
			ctx_stop(&ctxs[i]);
			if (ctxs[i].buf) rt_free_user(ctxs[i].buf);
			ctxs[i].used = false;
		}
	inited = false;
	RET(0);
}

// (mode, maxnum, port, rxbuflen, hello interval us, keepalive us, init count, retransmit us, callback)
void hle_sceNetAdhocMatchingCreate(CPU *c) {
	if (!inited) DONE_(ERR_NOT_INITIALIZED);
	int mode = (int)A0;
	if (mode < 1 || mode > 3) DONE_(ERR_INVALID_ARG);
	static int next_id = 1;
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i].used) continue;
		Ctx *x = &ctxs[i];
		memset(x, 0, sizeof *x);
		x->used = true;
		x->id = next_id++;
		x->mode = mode;
		x->maxnum = (int)A1 < 2 ? 2 : (int)A1;
		x->port = (uint16_t)A2;
		x->rxbuflen = A3 < 64 ? 64 : A3 > OPT_MAX ? OPT_MAX : A3;
		x->hello_us = A4 < 100000 ? 100000 : A4;
		x->keep_us = A5 < 500000 ? 500000 : A5;
		x->callback = M32(c->r[29]);  // the 9th argument: on the stack, after a0-a3 and t0-t3
		x->buf = rt_alloc_user(x->rxbuflen + 8, true, "matching events");
		rt_log("matching %d created: mode %d, max %d, port %u, hello %u us, keepalive %u us, callback %08x", x->id, mode,
		       x->maxnum, x->port, A4, A5, x->callback);
		RET(x->id);
		return;
	}
	RET(ERR_INVALID_ARG);
}

// (id, event thread priority, stack, input thread priority, stack, hello data length, hello data)
void hle_sceNetAdhocMatchingStart(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	if (x->running) DONE_(ERR_IS_RUNNING);
	int len = (int)A5;
	x->hello_len = len > 0 && A6 && len <= OPT_MAX ? len : 0;
	if (x->hello_len) memcpy(x->hello, MP(A6), (size_t)x->hello_len);
	x->running = true;
	x->next_hello = x->next_ping = 0;
	x->prio = A1 ? A1 : 0x30;
	if (!rt_native_thread("SceNetAdhocMatchingEvent", evth, x, x->prio, 0x2000)) {
		x->running = false;
		DONE_(ERR_INVALID_ARG);
	}
	rt_log("matching %d started (hello data %d bytes)", x->id, x->hello_len);
	RET(0);
}

void hle_sceNetAdhocMatchingStop(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	ctx_stop(x);
	RET(0);
}

void hle_sceNetAdhocMatchingDelete(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	ctx_stop(x);
	if (x->buf) rt_free_user(x->buf);
	x->used = false;
	RET(0);
}

// (id, mac, data length, data): a child asks a parent (or a peer another) to join; a parent accepts.
void hle_sceNetAdhocMatchingSelectTarget(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	if (!x->running) DONE_(ERR_NOT_RUNNING);
	if (!A1) DONE_(ERR_INVALID_ARG);
	const uint8_t *mac = MP(A1);
	MPeer *p = peer_find(x, mac);
	if (!p) DONE_(ERR_UNKNOWN_TARGET);
	int optlen = (int)A2 > 0 && A3 ? (int)A2 : 0;
	if (optlen > OPT_MAX) optlen = OPT_MAX;
	if (x->mode == MODE_PARENT) {
		if (p->state == PS_CHILD) DONE_(ERR_ALREADY_ESTABLISHED);
		if (count_state(x, PS_CHILD) >= x->maxnum - 1) DONE_(ERR_EXCEED_MAXNUM);
		if (p->state != PS_IN_REQ) DONE_(ERR_TARGET_NOT_READY);
		uint8_t m[2 + OPT_MAX + 1 + 6 * MAX_PEERS];
		m[0] = (uint8_t)optlen, m[1] = (uint8_t)(optlen >> 8);
		if (optlen) memcpy(m + 2, MP(A3), (size_t)optlen);
		int nsib = 0;
		for (int i = 0; i < MAX_PEERS; i++)
			if (x->peers[i].used && x->peers[i].state == PS_CHILD) memcpy(m + 3 + optlen + 6 * nsib++, x->peers[i].mac, 6);
		m[2 + optlen] = (uint8_t)nsib;
		p->state = PS_CHILD;
		send_pk(x, mac, PK_ACCEPT, m, 3 + optlen + 6 * nsib);
		for (int i = 0; i < MAX_PEERS; i++)  // the other children hear of it
			if (x->peers[i].used && x->peers[i].state == PS_CHILD && &x->peers[i] != p) send_pk(x, x->peers[i].mac, PK_BIRTH, mac, 6);
		log_ev(x, "accepts", mac);
		event(x, EV_ESTABLISHED, mac, NULL, 0);
		RET(0);
		return;
	}
	if (x->mode == MODE_CHILD && find_state(x, PS_PARENT)) DONE_(ERR_ALREADY_ESTABLISHED);
	if (x->mode == MODE_P2P && find_state(x, PS_P2P)) DONE_(ERR_ALREADY_ESTABLISHED);
	if (find_state(x, PS_OUT_REQ)) DONE_(ERR_REQUEST_IN_PROGRESS);
	if (p->state == PS_OFFER) {
		p->state = PS_OUT_REQ;
		p->heard = rt_now_us();
		send_pk(x, mac, PK_JOIN, optlen ? MP(A3) : NULL, optlen);
		log_ev(x, "asks to join", mac);
		RET(0);
		return;
	}
	if (x->mode == MODE_P2P && p->state == PS_IN_REQ) {
		uint8_t m[3 + OPT_MAX];
		m[0] = (uint8_t)optlen, m[1] = (uint8_t)(optlen >> 8);
		if (optlen) memcpy(m + 2, MP(A3), (size_t)optlen);
		m[2 + optlen] = 0;
		p->state = PS_P2P;
		send_pk(x, mac, PK_ACCEPT, m, 3 + optlen);
		event(x, EV_ESTABLISHED, mac, NULL, 0);
		RET(0);
		return;
	}
	RET(ERR_TARGET_NOT_READY);
}

// (id, mac, data length, data): refuses a request, withdraws ours, leaves the room or sends a child away.
void hle_sceNetAdhocMatchingCancelTargetWithOpt(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	if (!x->running) DONE_(ERR_NOT_RUNNING);
	if (!A1) DONE_(ERR_INVALID_ARG);
	const uint8_t *mac = MP(A1);
	MPeer *p = peer_find(x, mac);
	if (!p) DONE_(0);
	int optlen = (int)A2 > 0 && A3 ? (int)A2 : 0;
	if (optlen > OPT_MAX) optlen = OPT_MAX;
	log_ev(x, "cancels", mac);
	send_pk(x, mac, PK_CANCEL, optlen ? MP(A3) : NULL, optlen);
	if (x->mode == MODE_PARENT && p->state == PS_CHILD) send_pk(x, NULL, PK_DEATH, mac, 6);
	if (x->mode == MODE_CHILD && p->state == PS_PARENT) {
		for (int i = 0; i < MAX_PEERS; i++) x->peers[i].used = false;  // the whole room goes with the parent
	} else {
		p->used = false;
	}
	RET(0);
}
void hle_sceNetAdhocMatchingCancelTarget(CPU *c) {
	CPU call = *c;
	call.r[6] = 0, call.r[7] = 0;
	hle_sceNetAdhocMatchingCancelTargetWithOpt(&call);
	RET(call.r[2]);
}

void hle_sceNetAdhocMatchingSetHelloOpt(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	int len = (int)A1;
	x->hello_len = len > 0 && A2 && len <= OPT_MAX ? len : 0;
	if (x->hello_len) memcpy(x->hello, MP(A2), (size_t)x->hello_len);
	RET(0);
}

// (id, mac, length, data): to a member; DATA there, DATA_ACK here.
void hle_sceNetAdhocMatchingSendData(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	if (!x->running) DONE_(ERR_NOT_RUNNING);
	int len = (int)A2;
	if (!A1 || !A3) DONE_(ERR_INVALID_ARG);
	if (len <= 0 || len > OPT_MAX) DONE_(ERR_INVALID_DATALEN);
	const uint8_t *mac = MP(A1);
	MPeer *p = peer_find(x, mac);
	if (!p) DONE_(ERR_UNKNOWN_TARGET);
	if (!established(p->state)) DONE_(ERR_NOT_ESTABLISHED);
	send_pk(x, mac, PK_DATA, MP(A3), len);
	static int logged;
	if (getenv("DBZ_MATCHING_LOG") && logged++ < 200) log_ev(x, "data out to", mac);
	event(x, EV_DATA_ACK, mac, NULL, 0);
	RET(0);
}

void hle_sceNetAdhocMatchingAbortSendData(CPU *c) {
	if (!ctx_get(A0)) DONE_(ERR_INVALID_ID);
	RET(0);
}

// (id, size in/out, SceNetAdhocMatchingMember array: next, mac[6] + 2): this copy and its members.
void hle_sceNetAdhocMatchingGetMembers(CPU *c) {
	Ctx *x = ctx_get(A0);
	if (!x) DONE_(ERR_INVALID_ID);
	if (!x->running) DONE_(ERR_NOT_RUNNING);
	if (!A1) DONE_(ERR_INVALID_ARG);
	const uint8_t *list[MAX_PEERS + 1];
	int n = 0;
	MPeer *parent = find_state(x, PS_PARENT);
	if (x->mode == MODE_PARENT || parent || find_state(x, PS_P2P)) {
		if (parent) list[n++] = parent->mac;
		list[n++] = net_my_mac();
		for (int i = 0; i < MAX_PEERS; i++)
			if (x->peers[i].used && (x->peers[i].state == PS_CHILD || x->peers[i].state == PS_P2P)) list[n++] = x->peers[i].mac;
	}
	if (!A2) {
		M32(A1) = (uint32_t)(n * 12);
		RET(0);
		return;
	}
	int cap = (int)M32(A1) / 12;
	if (n > cap) n = cap;
	for (int i = 0; i < n; i++) {
		uint32_t e = A2 + (uint32_t)(i * 12);
		M32(e) = i + 1 < n ? e + 12 : 0;
		memcpy(MP(e + 4), list[i], 6);
		M16(e + 10) = 0;
	}
	M32(A1) = (uint32_t)(n * 12);
	RET(0);
}
