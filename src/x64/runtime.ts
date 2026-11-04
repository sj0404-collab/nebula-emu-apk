import { WebGLGfx } from './gfx';
import { HostAudio } from './audio';
// Nebula x64 — JS host for the wasm64 x86-64 emulator (emu/x64).  Pointers/sizes cross the boundary as BigInt (memory64).
export type RunStatus = 'idle' | 'running' | 'waiting-stdin' | 'exited' | 'fault' | 'break' | 'stopped';
export interface VfsEntry { path: string; size: number; dir: boolean; written: boolean }

export interface PeInfo {
  ok: boolean; error?: string;
  arch?: string; machine?: number; pe32plus?: boolean; subsystem?: string; dll?: boolean; clr?: boolean;
  imageBase?: string; sizeOfImage?: number; entry?: string; sections?: number; relocs?: boolean; tls?: boolean; exceptions?: boolean; resources?: number;
  sectionNames?: string[]; unknownImports?: string; imports?: number; knownImports?: number; unknownImportsCount?: number; privateDllImports?: number; dlls?: string[];
  verdict?: 'likely' | 'partial' | 'unlikely' | 'unsupported'; why?: string;
  dotnet?: 'managed' | 'managed-dll' | 'apphost' | null;   // .NET: IL assembly (started through dotnet.exe exec), library, or apphost launcher
}

export interface RunStats {
  status: RunStatus; icount: number; mips: number; frames: number; elapsedMs: number; exitCode: number;
  error: string; errorRip: string; errorBytes: string; report: string; title: string; subsystem: number; hasWindow: boolean; memMB: number;
  /** live guest processes (root + children spawned through CreateProcess/_spawn) */
  procs: number;
  threads: number;
  /** stereo frames mixed for the host audio output (0 = the program has not opened a sound device) */
  audioFrames: number;
  /** 'running' | 'suspended' (waiting for a user gesture — the sound is dropped meanwhile, the program keeps running) | 'none' */
  audio: string;
}

type Exports = {
  memory: WebAssembly.Memory;
  x64_alloc(n: bigint): bigint; x64_inspect(p: bigint, n: bigint): bigint; x64_load(p: bigint, n: bigint, name: bigint, cmd: bigint): number;
  x64_reserve(n: bigint): number; x64_run(budget: bigint): number; x64_key(vk: number, down: number): void; x64_mouse(x: number, y: number, ev: number): void;
  x64_icount(): bigint; x64_exit_code(): number; x64_error(): bigint; x64_error_rip(): bigint; x64_rip(): bigint; x64_report(): bigint; x64_console(): bigint;
  x64_screen_w(): number; x64_screen_h(): number; x64_screen(): bigint; x64_title(): bigint; x64_subsystem(): number; x64_sleep_ms(): number; x64_mem_used(): bigint; x64_has_window(): number; x64_proc_count(): number; x64_thread_count(): number; x64_audio_frames(): bigint;
  x64_api_stats(): bigint; x64_error_bytes(): bigint;
  x64_vfs_add(path: bigint, data: bigint, n: bigint): number; x64_vfs_mkdir(path: bigint): number; x64_vfs_remove(path: bigint): number;
  x64_vfs_size(path: bigint): bigint; x64_vfs_data(path: bigint): bigint; x64_vfs_list(): bigint;
  x64_stdin_write(data: bigint, n: bigint): void; x64_stdin_eof(): void; x64_wants_stdin(): number;
};

export const RUN_OK = 0, RUN_EXIT = 1, RUN_FAULT = 2, RUN_YIELD = 3, RUN_BREAK = 4;

export function wasm64Supported(): boolean {
  try {
    // memory64 feature probe: (module (memory i64 1))
    return WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 5, 3, 1, 4, 1]));
  } catch { return false; }
}

const MATH = [Math.sin, Math.cos, Math.tan, Math.exp, Math.log, Math.pow, Math.atan2, Math.asin, Math.acos, Math.atan, Math.sinh, Math.cosh, Math.tanh, Math.log10, Math.log2, Math.cbrt];

