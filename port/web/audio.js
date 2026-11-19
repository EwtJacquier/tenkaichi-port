// Browser audio output (bundled with --pre-js). The runtime pushes 44.1 kHz stereo s16 chunks as
// wall-clock time passes (plat_audio_push); an AudioWorklet plays them from its own FIFO on the
// audio thread, so a busy main thread doesn't cut the sound. The FIFO starts (and restarts after
// an underrun) once it holds `target` frames, skips ahead when far behind, and drops or repeats
// one frame now and then while its average level drifts from the target (clock drift). Where
// AudioWorklet is unavailable (pages served without a secure context) a ScriptProcessor plays the
// same FIFO from the main thread.

function dbzFifoClass() {
  return class T6Fifo {
    constructor(rate, targetMs) {
      this.cap = rate * 2;
      this.l = new Float32Array(this.cap);
      this.r = new Float32Array(this.cap);
      this.rd = 0;
      this.wr = 0;
      this.target = Math.round((rate * targetMs) / 1000);
      this.playing = false;
      this.avg = 0;
      this.sinceFix = 0;
      this.underruns = 0;
      this.minLevel = 1e9;
    }
    push(s16) {
      const n = s16.length >> 1;
      for (let i = 0; i < n; i++) {
        const k = this.wr % this.cap;
        this.l[k] = s16[2 * i] / 32768;
        this.r[k] = s16[2 * i + 1] / 32768;
        this.wr++;
      }
      if (this.wr - this.rd > this.cap) this.rd = this.wr - this.cap;
    }
    pull(L, R) {
      const n = L.length;
      let level = this.wr - this.rd;
      if (this.playing && level < this.minLevel) this.minLevel = level;
      if (!this.playing) {
        if (level < this.target) {
          L.fill(0);
          R.fill(0);
          return;
        }
        this.playing = true;
        this.avg = level;
      }
      if (level > this.target * 4) {
        this.rd = this.wr - this.target;
        level = this.target;
        this.avg = level;
      }
      this.avg += (level - this.avg) * 0.02;
      this.sinceFix += n;
      let skip = 0;
      let dupAt = -1;
      if (this.sinceFix >= 2048) {
        if (this.avg > this.target * 1.3) {
          skip = 1;
          this.sinceFix = 0;
        } else if (this.avg < this.target * 0.7) {
          dupAt = n >> 1;
          this.sinceFix = 0;
        }
      }
      for (let i = 0; i < n; i++) {
        if (this.wr === this.rd) {
          L.fill(0, i);
          R.fill(0, i);
          this.playing = false;
          this.underruns++;
          return;
        }
        const k = this.rd % this.cap;
        L[i] = this.l[k];
        R[i] = this.r[k];
        if (i !== dupAt) this.rd++;
      }
      if (skip && this.wr - this.rd > 1) this.rd++;
    }
  };
}

function dbzWorkletMain(Fifo) {
  class T6Out extends AudioWorkletProcessor {
    constructor(options) {
      super();
      this.fifo = new Fifo(sampleRate, options.processorOptions.targetMs);
      this.port.onmessage = (e) => this.fifo.push(e.data);
    }
    process(inputs, outputs) {
      const out = outputs[0];
      this.fifo.pull(out[0], out[1] || out[0]);
      this.frames = (this.frames || 0) + out[0].length;
      if (this.frames >= sampleRate) {  // once a second: statistics for the page (Module.dbzAudioStats)
        this.frames = 0;
        const f = this.fifo;
        this.port.postMessage({ underruns: f.underruns, level: f.wr - f.rd, avg: Math.round(f.avg), target: f.target, min: f.minLevel });
        f.minLevel = 1e9;
      }
      return true;
    }
  }
  registerProcessor('t6-out', T6Out);
}

Module.dbzAudioOpen = function () {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return 0;
  let ac;
  try {
    ac = new AC({ sampleRate: 44100, latencyHint: 'interactive' });
  } catch (e) {
    console.error('audio', e);
    return 0;
  }
  Module.dbzAudio = ac;
  // The page's volume (shell.html, its settings): a gain between the game's sound and the speakers.
  const gain = ac.createGain();
  gain.gain.value = Module.dbzVolume ?? 1;
  gain.connect(ac.destination);
  Module.dbzSetVolume = (v) => gain.gain.setTargetAtTime(v, ac.currentTime, 0.02);
  // Seconds from a pushed frame to the speaker: the FIFO's average level plus the browser's own.
  Module.dbzAudioLatency = () => {
    const st = Module.dbzAudioStats;
    const fifo = st ? st.avg : 0.05 * ac.sampleRate;
    return fifo / ac.sampleRate + (ac.outputLatency || 0) + (ac.baseLatency || 0);
  };
  // Until the output is ready, and while the context is suspended (no user gesture yet), chunks
  // are dropped: the game keeps its own pace and the FIFO re-buffers when sound starts.
  Module.dbzAudioPush = () => {};
  const scriptFallback = () => {
    const fifo = new (dbzFifoClass())(ac.sampleRate, 120);
    const node = ac.createScriptProcessor(2048, 0, 2);
    node.onaudioprocess = (ev) => {
      fifo.pull(ev.outputBuffer.getChannelData(0), ev.outputBuffer.getChannelData(1));
      Module.dbzAudioStats = { underruns: fifo.underruns, level: fifo.wr - fifo.rd, avg: Math.round(fifo.avg), target: fifo.target };
    };
    node.connect(gain);
    Module.dbzAudioNode = node;
    Module.dbzAudioPush = (chunk) => {
      if (ac.state === 'running') fifo.push(chunk);
    };
    console.log('audio: ScriptProcessor output (AudioWorklet unavailable)');
  };
  if (ac.audioWorklet && window.isSecureContext) {
    const src = `(${dbzWorkletMain})((${dbzFifoClass})());`;
    const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
    ac.audioWorklet
      .addModule(url)
      .then(() => {
        const node = new AudioWorkletNode(ac, 't6-out', {
          numberOfInputs: 0,
          numberOfOutputs: 1,
          outputChannelCount: [2],
          processorOptions: { targetMs: Number(new URLSearchParams(location.search).get('DBZ_AUDIO_MS')) || 30 },
        });
        node.connect(gain);
        node.port.onmessage = (e) => (Module.dbzAudioStats = e.data);
        Module.dbzAudioNode = node;
        Module.dbzAudioPush = (chunk) => {
          if (ac.state === 'running') node.port.postMessage(chunk, [chunk.buffer]);
        };
      })
      .catch((e) => {
        console.error('audio worklet', e);
        scriptFallback();
      });
  } else {
    scriptFallback();
  }
  return 1;
};
