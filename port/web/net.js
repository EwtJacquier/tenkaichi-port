// Ad hoc transport for runtime/net.c (bundled with --pre-js): a LiveKit room carries the game's
// messages as LiveKit data messages, all of them reliable (in order, sent again when lost). The
// game plays its battles in lockstep: every frame each copy needs every player's record of that
// frame, and a packet repeats only the 2 frames before it, so 3 packets lost in a row stall it
// for good ("Connection was lost": 5% of the packets lost natively already did it at the battle's
// start). Late is better than lost here; the input delay (mods/netdelay.c) absorbs the lateness.
// Each copy of the game joins as the
// participant named by its virtual MAC (12 hex digits). The page asks its server for an access
// token (GET livekit-token?room=&identity=, see tools/serve_web.py); the LiveKit client library
// (web/livekit-client.umd.js, a pinned copy of the livekit-client npm package) is loaded the
// first time the game turns ad hoc on.
//
// state: 0 off, 1 connecting, 2 up, -1 failed. inbox: {from, data} in arrival order from
// inbox[head] on (N.take); data null when that participant left.
//
// A window held back (hidden, minimized, in a background tab) stops its game but not the
// messages: the others' state messages come every frame. Once the inbox is full the oldest of the
// game's own messages ('D': the newer ones say the same, more recently) make room; the mods'
// ('X': a shot, where it rested, who plays next) and the comings and goings never do. With 2048
// and the oldest of any kind dropped, a window held back half a minute lost the other player's
// shot: its character stood, and then only the shot's result showed.
Module.dbzNet = (() => {
  const N = {state: 0, inbox: [], head: 0, live: 0, room: null, gen: 0};
  const INBOX_MAX = 16384;
  const DATA = 0x44;  // 'D', runtime/net.c MSG_DATA
  let lib = null, dropAt = 0;

  const loadLib = () => lib || (lib = new Promise((resolve, reject) => {
    if (globalThis.LivekitClient) return resolve(globalThis.LivekitClient);
    const s = document.createElement('script');
    s.src = 'livekit-client.umd.js';
    s.onload = () => globalThis.LivekitClient ? resolve(globalThis.LivekitClient) : reject(new Error('LivekitClient missing'));
    s.onerror = () => { lib = null; reject(new Error('cannot load livekit-client.umd.js')); };
    document.head.appendChild(s);
  }));

  const push = (from, data) => {
    if (!/^[0-9a-f]{12}$/.test(from)) return;  // not a copy of the game
    if (N.live >= INBOX_MAX) {  // full: the oldest game message goes (left in place as null)
      dropAt = Math.max(dropAt, N.head);
      while (dropAt < N.inbox.length && !(N.inbox[dropAt] && N.inbox[dropAt].data && N.inbox[dropAt].data[0] === DATA)) dropAt++;
      if (dropAt < N.inbox.length) N.inbox[dropAt++] = null, N.live--;
    }
    N.inbox.push({from, data});
    N.live++;
  };

  // The next message in arrival order (null when there is none), for runtime/platform_web.c.
  N.take = () => {
    while (N.head < N.inbox.length) {
      const m = N.inbox[N.head++];
      if (N.head >= 4096 && N.head * 2 >= N.inbox.length) {  // the part taken goes
        N.inbox = N.inbox.slice(N.head);
        dropAt = Math.max(0, dropAt - N.head);
        N.head = 0;
      }
      if (m) {
        N.live--;
        return m;
      }
    }
    return null;
  };

  N.start = (mac, roomName) => {
    if (N.state === 1 || N.state === 2) return;
    const gen = ++N.gen;
    N.state = 1;
    console.log('ad hoc: joining LiveKit room "' + roomName + '" as ' + mac);
    Promise.all([loadLib(), fetch('livekit-token?room=' + encodeURIComponent(roomName) + '&identity=' + mac)
      .then(r => { if (!r.ok) throw new Error('token: HTTP ' + r.status); return r.json(); })])
      .then(async ([LK, {url, token}]) => {
        const room = new LK.Room({adaptiveStream: false, dynacast: false});
        room.on(LK.RoomEvent.DataReceived, (payload, participant) => {
          if (gen === N.gen && participant) push(participant.identity, payload);
        });
        room.on(LK.RoomEvent.ParticipantDisconnected, p => { if (gen === N.gen) push(p.identity, null); });
        room.on(LK.RoomEvent.Disconnected, reason => {
          if (gen !== N.gen) return;
          console.warn('ad hoc: LiveKit disconnected', reason);
          N.room = null;
          N.state = -1;
        });
        await room.connect(url, token, {autoSubscribe: false});
        if (gen !== N.gen) return room.disconnect();
        N.room = room;
        N.state = 2;
        console.log('ad hoc: on the air (' + room.remoteParticipants.size + ' other(s) in the room)');
      })
      .catch(e => {
        if (gen !== N.gen) return;
        console.error('ad hoc: cannot join the LiveKit room:', e);
        N.state = -1;
      });
  };

  N.stop = () => {
    N.gen++;
    if (N.room) N.room.disconnect();
    N.room = null;
    N.state = 0;
    N.inbox = [];
    N.head = N.live = dropAt = 0;
  };

  N.send = (to, bytes) => {
    if (N.state !== 2 || !N.room) return;
    const opts = {reliable: true};
    if (to) opts.destinationIdentities = [to];
    N.room.localParticipant.publishData(bytes, opts).catch(e => console.warn('ad hoc: send failed', e));
  };

  return N;
})();