export class X64Runtime {
  private ex: Exports | null = null;
  private dec = new TextDecoder(); private enc = new TextEncoder();
  private t0 = 0; private frames = 0; private raf = 0; private timer = 0; private running = false;
  /** the device's sound output (Web Audio); the guest's waveOut/DirectSound/WASAPI/XAudio2 streams end up here */
  readonly audio = new HostAudio();
  status: RunStatus = 'idle';
  moduleBytes = 0;
  onConsole: ((fd: number, text: string) => void) | null = null;
  onPresent: ((px: Uint8Array, w: number, h: number) => void) | null = null;
  onStop: ((stats: RunStats) => void) | null = null;
  onLog: ((text: string) => void) | null = null;
  onStdinWait: ((waiting: boolean) => void) | null = null;
  private waitingStdin = false;
  readonly gfx = new WebGLGfx();
  private lastFrame: { p: bigint; w: number; h: number } | null = null;
  /** the most recently presented window contents (still valid after the program exits) — lets a re-mounted canvas show the final frame */
  lastPresented(): { px: Uint8Array; w: number; h: number } | null { const f = this.lastFrame; return f && this.ex ? { px: this.bytesAt(f.p, f.w * f.h * 4), w: f.w, h: f.h } : null; }

  async init(url = '/x64/nebula_x64.wasm'): Promise<void> {
    if (this.ex) return;
    const res = await fetch(url);
    if (!res.ok) throw new Error(`cannot fetch ${url}: ${res.status}`);
    const bytes = await res.arrayBuffer();
    this.moduleBytes = bytes.byteLength;
    const imports = {
      env: {
        host_write: (fd: number, p: bigint, n: bigint) => {
          const text = this.dec.decode(this.bytesAt(p, Number(n)));
          if (fd === 3) this.onLog?.(text); else this.onConsole?.(fd, text);
        },
        host_now_ms: () => performance.now(),
        host_wall_ms: () => Date.now(),
        host_present: (p: bigint, w: number, h: number) => { this.frames++; this.lastFrame = { p, w, h }; this.onPresent?.(this.bytesAt(p, w * h * 4), w, h); },
        host_math: (op: number, a: number, b: number) => MATH[op] ? MATH[op](a, b) : 0,
        // Direct3D 11 layer → WebGL2 (src/lib/x64/gfx.ts)
        host_gfx_init: () => { this.gfx.log = (t) => this.onLog?.(t); return this.gfx.init(); },
        host_gfx_shader: (kind: number, glsl: bigint) => this.gfx.shader(kind, this.cstr(glsl)),
        host_gfx_buffer: (id: number, data: bigint, len: number, bind: number) => this.gfx.buffer(id, data && len ? this.bytesAt(data, len).slice() : null, bind),
        host_gfx_texture: (id: number, w: number, h: number, fmt: number, px: bigint, pitch: number) => this.gfx.texture(id, w, h, fmt, px ? this.bytesAt(px, (pitch || w * 16) * h) : null, pitch),
        host_gfx_clear: (id: number, rgba: bigint) => this.gfx.clear(id, new Float32Array(this.ex!.memory.buffer, Number(rgba), 4)),
        host_gfx_clear_depth: (id: number, depth: number) => this.gfx.clearDepth(id, depth),
        host_gfx_draw: (cmd: bigint) => this.gfx.draw(new Uint32Array(this.ex!.memory.buffer, Number(cmd), 160)),
        host_gfx_readback: (id: number, out: bigint) => { const t = this.gfx.size(id); if (t) this.gfx.readback(id, this.bytesAt(out, t.w * t.h * 4)); },
        host_gfx_free: (id: number) => this.gfx.free(id),
        // sound → the device's audio output (src/lib/x64/audio.ts)
        host_audio_open: (rate: number) => { this.audio.log = (t) => this.onLog?.(t); return this.audio.open(rate); },
        host_audio_write: (lr: bigint, frames: number) => this.audio.write(new Float32Array(this.ex!.memory.buffer, Number(lr), frames * 2)),
        host_audio_queued: () => this.audio.queued(),
      },
    };
    const { instance } = await WebAssembly.instantiate(bytes, imports);
    this.ex = instance.exports as unknown as Exports;
  }

