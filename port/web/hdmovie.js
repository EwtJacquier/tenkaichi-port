// HD movies in the browser (runtime/hdmovie.c; one file for every PSP port, with it): the browser's own
// H.264 decoder (WebCodecs' VideoDecoder, on the GPU where it can) decodes a movie's HD version a few
// frames ahead of the game, off the game's thread, and each frame goes from the decoder straight into
// the movie's texture (texImage2D of the VideoFrame). FFmpeg in WebAssembly took some 20 ms for each
// 1920x1088 frame on the game's thread, and another 10 to turn it into RGBA and send it.
//
// The stream (movies/<name>.h264) is H.264 Annex B, one picture a frame, no B-frames; hdmovie.c has
// found where each frame's access unit starts in it (au) and which ones are IDR pictures (idr), and
// keeps the file in its memory until close. The game asks for its frames in order, and over again
// from the first for a movie in a loop: the decoder goes on from the last frame to the first (an IDR
// picture), so frames are counted on and on (L; the frame is L % count).
//
// A decoder may hold decoded frames back, for B-frames to come in between: Chrome's hardware ones hold
// as many as the level allows (4 at 1920x1088, level 4.0) unless the stream says it never reorders,
// which GStreamer's vaapih264enc doesn't (x264 does), and the game got its frames late, every other
// one its own. So each sequence parameter set goes to the decoder saying so (spsNoReorder), and a
// decoder that still holds frames back gets more fed ahead (lag).
Module.hdMovie = (() => {
	const AHEAD = 4;  // frames decoded before the game asks for them
	const MAX_LAG = 16;  // frames a decoder may hold back (an H.264 picture buffer at most)
	const CATCH_UP = 30;  // a frame further on than this from the decoder's place: begun afresh from an IDR picture before it
	const movies = new Map();
	let nextId = 1;

	// 'avc1.PPCCLL' from the sequence parameter set before the first picture
	function codecOf(data, au) {
		const a = data + HEAPU32[au >> 2], b = data + HEAPU32[(au >> 2) + 1];
		for (let i = a; i + 6 < b; i++)
			if (HEAPU8[i] === 0 && HEAPU8[i + 1] === 0 && HEAPU8[i + 2] === 1 && (HEAPU8[i + 3] & 0x1F) === 7) {
				const hex = (v) => v.toString(16).padStart(2, '0');
				return 'avc1.' + hex(HEAPU8[i + 4]) + hex(HEAPU8[i + 5]) + hex(HEAPU8[i + 6]);
			}
		return null;
	}

	// The sequence parameter set (a NAL unit, header byte first, emulation prevention bytes in) with the
	// VUI's bitstream restriction saying no frame is held back for reordering (max_num_reorder_frames 0,
	// max_dec_frame_buffering the reference frames); null if it already says so or can't be read.
	function spsNoReorder(nal) {
		const rb = [];
		for (let i = 1, z = 0; i < nal.length; i++) {
			if (z >= 2 && nal[i] === 3) {
				z = 0;
				continue;
			}
			z = nal[i] === 0 ? z + 1 : 0;
			rb.push(nal[i]);
		}
		let p = 0;
		const bit = () => {
			if (p >= rb.length * 8) throw new Error('short');
			return (rb[p >> 3] >> (7 - (p++ & 7))) & 1;
		};
		const u = (n) => {
			let v = 0;
			for (let i = 0; i < n; i++) v = v * 2 + bit();
			return v;
		};
		const ue = () => {
			let z = 0;
			while (!bit()) z++;
			return 2 ** z - 1 + u(z);
		};
		const out = [];
		const put = (v, n) => {
			for (let i = n - 1; i >= 0; i--) out.push(Math.floor(v / 2 ** i) & 1);
		};
		const putUe = (v) => {
			const n = Math.floor(Math.log2(v + 1));
			put(0, n), put(v + 1, n + 1);
		};
		let refs, head;  // head: the bits kept, up to where the restriction goes
		try {
			const profile = u(8);
			u(16), ue();
			if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].includes(profile)) {
				if (ue() === 3) u(1);
				ue(), ue(), u(1);
				if (u(1)) return null;  // scaling matrices: not read here
			}
			ue();
			const poc = ue();
			if (poc === 0) ue();
			else if (poc === 1) {
				u(1), ue(), ue();
				for (let n = ue(); n > 0; n--) ue();
			}
			refs = ue();
			u(1), ue(), ue();
			if (!u(1)) u(1);
			u(1);
			if (u(1)) ue(), ue(), ue(), ue();
			const vuiAt = p;
			if (!u(1)) {  // no VUI: one with nothing in it but the restriction
				for (let i = 0; i < vuiAt; i++) out.push((rb[i >> 3] >> (7 - (i & 7))) & 1);
				put(1, 1), put(0, 8);
			} else {
				if (u(1) && u(8) === 255) u(32);
				if (u(1)) u(1);
				if (u(1)) {
					u(4);
					if (u(1)) u(24);
				}
				if (u(1)) ue(), ue();
				if (u(1)) u(32), u(32), u(1);
				const hrd = () => {
					const n = ue();
					u(8);
					for (let i = 0; i <= n; i++) ue(), ue(), u(1);
					u(20);
				};
				const nalHrd = u(1);
				if (nalHrd) hrd();
				const vclHrd = u(1);
				if (vclHrd) hrd();
				if (nalHrd || vclHrd) u(1);
				u(1);
				head = p;
				if (u(1)) {
					u(1), ue(), ue(), ue(), ue();
					if (ue() === 0) return null;  // it says so already
				}
				for (let i = 0; i < head; i++) out.push((rb[i >> 3] >> (7 - (i & 7))) & 1);
			}
		} catch (e) {
			return null;
		}
		// bitstream_restriction_flag, motion vectors over picture boundaries, the defaults of the bytes per
		// picture and bits per macroblock limits and of the motion vectors' lengths; then no reordering
		put(1, 1), put(1, 1), putUe(2), putUe(1), putUe(15), putUe(15), putUe(0), putUe(Math.max(refs, 1));
		put(1, 1);  // rbsp_stop_one_bit
		while (out.length % 8) out.push(0);
		const nalOut = [nal[0]];
		for (let i = 0, z = 0; i < out.length; i += 8) {
			let b = 0;
			for (let j = 0; j < 8; j++) b = b * 2 + out[i + j];
			if (z >= 2 && b <= 3) nalOut.push(3), z = 0;
			z = b === 0 ? z + 1 : 0;
			nalOut.push(b);
		}
		return new Uint8Array(nalOut);
	}

	// Where the sequence parameter set is in bytes d[a..b) (an access unit): [start, end) of its NAL unit.
	function findSps(d, a, b) {
		for (let i = a; i + 3 < b; i++)
			if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 1 && (d[i + 3] & 0x1F) === 7) {
				let e = i + 3;
				while (e + 2 < b && !(d[e] === 0 && d[e + 1] === 0 && d[e + 2] === 1)) e++;
				if (e + 2 >= b) e = b;
				else if (d[e - 1] === 0) e--;  // a 4-byte start code's leading zero
				return [i + 3, e];
			}
		return null;
	}

	function start(m) {
		m.dec = new VideoDecoder({output: (f) => output(m, f), error: (e) => broke(m, e)});
		m.dec.configure(m.config);
		m.pending.clear();
		m.feedL = -1;  // nothing fed: the next frame asked for is begun from an IDR picture
	}

	function broke(m, e) {
		if (m.closed) return;
		console.error('HD movie: the browser\'s decoder stopped: ' + (e && e.message || e));
		if (!m.soft) {  // once more in software (a GPU decoder that can't)
			m.soft = true;
			m.config.hardwareAcceleration = 'prefer-software';
			try {
				if (m.dec.state !== 'closed') m.dec.close();
				start(m);
				return;
			} catch (e2) {}
		}
		m.failed = true;
	}

	function chunk(m, L) {
		const k = L % m.count, key = HEAPU8[m.idr + k] !== 0;
		const a = m.data + HEAPU32[(m.au >> 2) + k], b = m.data + HEAPU32[(m.au >> 2) + k + 1];
		let data = HEAPU8.subarray(a, b);
		const s = key && m.sps ? findSps(HEAPU8, a, b) : null;
		if (s && s[1] - s[0] === m.sps.old.length && m.sps.old.every((v, i) => HEAPU8[s[0] + i] === v)) {
			data = new Uint8Array(b - a - m.sps.old.length + m.sps.new.length);
			data.set(HEAPU8.subarray(a, s[0]), 0);
			data.set(m.sps.new, s[0] - a);
			data.set(HEAPU8.subarray(s[1], b), s[0] - a + m.sps.new.length);
		}
		m.dec.decode(new EncodedVideoChunk({type: key ? 'key' : 'delta', timestamp: L * 1000, data}));
		m.pending.set(L, performance.now());  // when it went in
	}

	// the decoder kept fed up to AHEAD frames on from the one the game shows, and as many more as it holds back
	function feed(m) {
		try {
			while (!m.failed && m.feedL >= 0 && m.feedL <= m.targetL + AHEAD + m.lag && m.dec.decodeQueueSize < 16) chunk(m, m.feedL++);
		} catch (e) {
			broke(m, e);
		}
	}

	function output(m, f) {
		const L = Math.round(f.timestamp / 1000);
		m.pending.delete(L);
		if (m.closed || m.frames.has(L) || L < m.targetL - 1 || L > m.targetL + AHEAD + m.lag) f.close();
		else m.frames.set(L, f), m.w = f.displayWidth, m.h = f.displayHeight;
		feed(m);
	}

	return {
		// a movie mpeg.c has read (pointers into its memory): an id, 0 if this browser can't decode it
		open(data, au, idr, count) {
			if (typeof VideoDecoder !== 'function' || typeof EncodedVideoChunk !== 'function') return 0;
			const codec = codecOf(data, au);
			if (!codec) return 0;
			let sps = null;  // the stream's sequence parameter set (old) and what goes to the decoder instead (new)
			const at = findSps(HEAPU8, data + HEAPU32[au >> 2], data + HEAPU32[(au >> 2) + 1]);
			if (at) {
				const old = HEAPU8.slice(at[0], at[1]), nw = spsNoReorder(old);
				if (nw) sps = {old, new: nw};
			}
			const m = {
				data, au, idr, count, sps, frames: new Map(), pending: new Map(), feedL: -1, targetL: 0, w: 0, h: 0, lag: 0, asked: 0, hd: 0, fills: 0, fillMs: 0, fillMax: 0, hw: '?',
				// the colors as the game's sceMpegAvcCsc turns them (BT.601, limited range; the frames were
				// approved so), and nothing more done to them
				config: {codec, optimizeForLatency: true, colorSpace: {primaries: 'bt709', transfer: 'iec61966-2-1', matrix: 'smpte170m', fullRange: false}},
			};
			try {
				start(m);
				m.feedL = 0;  // its first frames at once
				feed(m);
			} catch (e) {
				console.error('HD movie: ' + codec + ': ' + (e && e.message || e));
				return 0;
			}
			if (VideoDecoder.isConfigSupported)
				VideoDecoder.isConfigSupported({...m.config, hardwareAcceleration: 'prefer-hardware'})
					.then((r) => { m.hw = r.supported ? 'hardware' : 'software'; }).catch(() => {});
			const id = nextId++;
			movies.set(id, m);
			return id;
		},

		// the game shows frame k: 1 its picture is here, 0 not yet (the game's own one meanwhile), -1 none
		// will come (the decoder failed)
		want(id, k) {
			const m = movies.get(id);
			if (!m || m.failed) return -1;
			// k as counted on: the nearest to the last one asked for (after the last frame, the first)
			let L = m.targetL - (m.targetL % m.count) + k;
			if (L - m.targetL > m.count / 2 && L >= m.count) L -= m.count;
			else if (m.targetL - L > m.count / 2) L += m.count;
			m.targetL = L;
			for (const [j, f] of m.frames)
				if (j < L - 1 || j > L + AHEAD + m.lag) f.close(), m.frames.delete(j);
			// it went in long since and the decoder, with nothing else to do, hasn't given it out: it holds frames back
			if (!m.frames.has(L) && m.pending.has(L) && m.dec.decodeQueueSize === 0 && m.lag < MAX_LAG &&
			    performance.now() - m.pending.get(L) > 40) {
				m.lag++;
				console.log('HD movie: the browser\'s decoder holds frames back: ' + (AHEAD + m.lag) + ' fed ahead');
			}
			if (!m.frames.has(L) && !m.pending.has(L) && !(m.feedL >= 0 && m.feedL <= L && L - m.feedL <= CATCH_UP)) {
				let j = L;  // begun afresh, from the IDR picture at or before it
				while (j % m.count && !HEAPU8[m.idr + (j % m.count)]) j--;
				try {
					if (m.feedL >= 0) m.dec.reset(), m.dec.configure(m.config), m.pending.clear();
					m.feedL = j;
				} catch (e) {
					broke(m, e);
				}
			}
			feed(m);
			m.asked++;
			if (!m.frames.has(L)) return 0;
			m.hd++;
			return 1;
		},

		// its pictures' size: width << 16 | height
		size(id) {
			const m = movies.get(id);
			return m ? m.w << 16 | m.h : 0;
		},

		// frame k's picture into the texture bound (ge.c, when the game draws it): 1 done
		fill(id, k, gl) {
			const m = movies.get(id);
			if (!m) return 0;
			for (const [L, f] of m.frames)
				if (L % m.count === k) {
					try {
						const t = performance.now();
						gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
						gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, f);
						gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.BROWSER_DEFAULT_WEBGL);
						const ms = performance.now() - t;
						m.fills++, m.fillMs += ms, m.fillMax = Math.max(m.fillMax, ms);
					} catch (e) {  // a browser that decodes but can't send a VideoFrame to WebGL
						console.error('HD movie: ' + (e && e.message || e));
						m.failed = true;
						return 0;
					}
					return 1;
				}
			return 0;
		},

		close(id) {
			const m = movies.get(id);
			if (!m) return;
			m.closed = true;
			movies.delete(id);
			if (m.asked)  // what it cost, for the console (F12)
				console.log(`HD movie: ${m.hd} of ${m.asked} frames in HD, ${m.w}x${m.h}, decoded by the browser (${m.hw}), ` +
				            `${AHEAD + m.lag} ahead; into the texture ${(m.fillMs / Math.max(m.fills, 1)).toFixed(2)} ms a frame (at most ${m.fillMax.toFixed(1)})`);
			for (const f of m.frames.values()) f.close();
			m.frames.clear();
			try {
				if (m.dec.state !== 'closed') m.dec.close();
			} catch (e) {}
		},
	};
})();
