// Nebula x64 — host sound output.  The emulator mixes every guest stream (waveOut, DirectSound, WASAPI, XAudio2 → WASAPI) into
// 10 ms blocks of interleaved stereo float32 and hands them here; they are scheduled sample-accurately on an AudioContext, i.e.
// they come out of whatever the browser/Android routes audio to (speaker, Bluetooth, USB DAC).  No AudioWorklet file is needed,
// so this also works inside sandboxed/offline previews.  While the context is suspended (autoplay policy: no user gesture yet)
// a wall-clock stand-in keeps the guest's timing honest and the sound is dropped; it is picked up at the next gesture.
export class HostAudio {
  log: ((t: string) => void) | null = null;
  private ctx: AudioContext | null = null;
  private rate = 0;
  private nextTime = 0;            // AudioContext time at which the next block starts
  private written = 0;             // frames handed to us since open()
  private wallT0 = 0; private wallFrames = 0;   // stand-in clock while the context is not running
  private gestureHooked = false;
  private lastWrite = 0;

  state(): string { return !this.ctx ? 'none' : this.ctx.state === 'running' ? 'running' : 'suspended'; }
  /** true while the guest is actively feeding sound (used to keep the scheduler ticking in background tabs) */
  active(): boolean { return !!this.ctx && performance.now() - this.lastWrite < 500; }

  open(rate: number): number {
    if (this.ctx) return this.rate;
    const AC: typeof AudioContext | undefined = (window as unknown as { AudioContext?: typeof AudioContext; webkitAudioContext?: typeof AudioContext }).AudioContext
      ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
    if (!AC) { this.log?.('[audio] this browser has no Web Audio — sound is discarded (timing is still real)\n'); this.rate = rate || 48000; this.wallT0 = performance.now(); this.wallFrames = 0; return this.rate; }
    try { this.ctx = new AC({ sampleRate: rate || 48000, latencyHint: 'interactive' }); } catch { try { this.ctx = new AC(); } catch { this.ctx = null; } }
    if (!this.ctx) { this.log?.('[audio] AudioContext could not be created — sound is discarded\n'); this.rate = rate || 48000; this.wallT0 = performance.now(); this.wallFrames = 0; return this.rate; }
    this.rate = this.ctx.sampleRate; this.written = 0; this.wallFrames = 0; this.wallT0 = performance.now(); this.nextTime = 0;
    this.log?.(`[audio] device output: ${this.rate} Hz, ${this.ctx.state === 'running' ? 'running' : 'suspended until the first tap/click (autoplay policy)'}\n`);
    if (this.ctx.state !== 'running') { void this.ctx.resume().catch(() => undefined); this.hookGesture(); }
    this.ctx.onstatechange = () => { if (this.ctx?.state === 'running') { this.nextTime = 0; this.log?.('[audio] output running\n'); } };
    return this.rate;
  }

  private hookGesture() {
    if (this.gestureHooked) return; this.gestureHooked = true;
    const kick = () => { if (this.ctx && this.ctx.state !== 'running') void this.ctx.resume().catch(() => undefined); };
    for (const ev of ['pointerdown', 'keydown', 'touchend']) document.addEventListener(ev, kick, { capture: true, passive: true });
  }

  /** one block of interleaved L/R float32 */
  write(lr: Float32Array) {
    const frames = lr.length >> 1; this.lastWrite = performance.now(); this.written += frames;
    const ctx = this.ctx;
    if (!ctx || ctx.state !== 'running') { this.wallFrames += frames; return; }
    const buf = ctx.createBuffer(2, frames, this.rate);
    const l = buf.getChannelData(0), r = buf.getChannelData(1);
    for (let i = 0; i < frames; i++) { l[i] = lr[2 * i]; r[i] = lr[2 * i + 1]; }
    const src = ctx.createBufferSource(); src.buffer = buf; src.connect(ctx.destination);
    const now = ctx.currentTime;
    if (this.nextTime < now + 0.005) this.nextTime = now + 0.02;   // (re)start with 20 ms of safety margin after an underrun / resume
    src.start(this.nextTime); this.nextTime += frames / this.rate;
  }

  /** frames written but not yet played — the mixer keeps ~100 ms queued */
  queued(): number {
    const ctx = this.ctx;
    if (ctx && ctx.state === 'running') { const q = Math.round((this.nextTime - ctx.currentTime) * this.rate); return q > 0 ? q : 0; }
    // stand-in clock: play `wallFrames` at the nominal rate
    const played = (performance.now() - this.wallT0) / 1000 * this.rate; let q = this.wallFrames - played;
    if (q < 0) { this.wallT0 = performance.now() - this.wallFrames / this.rate * 1000; q = 0; }
    return Math.round(q);
  }

  /** drop what is still scheduled (program stopped) */
  flush() { this.nextTime = 0; this.wallFrames = 0; this.wallT0 = performance.now(); }
  framesWritten(): number { return this.written; }
}