  private bytesAt(p: bigint, n: number): Uint8Array { return new Uint8Array(this.ex!.memory.buffer, Number(p), n); }
  private cstr(p: bigint): string {
    if (!p) return '';
    const a = new Uint8Array(this.ex!.memory.buffer, Number(p)); let n = 0; while (a[n]) n++;
    return this.dec.decode(a.subarray(0, n));
  }
  private put(bytes: Uint8Array): bigint { const p = this.ex!.x64_alloc(BigInt(bytes.length)); if (!p && bytes.length) throw new Error(`wasm memory exhausted (could not allocate ${bytes.length} bytes)`); new Uint8Array(this.ex!.memory.buffer, Number(p), bytes.length).set(bytes); return p; }
  /** grow guest memory once for a batch of files (one memory.grow instead of one per file) */
  reserve(bytes: number): boolean { return !!this.ex?.x64_reserve(BigInt(bytes)); }
  /** guest memory is bump-allocated, so large exe images are uploaded once and reused on every Run */
  private putCache = new WeakMap<Uint8Array, bigint>();
  private putCached(bytes: Uint8Array): bigint { let p = this.putCache.get(bytes); if (p === undefined) { p = this.put(bytes); this.putCache.set(bytes, p); } return p; }
  private putStr(s: string): bigint { return this.put(this.enc.encode(s + '\0')); }

  inspect(bytes: Uint8Array): PeInfo {
    const ex = this.ex!; const p = this.putCached(bytes);
    const json = this.cstr(ex.x64_inspect(p, BigInt(bytes.length)));
    try { return JSON.parse(json) as PeInfo; } catch { return { ok: false, error: 'inspector returned malformed data: ' + json.slice(0, 120) }; }
  }

  /** Load an exe and start the frame loop.  Returns the loader error text, or null on success. */
  start(bytes: Uint8Array, name: string, cmdline?: string): string | null {
    const ex = this.ex!; this.stop();
    const p = this.putCached(bytes);
    const rc = ex.x64_load(p, BigInt(bytes.length), this.putStr(name), this.putStr(cmdline ?? name));
    if (rc !== 0) { this.status = 'fault'; return this.cstr(ex.x64_report()) || `loader error ${rc}`; }
    this.status = 'running'; this.running = true; this.t0 = performance.now(); this.frames = 0; this.lastFrame = null;
    this.schedule();
    return null;
  }

  private schedule() {
    if (this.audio.active()) { this.timer = window.setTimeout(() => this.frame(), document.hidden ? 4 : 2); return; }   /* rAF stops in hidden tabs; audible tabs keep their timers */
    this.raf = requestAnimationFrame(() => this.frame());
  }
  /** one host frame: run guest code for up to ~12 ms or until it yields for a presented frame / sleep */
  private frame() {
    if (!this.running || !this.ex) return;
    const ex = this.ex; const start = performance.now(); let r = RUN_OK;
    for (let i = 0; i < 64; i++) {
      r = ex.x64_run(400_000n);
      if (r === RUN_YIELD) {
        if (ex.x64_wants_stdin()) { this.waitingStdin = true; this.status = 'waiting-stdin'; this.onStdinWait?.(true); return; }
        if (ex.x64_sleep_ms() > 0.5 || ex.x64_has_window()) break; continue;
      }
      if (r !== RUN_OK) break;
      if (performance.now() - start > 12) break;
    }
    if (r === RUN_EXIT || r === RUN_FAULT || r === RUN_BREAK) {
      this.running = false; this.status = r === RUN_EXIT ? 'exited' : r === RUN_FAULT ? 'fault' : 'break';
      this.onStop?.(this.stats());
      return;
    }
    this.schedule();
  }

  stop() { if (this.running) { this.running = false; this.status = 'stopped'; cancelAnimationFrame(this.raf); clearTimeout(this.timer); this.audio.flush(); if (this.waitingStdin) { this.waitingStdin = false; this.onStdinWait?.(false); } } }

  private resumeFromStdin() { if (this.waitingStdin && this.running) { this.waitingStdin = false; this.status = 'running'; this.onStdinWait?.(false); this.schedule(); } }
  /** feed text to the guest's stdin (a trailing newline is what line-oriented programs expect) */
  writeStdin(text: string) { if (!this.ex) return; const b = this.enc.encode(text); const p = this.put(b); this.ex.x64_stdin_write(p, BigInt(b.length)); this.resumeFromStdin(); }
  /** close stdin: blocked reads return 0 bytes (Ctrl-D / Ctrl-Z) */
  closeStdin() { if (!this.ex) return; this.ex.x64_stdin_eof(); this.resumeFromStdin(); }
  get waitingForStdin() { return this.waitingStdin; }

  /* ---- virtual file system (shared by every program run in this runtime; survives start()) ---- */
  vfsAdd(path: string, data: Uint8Array): boolean { const ex = this.ex!; return !!ex.x64_vfs_add(this.putStr(path), this.put(data), BigInt(data.length)); }
  vfsMkdir(path: string): boolean { return !!this.ex!.x64_vfs_mkdir(this.putStr(path)); }
  vfsRemove(path: string): boolean { return !!this.ex!.x64_vfs_remove(this.putStr(path)); }
  vfsList(): VfsEntry[] { if (!this.ex) return []; try { return JSON.parse(this.cstr(this.ex.x64_vfs_list())) as VfsEntry[]; } catch { return []; } }
  vfsRead(path: string): Uint8Array | null {
    const ex = this.ex!; const n = Number(ex.x64_vfs_size(this.putStr(path))); if (n < 0) return null;
    const p = ex.x64_vfs_data(this.putStr(path)); if (!p && n > 0) return null;
    return n ? this.bytesAt(p, n).slice() : new Uint8Array(0);
  }

  key(vk: number, down: boolean) { if (this.running) this.ex!.x64_key(vk, down ? 1 : 0); }
  mouse(x: number, y: number, ev: number) { if (this.running) this.ex!.x64_mouse(x | 0, y | 0, ev); }

  stats(): RunStats {
    const ex = this.ex; if (!ex) return { status: this.status, icount: 0, mips: 0, frames: 0, elapsedMs: 0, exitCode: 0, error: '', errorRip: '', errorBytes: '', report: '', title: '', subsystem: 0, hasWindow: false, memMB: 0, procs: 0, threads: 0, audioFrames: 0, audio: 'none' };
    const el = Math.max(1, performance.now() - this.t0); const ic = Number(ex.x64_icount());
    return {
      status: this.status, icount: ic, mips: ic / el / 1000, frames: this.frames, elapsedMs: el, exitCode: ex.x64_exit_code(),
      error: this.cstr(ex.x64_error()), errorRip: '0x' + ex.x64_error_rip().toString(16), errorBytes: this.cstr(ex.x64_error_bytes()), report: this.cstr(ex.x64_report()),
      title: this.cstr(ex.x64_title()), subsystem: ex.x64_subsystem(), hasWindow: !!ex.x64_has_window(), memMB: Number(ex.x64_mem_used()) / 1048576, procs: ex.x64_proc_count(), threads: ex.x64_thread_count ? ex.x64_thread_count() : 0, audioFrames: ex.x64_audio_frames ? Number(ex.x64_audio_frames()) : 0, audio: this.audio.state(),
    };
  }
  apiStats(): string { return this.ex ? this.cstr(this.ex.x64_api_stats()) : ''; }
  screenSize(): { w: number; h: number } { return this.ex ? { w: this.ex.x64_screen_w(), h: this.ex.x64_screen_h() } : { w: 0, h: 0 }; }
}

/** DOM KeyboardEvent → Windows virtual-key code */
export function vkFromEvent(e: KeyboardEvent): number {
  const c = e.code;
  if (/^Key[A-Z]$/.test(c)) return c.charCodeAt(3);
  if (/^Digit[0-9]$/.test(c)) return c.charCodeAt(5);
  if (/^Numpad[0-9]$/.test(c)) return 0x60 + (c.charCodeAt(6) - 48);
  if (/^F([1-9]|1[0-2])$/.test(c)) return 0x70 + parseInt(c.slice(1), 10) - 1;
  const map: Record<string, number> = {
    ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28, Space: 0x20, Escape: 0x1B, Enter: 0x0D, NumpadEnter: 0x0D, Tab: 0x09, Backspace: 0x08,
    ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11, AltLeft: 0x12, AltRight: 0x12, CapsLock: 0x14,
    Home: 0x24, End: 0x23, PageUp: 0x21, PageDown: 0x22, Insert: 0x2D, Delete: 0x2E, Pause: 0x13,
    Minus: 0xBD, Equal: 0xBB, Comma: 0xBC, Period: 0xBE, Slash: 0xBF, Semicolon: 0xBA, Quote: 0xDE, BracketLeft: 0xDB, BracketRight: 0xDD, Backslash: 0xDC, Backquote: 0xC0,
    NumpadAdd: 0x6B, NumpadSubtract: 0x6D, NumpadMultiply: 0x6A, NumpadDivide: 0x6F, NumpadDecimal: 0x6E,
  };
  return map[c] ?? 0;
}
