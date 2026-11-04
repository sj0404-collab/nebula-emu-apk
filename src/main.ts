import './style.css';
import { X64Runtime, wasm64Supported, type PeInfo, type RunStats } from './x64/runtime';
import { FramePresenter } from './x64/present';
import * as oc from './opencode-client';
import * as store from './store';
import * as workspace from './workspace';
import { isNative, startOnDeviceOpencode, NebulaEmu } from './native';

/* ---------- DOM ---------- */
const app = document.querySelector<HTMLDivElement>('#app')!;
app.innerHTML = `
<div class="layout">
  <header class="topbar">
    <div class="brand">Nebula <span>Emu</span></div>
    <span class="badge" id="emu-badge">wasm…</span>
    <span class="badge" id="oc-badge">opencode…</span>
    <div class="spacer"></div>
    <nav class="tabs">
      <button type="button" data-tab="emu" class="active">Эмулятор</button>
      <button type="button" data-tab="oc">opencode</button>
    </nav>
  </header>

  <div class="main" id="pane-emu">
    <aside class="sidebar">
      <div class="drop" id="drop">Перетащи .exe сюда<br/>или выбери файл</div>
      <input type="file" id="file" accept=".exe,application/octet-stream" class="hidden" />
      <button type="button" id="btn-inspect" class="hidden">PE inspect</button>
      <h3>Сэмплы</h3>
      <div id="samples"></div>
      <h3>Управление</h3>
      <button type="button" id="btn-stop" disabled>Стоп</button>
      <button type="button" id="btn-clear">Очистить консоль</button>
    </aside>
    <section class="stage">
      <div class="screen-wrap">
        <canvas id="screen" width="640" height="480"></canvas>
        <div class="placeholder" id="placeholder">
          Загрузи wasm-модуль и запусти PE-файл.<br/>
          Поддерживаются x86 (PE32) и x86-64 (PE32+) console/GUI.<br/>
          <code>npm run build:wasm</code> — сборка ядра (zig).
        </div>
      </div>
      <div class="console">
        <div class="console-head">
          <span>Консоль</span>
          <span class="spacer"></span>
          <span id="stat-mips">— MIPS</span>
        </div>
        <pre id="console"></pre>
        <div class="stdin-row" id="stdin-row">
          <input id="stdin" placeholder="stdin…" autocomplete="off" />
          <button type="button" id="stdin-send">⏎</button>
          <button type="button" id="stdin-eof">EOF</button>
        </div>
      </div>
    </section>
  </div>

  <div class="oc" id="pane-oc">
    <div class="oc-bar">
      <span class="badge" id="oc-state">probe</span>
      <span class="badge" id="oc-session">—</span>
      <span class="badge" id="oc-tok">0 tok</span>
      <label class="oc-model" title="Модель для следующих сообщений">
        <span>model</span>
        <select id="oc-model"></select>
      </label>
      <div class="spacer"></div>
      <input type="text" id="oc-endpoint" placeholder="endpoint (пусто = авто)" />
      <button type="button" id="oc-save">OK</button>
      <button type="button" id="oc-retry" title="Переподключить">↻</button>
    </div>
    <div class="oc-body">
      <aside class="oc-sess" aria-label="Сессии">
        <div class="oc-sess-actions">
          <button type="button" id="oc-new" title="Новая сессия">+ new</button>
          <button type="button" id="oc-ws" title="Выбрать рабочую папку">📁</button>
          <button type="button" id="oc-export" title="Экспорт текущей сессии (JSON)">⬇</button>
          <button type="button" id="oc-import" title="Импорт сессии из JSON">⬆</button>
          <input type="file" id="oc-import-file" accept="application/json,.json" class="hidden" />
        </div>
        <div class="oc-sess-list" id="oc-sess-list"></div>
        <div class="oc-ws-path" id="oc-ws-path" title="Рабочая папка opencode">workspace: —</div>
      </aside>
      <div class="oc-main">
        <div class="oc-tui" id="oc-log" aria-live="polite"></div>
        <div class="oc-statusline">
          <span id="oc-mode">normal</span>
          <span id="oc-model-label">model: —</span>
          <span class="spacer"></span>
          <span id="oc-hint">Enter — отправить · Shift+Enter — перенос · Ctrl+L — clear · сессии переключаются в любой момент</span>
        </div>
        <div class="oc-compose">
          <span class="oc-prompt" id="oc-prompt">❯</span>
          <textarea id="oc-input" rows="3" placeholder="Сообщение opencode…"></textarea>
          <button type="button" class="primary" id="oc-send">Send</button>
        </div>
      </div>
    </div>
  </div>

  <footer class="statusbar">
    <span>status: <strong id="st-status">idle</strong></span>
    <span>ins: <strong id="st-ins">0</strong></span>
    <span>fps: <strong id="st-fps">0</strong></span>
    <span>mem: <strong id="st-mem">0</strong> MB</span>
    <span>title: <strong id="st-title">—</strong></span>
    <span class="spacer"></span>
    <span id="st-pe">—</span>
  </footer>
</div>`;

const $ = <T extends HTMLElement>(sel: string) => document.querySelector(sel) as T;
const consoleEl = $('#console');
const screenCanvas = $<HTMLCanvasElement>('#screen');
const placeholder = $('#placeholder');
const fileInput = $<HTMLInputElement>('#file');
const dropEl = $('#drop');
const stdinRow = $('#stdin-row');
const stdinInput = $<HTMLInputElement>('#stdin');

function log(text: string, cls?: string): void {
  const line = document.createElement('span');
  if (cls) line.className = cls;
  line.textContent = text.endsWith('\n') ? text : text + '\n';
  consoleEl.appendChild(line);
  consoleEl.scrollTop = consoleEl.scrollHeight;
}

function logErr(text: string): void {
  log(text, 'err');
}

/* ---------- samples ---------- */
interface Sample {
  name: string;
  href: string;
  desc: string;
  args?: string;
}
const SAMPLES: Sample[] = [
  { name: 'hello.exe', href: '/x64/samples/hello.exe', desc: 'UCRT console — printf/qsort, exit 7' },
  { name: 'hello32.exe', href: '/x64/samples/hello32.exe', desc: '32-bit PE32 console (IA-32 mode)' },
  { name: 'bench.exe', href: '/x64/samples/bench.exe', desc: 'CPU: sieve / mandelbrot / memcpy' },
  { name: 'threads.exe', href: '/x64/samples/threads.exe', desc: 'Win32 threads + sync, 39 checks' },
  { name: 'NebulaStrike32.exe', href: '/x64/samples/NebulaStrike32.exe', desc: 'GDI-игра, 32-bit PE' },
  { name: 'seh32.exe', href: '/x64/samples/seh32.exe', desc: '32-bit SEH self-test' },
  { name: 'cxxexc.exe', href: '/x64/samples/cxxexc.exe', desc: 'C++ exceptions (x64 SEH)' },
  { name: 'd3d11tri.exe', href: '/x64/samples/d3d11tri.exe', desc: 'D3D11 → WebGL2 triangle', args: '3' },
  { name: 'cmd.exe', href: '/x64/apps/cmd.exe', desc: 'Встроенный Command Prompt', args: '/k demo.bat' },
  { name: 'busybox64.exe', href: '/x64/samples/busybox64.exe', desc: 'busybox-w32: ls, sh, pipelines' },
  { name: '7z.exe', href: '/x64/apps/7z.exe', desc: '7-Zip 25.01 (+7z.dll LoadLibrary)', args: 'a -bb0 -mx1 demo.7z demo.txt', },
  { name: 'sqlite3.exe', href: '/x64/apps/sqlite3.exe', desc: 'SQLite 3.50.4 shell', args: '-init demo.sql demo.db .quit' },
  { name: 'rg.exe', href: '/x64/apps/rg.exe', desc: 'ripgrep 14 (Rust MSVC)', args: '-n -i "lua|sqlite" demo.lua demo.sql demo.json' },
];

const samplesEl = $('#samples');
for (const s of SAMPLES) {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'sample';
  b.innerHTML = `<strong>${s.name}</strong><small>${s.desc}</small>`;
  b.addEventListener('click', () => void runUrl(s.href, s.name, s.args));
  samplesEl.appendChild(b);
}

/* ---------- runtime ---------- */
const rt = new X64Runtime();
let presenter: FramePresenter | null = null;
let ready = false;
let currentBytes: Uint8Array | null = null;
let currentArgs = '';
let framesWindow = 0;
let lastFpsAt = performance.now();

rt.onConsole = (fd, text) => {
  if (fd === 2) logErr(text);
  else log(text);
};
rt.onLog = (text) => log(text, 'sys');
rt.onPresent = (px, w, h) => {
  if (!presenter) {
    try {
      presenter = new FramePresenter(screenCanvas);
    } catch (e) {
      logErr(`WebGL: ${String(e)}`);
      return;
    }
  }
  screenCanvas.classList.add('visible');
  placeholder.classList.add('hidden');
  presenter.present(px, w, h);
  framesWindow++;
};
rt.onStop = (stats) => updateStats(stats);
rt.onStdinWait = (waiting) => {
  stdinRow.classList.toggle('active', waiting);
  if (waiting) stdinInput.focus();
};

function updateStats(s: RunStats): void {
  $('#st-status').textContent = s.status;
  $('#st-ins').textContent = s.icount.toLocaleString('ru-RU');
  $('#st-mem').textContent = s.memMB.toFixed(1);
  $('#st-title').textContent = s.title || '—';
  if (s.status === 'exited') log(`[exit ${s.exitCode}]`, 'ok');
  if (s.status === 'fault') {
    logErr(`[fault] ${s.error} @ ${s.errorRip} ${s.errorBytes}`);
    if (s.report) log(s.report, 'sys');
  }
  ($('#btn-stop') as HTMLButtonElement).disabled = true;
  if (s.frames > 0) log(`[frames ${s.frames}, ${(s.icount / s.elapsedMs / 1000).toFixed(1)} MIPS]`, 'sys');
}

function tickStatus(): void {
  if (rt.status === 'running') {
    const now = performance.now();
    const dt = (now - lastFpsAt) / 1000;
    if (dt >= 0.5) {
      const fps = framesWindow / dt;
      $('#st-fps').textContent = fps.toFixed(0);
      const s = rt.stats();
      $('#stat-mips').textContent = `${s.mips.toFixed(1)} MIPS`;
      $('#st-ins').textContent = s.icount.toLocaleString('ru-RU');
      $('#st-mem').textContent = s.memMB.toFixed(1);
      framesWindow = 0;
      lastFpsAt = now;
    }
    requestAnimationFrame(tickStatus);
  } else {
    framesWindow = 0;
    lastFpsAt = performance.now();
  }
}

async function ensureRuntime(): Promise<boolean> {
  if (ready) return true;
  if (!wasm64Supported()) {
    $('#emu-badge').textContent = 'no memory64';
    $('#emu-badge').className = 'badge off';
    logErr('Браузер без memory64 (wasm64). Нужен Chromium ≥ 133 / Firefox ≥ 134 / Node ≥ 22.');
    return false;
  }
  try {
    await rt.init('/x64/nebula_x64.wasm');
    ready = true;
    $('#emu-badge').textContent = `wasm ${Math.round(rt.moduleBytes / 1024)} KB`;
    $('#emu-badge').className = 'badge live';
    log(`[emu] module loaded (${(rt.moduleBytes / 1024).toFixed(0)} KB)`, 'ok');
    return true;
  } catch (e) {
    $('#emu-badge').textContent = 'load error';
    $('#emu-badge').className = 'badge off';
    logErr(`[emu] ${String(e)} — запусти npm run build:wasm`);
    return false;
  }
}

function peLine(info: PeInfo): string {
  if (!info.ok) return `PE error: ${info.error ?? 'unknown'}`;
  const v = info.verdict ?? 'unknown';
  const imp = info.imports != null ? `${info.knownImports ?? 0}/${info.imports}` : '?';
  return `${info.arch} ${info.subsystem ?? ''} entry=${info.entry ?? '?'} imports=${imp} verdict=${v}${info.why ? ' — ' + info.why : ''}`;
}

async function runBytes(bytes: Uint8Array, name: string, args?: string): Promise<void> {
  if (!(await ensureRuntime())) return;
  currentBytes = bytes;
  currentArgs = args ?? name;
  try {
    const info = rt.inspect(bytes);
    $('#st-pe').textContent = peLine(info);
    log(`[inspect] ${peLine(info)}`, 'sys');
  } catch (e) {
    logErr(`[inspect] ${String(e)}`);
  }
  const err = rt.start(bytes, name, currentArgs);
  if (err) {
    logErr(err);
    $('#st-status').textContent = 'fault';
    return;
  }
  ($('#btn-stop') as HTMLButtonElement).disabled = false;
  $('#st-status').textContent = 'running';
  log(`[run] ${name} ${args ?? ''}`, 'ok');
  tickStatus();
}

async function runUrl(href: string, name: string, args?: string): Promise<void> {
  try {
    log(`[load] ${href}`, 'sys');
    const r = await fetch(href);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const buf = new Uint8Array(await r.arrayBuffer());
    await runBytes(buf, name, args);
  } catch (e) {
    logErr(`[load] ${String(e)}`);
  }
}

/* ---------- file drop ---------- */
dropEl.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => {
  const f = fileInput.files?.[0];
  if (f) void loadFile(f);
});
dropEl.addEventListener('dragover', (e) => {
  e.preventDefault();
  dropEl.classList.add('over');
});
dropEl.addEventListener('dragleave', () => dropEl.classList.remove('over'));
dropEl.addEventListener('drop', (e) => {
  e.preventDefault();
  dropEl.classList.remove('over');
  const f = e.dataTransfer?.files?.[0];
  if (f) void loadFile(f);
});

async function loadFile(file: File): Promise<void> {
  const buf = new Uint8Array(await file.arrayBuffer());
  const name = file.name.endsWith('.exe') ? file.name : file.name + '.exe';
  await runBytes(buf, name, name);
}

$('#btn-stop').addEventListener('click', () => {
  rt.stop();
  $('#st-status').textContent = 'stopped';
  ($('#btn-stop') as HTMLButtonElement).disabled = true;
  log('[stop]', 'sys');
});
$('#btn-clear').addEventListener('click', () => {
  consoleEl.textContent = '';
});
$('#btn-inspect')?.addEventListener('click', () => {
  if (!currentBytes) return;
  try {
    log(`[inspect] ${peLine(rt.inspect(currentBytes))}`, 'sys');
  } catch (e) {
    logErr(String(e));
  }
});

function sendStdin(): void {
  const v = stdinInput.value;
  stdinInput.value = '';
  rt.writeStdin(v.endsWith('\n') ? v : v + '\n');
}
$('#stdin-send').addEventListener('click', sendStdin);
stdinInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    sendStdin();
  }
});
$('#stdin-eof').addEventListener('click', () => rt.closeStdin());

/* keyboard → guest when running */
window.addEventListener('keydown', (e) => {
  if (rt.status !== 'running') return;
  if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return;
  const vk = vkFrom(e);
  if (vk) {
    rt.key(vk, true);
    if (!e.metaKey && !e.ctrlKey) e.preventDefault();
  }
});
window.addEventListener('keyup', (e) => {
  if (rt.status !== 'running') return;
  const vk = vkFrom(e);
  if (vk) rt.key(vk, false);
});
screenCanvas.addEventListener('mousemove', (e) => {
  if (rt.status !== 'running' || !presenter) return;
  const rect = screenCanvas.getBoundingClientRect();
  const g = presenter.toGuest(e.clientX - rect.left, e.clientY - rect.top, ...screenSize());
  if (g) rt.mouse(g.x, g.y, 0);
});
screenCanvas.addEventListener('mousedown', (e) => {
  if (rt.status !== 'running' || !presenter) return;
  const rect = screenCanvas.getBoundingClientRect();
  const g = presenter.toGuest(e.clientX - rect.left, e.clientY - rect.top, ...screenSize());
  if (g) rt.mouse(g.x, g.y, e.button === 2 ? 3 : 1);
});
screenCanvas.addEventListener('mouseup', (e) => {
  if (rt.status !== 'running' || !presenter) return;
  const rect = screenCanvas.getBoundingClientRect();
  const g = presenter.toGuest(e.clientX - rect.left, e.clientY - rect.top, ...screenSize());
  if (g) rt.mouse(g.x, g.y, e.button === 2 ? 4 : 2);
});
screenCanvas.addEventListener('contextmenu', (e) => e.preventDefault());

function screenSize(): [number, number] {
  const s = rt.screenSize();
  return [s.w || 640, s.h || 480];
}

function vkFrom(e: KeyboardEvent): number {
  const c = e.code;
  if (/^Key[A-Z]$/.test(c)) return c.charCodeAt(3);
  if (/^Digit[0-9]$/.test(c)) return c.charCodeAt(5);
  if (/^F([1-9]|1[0-2])$/.test(c)) return 0x70 + parseInt(c.slice(1), 10) - 1;
  const map: Record<string, number> = {
    ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28,
    Space: 0x20, Escape: 0x1b, Enter: 0x0d, Tab: 0x09, Backspace: 0x08,
    ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11,
    AltLeft: 0x12, AltRight: 0x12, Home: 0x24, End: 0x23, PageUp: 0x21, PageDown: 0x22,
    Delete: 0x2e, Insert: 0x2d,
  };
  return map[c] ?? 0;
}

/* ---------- tabs ---------- */
const tabs = document.querySelectorAll<HTMLButtonElement>('.tabs button');
const paneEmu = $('#pane-emu');
const paneOc = $('#pane-oc');
tabs.forEach((btn) => {
  btn.addEventListener('click', () => {
    tabs.forEach((b) => b.classList.remove('active'));
    btn.classList.add('active');
    const t = btn.dataset.tab;
    paneEmu.classList.toggle('hidden', t !== 'emu');
    paneOc.classList.toggle('active', t === 'oc');
    if (t === 'oc') void initOpencode();
  });
});

/* ---------- opencode TUI pane ---------- */
const ocLog = $('#oc-log');
const ocState = $('#oc-state');
const ocSessionEl = $('#oc-session');
const ocTokEl = $('#oc-tok');
const ocMode = $('#oc-mode');
const ocModelSel = $<HTMLSelectElement>('#oc-model');
const ocModelLabel = $('#oc-model-label');
const ocInput = $<HTMLTextAreaElement>('#oc-input');
const ocSendBtn = $<HTMLButtonElement>('#oc-send');
const ocSessList = $('#oc-sess-list');
const ocWsPathEl = $('#oc-ws-path');
const ocImportFile = $<HTMLInputElement>('#oc-import-file');
let ocInfo: oc.BridgeInfo | null = null;
let ocInited = false;
let ocCloser: (() => void) | null = null;
let ocServerOnline = false;

interface OcLine {
  role: string;
  id?: string;
  text: string;
}

/** Per-session view state — survives tab switch and reload (via IndexedDB). */
interface OcView {
  id: string;
  title: string;
  lines: OcLine[];
  accum: Map<string, string>;
  draft: string;
  busy: boolean;
  tokens: number;
  /** true when this id exists on the current opencode server */
  onServer: boolean;
  updatedAt: number;
}

const ocViews = new Map<string, OcView>();
let ocSession: string | null = null;
let ocDraftTimer: number | undefined;

function ocActiveView(): OcView | null {
  return ocSession ? (ocViews.get(ocSession) ?? null) : null;
}

function ocEnsureView(id: string, title?: string): OcView {
  let v = ocViews.get(id);
  if (!v) {
    v = {
      id,
      title: title || id.slice(0, 10),
      lines: [],
      accum: new Map(),
      draft: '',
      busy: false,
      tokens: 0,
      onServer: false,
      updatedAt: Date.now(),
    };
    ocViews.set(id, v);
  } else if (title) {
    v.title = title;
  }
  return v;
}

function ocSetBusyFor(sid: string, busy: boolean): void {
  const v = ocViews.get(sid);
  if (!v) return;
  v.busy = busy;
  v.updatedAt = Date.now();
  if (sid === ocSession) {
    ocSendBtn.disabled = busy;
    ocMode.textContent = busy ? 'working' : 'normal';
    ocMode.className = busy ? 'busy' : '';
    if (busy) {
      if (!document.getElementById('oc-cursor')) {
        const d = document.createElement('div');
        d.id = 'oc-cursor';
        d.className = 'tui-line sys';
        d.textContent = '█';
        ocLog.appendChild(d);
        ocLog.scrollTop = ocLog.scrollHeight;
      }
    } else {
      document.getElementById('oc-cursor')?.remove();
    }
  }
  ocRenderSessionList();
  void store.saveSessionMeta({
    id: sid,
    title: v.title,
    busy,
    updatedAt: v.updatedAt,
  });
}

function ocSetBusy(busy: boolean): void {
  if (ocSession) ocSetBusyFor(ocSession, busy);
}

function ocUpdateModelLabel(): void {
  const ref = ocModelSel.value || oc.getSavedModel() || 'opencode/mimo-v2.6-flash-free';
  ocModelLabel.textContent = `model: ${ref}`;
}

/** Preferred default: free model (opencode free first, then any free). */
function pickDefaultFree(list: oc.ModelChoice[]): string {
  const order = [
    'opencode/mimo-v2.6-flash-free',
    'opencode/mimo-v2.5-free',
    'opencode/big-pickle',
    'pollinations/openai-fast',
  ];
  for (const ref of order) {
    if (list.some((m) => m.ref === ref && m.free)) return ref;
  }
  const free = list.find((m) => m.free && m.status !== 'deprecated');
  return free?.ref ?? '';
}

async function ocFillModels(): Promise<void> {
  const saved = oc.getSavedModel();
  try {
    const raw = await oc.listModels();
    const list = oc.withPollinationsFallback(raw);
    ocModelSel.innerHTML = '';

    const def = document.createElement('option');
    def.value = '';
    def.textContent = 'server default';
    ocModelSel.appendChild(def);

    // --- tree: FREE by provider first, then the rest by provider ---
    const byProvider = new Map<string, oc.ModelChoice[]>();
    for (const m of list) {
      const key = m.providerID;
      if (!byProvider.has(key)) byProvider.set(key, []);
      byProvider.get(key)!.push(m);
    }

    const makeGroup = (label: string, items: oc.ModelChoice[]): void => {
      if (!items.length) return;
      const g = document.createElement('optgroup');
      g.label = label;
      for (const m of items) {
        if (m.status && m.status !== 'active' && m.status !== 'deprecated') continue;
        const o = document.createElement('option');
        o.value = m.ref;
        const ctx = m.context ? ` · ${(m.context / 1000).toFixed(0)}k` : '';
        const off = m.status === 'deprecated' ? ' (off)' : '';
        o.textContent = `${m.name} (${m.ref})${ctx}${off}`;
        if (m.status === 'deprecated') o.disabled = true;
        g.appendChild(o);
      }
      if (g.children.length) ocModelSel.appendChild(g);
    };

    // FREE groups — providers that have free models (Pollinations always present as fallback)
    const providerOrder = [
      'opencode',
      'pollinations',
      'orcarouter',
      'merge-gateway',
      'openrouter',
      'freemodel',
    ];
    const seen = new Set<string>();
    for (const pid of providerOrder) {
      const items = (byProvider.get(pid) ?? []).filter((m) => m.free);
      makeGroup(`FREE · ${pid}`, items);
      seen.add(pid);
    }
    for (const [pid, items] of byProvider) {
      if (seen.has(pid)) continue;
      const freeItems = items.filter((m) => m.free);
      if (freeItems.length) makeGroup(`FREE · ${pid}`, freeItems);
    }

    // PAID / non-free groups
    for (const pid of providerOrder) {
      const items = (byProvider.get(pid) ?? []).filter((m) => !m.free);
      makeGroup(`${pid} (paid/other)`, items);
    }
    for (const [pid, items] of byProvider) {
      if (providerOrder.includes(pid)) continue;
      const rest = items.filter((m) => !m.free);
      if (rest.length) makeGroup(`${pid} (paid/other)`, rest);
    }

    // restore: saved if present, else first free
    let preferred = saved;
    if (!preferred || ![...ocModelSel.options].some((o) => o.value === preferred)) {
      preferred = pickDefaultFree(list);
      if (preferred) oc.setSavedModel(preferred);
    }
    if (preferred && [...ocModelSel.options].some((o) => o.value === preferred)) {
      ocModelSel.value = preferred;
    } else {
      ocModelSel.value = '';
    }
    ocUpdateModelLabel();
    const freeCount = list.filter((m) => m.free).length;
    log(`[opencode] models: ${list.length} (${freeCount} free) · default ${ocModelSel.value || 'server'}`, 'sys');
  } catch (e) {
    log(`[opencode] models list failed: ${String(e)}`, 'sys');
    ocModelSel.innerHTML = '';
    const opts = [
      { v: 'opencode/mimo-v2.6-flash-free', t: 'FREE · opencode/mimo-v2.6-flash-free' },
      { v: 'opencode/mimo-v2.5-free', t: 'FREE · opencode/mimo-v2.5-free' },
      { v: 'pollinations/openai-fast', t: 'FREE · pollinations/openai-fast (fallback)' },
      { v: 'opencode/big-pickle', t: 'opencode/big-pickle' },
    ];
    for (const { v, t } of opts) {
      const o = document.createElement('option');
      o.value = v;
      o.textContent = t;
      ocModelSel.appendChild(o);
    }
    const preferred = saved && opts.some((x) => x.v === saved) ? saved : opts[0].v;
    ocModelSel.value = preferred;
    oc.setSavedModel(preferred);
    ocUpdateModelLabel();
  }
}

ocModelSel.addEventListener('change', () => {
  oc.setSavedModel(ocModelSel.value);
  ocUpdateModelLabel();
  ocAppendLine('sys', `model → ${ocModelSel.value || 'server default'}`);
});

function ocRoleLabel(role: string): string {
  return role === 'user'
    ? '❯ you'
    : role === 'error'
      ? '✗ err'
      : role === 'sys'
        ? '· sys'
        : '◆ oc';
}

function ocMakeLineEl(line: OcLine): HTMLElement {
  const div = document.createElement('div');
  div.className = `tui-line ${line.role}`;
  if (line.id) div.dataset.mid = line.id;
  const roleEl = document.createElement('span');
  roleEl.className = 'role';
  roleEl.textContent = ocRoleLabel(line.role);
  const body = document.createElement('span');
  body.className = 'body';
  body.textContent = line.text;
  div.append(roleEl, body);
  return div;
}

/** Rebuild visible log from the active view (on switch / history load). */
function ocRenderActive(): void {
  ocLog.innerHTML = '';
  const v = ocActiveView();
  if (!v) return;
  for (const line of v.lines) ocLog.appendChild(ocMakeLineEl(line));
  if (v.busy) {
    const d = document.createElement('div');
    d.id = 'oc-cursor';
    d.className = 'tui-line sys';
    d.textContent = '█';
    ocLog.appendChild(d);
  }
  ocLog.scrollTop = ocLog.scrollHeight;
  ocSendBtn.disabled = v.busy;
  ocMode.textContent = v.busy ? 'working' : 'normal';
  ocMode.className = v.busy ? 'busy' : '';
  ocSessionEl.textContent = v.id.slice(0, 10);
  ocTokEl.textContent = `${v.tokens} tok`;
  ocInput.value = v.draft;
}

/**
 * Append or upsert a line in a session view.
 * If that session is active → mutate DOM; otherwise only the model (rendered on switch).
 */
function ocUpsertLine(sid: string | null, role: string, text: string, id?: string): void {
  if (!sid) {
    // global / unknown session → active view
    sid = ocSession;
  }
  if (!sid) return;
  const v = ocEnsureView(sid);
  if (id) {
    const existing = v.lines.find((l) => l.id === id);
    if (existing) {
      existing.text = text;
      if (sid === ocSession) {
        const el = ocLog.querySelector<HTMLElement>(`[data-mid="${CSS.escape(id)}"] .body`);
        if (el) {
          el.textContent = text;
          ocLog.scrollTop = ocLog.scrollHeight;
        } else {
          ocRenderActive();
        }
      }
      return;
    }
  }
  const line: OcLine = { role, text, id };
  v.lines.push(line);
  v.updatedAt = Date.now();
  if (sid === ocSession) {
    const el = ocMakeLineEl(line);
    // drop cursor, append, re-add cursor
    document.getElementById('oc-cursor')?.remove();
    ocLog.appendChild(el);
    if (v.busy) {
      const d = document.createElement('div');
      d.id = 'oc-cursor';
      d.className = 'tui-line sys';
      d.textContent = '█';
      ocLog.appendChild(d);
    }
    ocLog.scrollTop = ocLog.scrollHeight;
  }
}

function ocAppendLine(role: string, text: string, id?: string): void {
  if (!ocSession) {
    // no active session yet — render into the log without a view slot
    const el = ocMakeLineEl({ role, text, id });
    document.getElementById('oc-cursor')?.remove();
    ocLog.appendChild(el);
    ocLog.scrollTop = ocLog.scrollHeight;
    return;
  }
  ocUpsertLine(ocSession, role, text, id);
}

/** Persist view lines → IndexedDB (JSON of OcMessageEnvelope-like). */
function ocSerializeLines(v: OcView): unknown[] {
  // group consecutive by role+id stream into pseudo-messages for export readability
  return v.lines.map((l) => ({
    info: { id: l.id ?? `local-${Math.random().toString(36).slice(2)}`, role: l.role === 'user' ? 'user' : 'assistant', sessionID: v.id },
    parts: [{ id: l.id ?? 'p', messageID: l.id ?? 'p', type: 'text', text: l.text }],
    localRole: l.role,
  }));
}

async function ocCacheView(v: OcView): Promise<void> {
  try {
    await store.saveMessages(v.id, JSON.stringify(ocSerializeLines(v)));
    await store.saveSessionMeta({
      id: v.id,
      title: v.title,
      busy: v.busy,
      updatedAt: Date.now(),
    });
    if (v.draft) await store.saveDraft(v.id, v.draft);
    else await store.clearDraft(v.id);
  } catch {
    /* ignore quota */
  }
}

function ocRenderSessionList(): void {
  ocSessList.innerHTML = '';
  const items = [...ocViews.values()].sort((a, b) => b.updatedAt - a.updatedAt);
  if (!items.length) {
    const empty = document.createElement('div');
    empty.className = 'oc-sess-empty';
    empty.textContent = 'нет сессий';
    ocSessList.appendChild(empty);
    return;
  }
  for (const v of items) {
    const row = document.createElement('div');
    row.className = 'oc-sess-item' + (v.id === ocSession ? ' active' : '') + (v.busy ? ' busy' : '');
    row.title = v.id;
    row.tabIndex = 0;

    const main = document.createElement('button');
    main.type = 'button';
    main.className = 'oc-sess-main';
    const title = document.createElement('span');
    title.className = 'oc-sess-title';
    title.textContent = v.title || v.id.slice(0, 10);
    const meta = document.createElement('span');
    meta.className = 'oc-sess-meta';
    meta.textContent = (v.busy ? '● ' : '') + (v.onServer ? '' : 'local ') + v.id.slice(0, 8);
    main.append(title, meta);
    main.addEventListener('click', () => void ocSwitchSession(v.id));

    const tools = document.createElement('span');
    tools.className = 'oc-sess-tools';

    const ren = document.createElement('button');
    ren.type = 'button';
    ren.className = 'oc-sess-btn';
    ren.title = 'Переименовать';
    ren.textContent = '✎';
    ren.addEventListener('click', (e) => {
      e.stopPropagation();
      void ocRenameSession(v.id);
    });

    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'oc-sess-btn';
    del.title = 'Удалить';
    del.textContent = '✕';
    del.addEventListener('click', (e) => {
      e.stopPropagation();
      void ocDeleteSession(v.id);
    });

    tools.append(ren, del);
    row.append(main, tools);
    ocSessList.appendChild(row);
  }
}

function ocUpdateWsLabel(): void {
  const ws = workspace.getWorkspace();
  const path = ws?.path || workspace.workspacePath() || 'default';
  const short = path.length > 42 ? '…' + path.slice(-40) : path;
  ocWsPathEl.textContent = `workspace: ${short}`;
  ocWsPathEl.title = path;
}

async function initOpencode(): Promise<void> {
  if (ocInited) return;
  ocInited = true;
  const saved = oc.getEndpoint();
  ($('#oc-endpoint') as HTMLInputElement).value = saved;
  await workspace.initWorkspace();
  ocUpdateWsLabel();
  await ocRestoreFromStore();
  await refreshOc();
}

/** Load cached sessions/drafts from IndexedDB so reload/offline still shows work. */
async function ocRestoreFromStore(): Promise<void> {
  try {
    const metas = await store.listStoredSessions();
    const openIds = await store.loadOpenTabs();
    const activeId = await store.loadActiveTab();
    for (const m of metas) {
      const v = ocEnsureView(m.id, m.title);
      if (m.busy) {
        // server died mid-flight — do not stay locked forever
        v.busy = false;
      }
      const json = await store.loadMessagesJson(m.id);
      if (json && v.lines.length === 0) {
        try {
          const arr = JSON.parse(json) as {
            info?: { role?: string };
            parts?: { text?: string; id?: string; messageID?: string }[];
            localRole?: string;
          }[];
          for (const env of arr) {
            const role = env.localRole ?? (env.info?.role === 'user' ? 'user' : 'assistant');
            for (const p of env.parts ?? []) {
              if (p.text) v.lines.push({ role, text: p.text, id: p.id ?? p.messageID });
            }
          }
        } catch { /* corrupt cache */ }
      }
      v.draft = await store.loadDraft(m.id);
      v.updatedAt = m.updatedAt;
    }
    // restore open tab order preference (ids only — views already exist)
    if (openIds.length) {
      for (const id of openIds) if (!ocViews.has(id)) ocEnsureView(id);
    }
    if (activeId && ocViews.has(activeId)) {
      ocSwitchSession(activeId);
    } else if (ocViews.size) {
      const first = [...ocViews.values()].sort((a, b) => b.updatedAt - a.updatedAt)[0];
      ocSwitchSession(first.id);
    }
    ocRenderSessionList();
  } catch (e) {
    log(`[opencode] store restore: ${String(e)}`, 'sys');
  }
}

async function ocSwitchSession(id: string): Promise<void> {
  if (ocSession === id) {
    ocRenderActive();
    return;
  }
  // save outgoing draft
  const prev = ocActiveView();
  if (prev) {
    prev.draft = ocInput.value;
    void ocCacheView(prev);
  }
  ocSession = id;
  const v = ocEnsureView(id);
  v.draft = v.draft || (await store.loadDraft(id).catch(() => ''));
  ocRenderActive();
  ocRenderSessionList();
  void store.saveActiveTab(id);
  void store.saveOpenTabs([...ocViews.keys()]);

  // pull fresh history from server when online
  if (v.onServer && ocServerOnline) {
    try {
      const list = await oc.listMessages(id);
      if (list.length) {
        v.lines = [];
        v.accum.clear();
        for (const env of list) {
          const role = env.info.role === 'user' ? 'user' : 'assistant';
          for (const p of env.parts) {
            if (p.type === 'text' && p.text) {
              v.lines.push({ role, text: p.text, id: p.messageID || p.id });
            } else if (p.type === 'tool') {
              const title = p.state?.title ?? p.tool ?? 'tool';
              const st = p.state?.status ?? '';
              const mark = st === 'error' ? '✗' : st === 'completed' ? '✓' : st === 'running' ? '⋯' : '⚙';
              v.lines.push({ role: 'sys', text: `${mark} ${title}${st ? ` [${st}]` : ''}`, id: `tool-${p.id}` });
            }
          }
        }
        ocRenderActive();
        void ocCacheView(v);
      }
    } catch {
      /* offline — keep cache */
    }
  }
}

async function ocRenameSession(id: string): Promise<void> {
  const v = ocEnsureView(id);
  const next = window.prompt('Название сессии:', v.title);
  if (next === null) return;
  const title = next.trim() || v.title;
  v.title = title;
  v.updatedAt = Date.now();
  if (v.onServer && ocServerOnline) {
    try {
      await oc.renameSession(id, title);
    } catch (e) {
      log(`[opencode] rename server: ${String(e)}`, 'sys');
    }
  }
  await store.saveSessionMeta({ id, title, busy: v.busy, updatedAt: v.updatedAt });
  ocRenderSessionList();
}

async function ocDeleteSession(id: string): Promise<void> {
  const v = ocViews.get(id);
  const title = v?.title ?? id;
  if (!window.confirm(`Удалить сессию «${title}»?`)) return;
  if (v?.onServer && ocServerOnline) {
    try {
      await oc.deleteSession(id);
    } catch (e) {
      log(`[opencode] delete server: ${String(e)}`, 'sys');
    }
  }
  ocViews.delete(id);
  await store.removeStoredSession(id);
  if (ocSession === id) {
    const next = [...ocViews.values()].sort((a, b) => b.updatedAt - a.updatedAt)[0];
    if (next) await ocSwitchSession(next.id);
    else await newOcSession();
  }
  ocRenderSessionList();
}

async function refreshOc(): Promise<void> {
  ocState.textContent = 'probe…';
  ocState.className = 'badge warn';
  ocInfo = await oc.probe();
  if (ocInfo.ok) {
    ocServerOnline = true;
    ocState.textContent = `live · ${ocInfo.opencode ?? '?'} · ${ocInfo.source ?? ''}`;
    ocState.className = 'badge live';
    log(`[opencode] connected: v${ocInfo.opencode} via ${ocInfo.source}`, 'ok');
    await ocFillModels();

    // merge server session list into views (scoped to current workspace when set)
    try {
      const remote = await oc.listSessions();
      for (const s of remote) {
        const v = ocEnsureView(s.id, s.title || s.id.slice(0, 10));
        v.onServer = true;
        if (s.title) v.title = s.title;
        v.updatedAt = s.time?.updated ?? v.updatedAt;
      }
      // hide server sessions from other folders while a workspace is active
      const wsPath = workspace.workspacePath();
      if (wsPath) {
        const keep = new Set(remote.map((s) => s.id));
        if (ocSession && !keep.has(ocSession)) {
          // active session is from another folder → switch to a matching one if any
          const alt = remote
            .slice()
            .sort((a, b) => (b.time?.updated ?? 0) - (a.time?.updated ?? 0))[0];
          if (alt) await ocSwitchSession(alt.id);
        }
        for (const id of [...ocViews.keys()]) {
          const v = ocViews.get(id)!;
          if (v.onServer && !keep.has(id) && id !== ocSession) ocViews.delete(id);
        }
      }
    } catch (e) {
      log(`[opencode] list sessions: ${String(e)}`, 'sys');
    }

    if (!ocSession) {
      const first = [...ocViews.values()].sort((a, b) => b.updatedAt - a.updatedAt)[0];
      if (first) await ocSwitchSession(first.id);
      else await newOcSession();
    } else {
      const v = ocViews.get(ocSession);
      if (v && !v.onServer && ocServerOnline) {
        // local-only cache — try open on server
        try {
          const remote = await oc.listSessions();
          const hit = remote.find((s) => s.id === ocSession);
          if (hit) {
            v.onServer = true;
            if (hit.title) v.title = hit.title;
          }
        } catch { /* ignore */ }
      }
      ocRenderActive();
    }
    startOcEvents();
    ocSetBusy(ocActiveView()?.busy ?? false);
    ocRenderSessionList();
  } else {
    ocServerOnline = false;
    ocState.textContent = 'offline (cache)';
    ocState.className = 'badge warn';
    ocAppendLine('error', `opencode недоступен: ${ocInfo.error ?? 'no bridge'}\nПоказаны сессии из браузерного хранилища.\nДесктоп: npm run opencode · Android: встроенный serve`);
    // still allow viewing cached sessions
    if (!ocSession && ocViews.size) {
      const first = [...ocViews.values()].sort((a, b) => b.updatedAt - a.updatedAt)[0];
      await ocSwitchSession(first.id);
    }
    ocRenderSessionList();
  }
}

/** Serialize newOcSession so concurrent clicks cannot spawn twins. */
let ocNewLock: Promise<void> | null = null;

async function newOcSession(): Promise<void> {
  if (ocNewLock) {
    await ocNewLock;
    // after the in-flight create finishes, if a session for this folder exists — reuse it
    const ws0 = workspace.workspacePath();
    if (ws0 && ocServerOnline) {
      const ex = await oc.findSessionForWorkspace(ws0);
      if (ex) {
        await ocSwitchSession(ex.id);
        ocRenderSessionList();
        return;
      }
    }
    return;
  }
  ocNewLock = (async () => {
    try {
      const wsPath = workspace.workspacePath();
      // Reuse the session already bound to this folder — do not spawn a twin
      if (wsPath && ocServerOnline) {
        const existing = await oc.findSessionForWorkspace(wsPath);
        if (existing) {
          const v = ocEnsureView(existing.id, existing.title || `session ${existing.id.slice(0, 10)}`);
          v.onServer = true;
          v.updatedAt = existing.time?.updated ?? Date.now();
          ocAppendLine('sys', `уже есть сессия для ${wsPath} → ${existing.id} (не создаю дубликат)`);
          await ocSwitchSession(existing.id);
          ocRenderSessionList();
          void store.saveOpenTabs([...ocViews.keys()]);
          void store.saveActiveTab(existing.id);
          return;
        }
      }
      const id = await oc.createSession();
      const v = ocEnsureView(id, `session ${id.slice(0, 10)}`);
      v.onServer = true;
      v.lines = [];
      v.accum.clear();
      v.draft = '';
      v.busy = false;
      v.updatedAt = Date.now();
      const modelRef = ocModelSel.value || oc.getSavedModel();
      v.lines.push({
        role: 'sys',
        text: `session ${id} · model ${modelRef || 'server default'}${wsPath ? ` · ws ${wsPath}` : ''}`,
      });
      v.lines.push({ role: 'assistant', text: 'Готово. Напиши сообщение ниже (Enter — отправить).' });
      await ocSwitchSession(id);
      ocRenderSessionList();
      void store.saveOpenTabs([...ocViews.keys()]);
      void store.saveActiveTab(id);
    } catch (e) {
      ocAppendLine('error', `create session: ${String(e)}`);
    } finally {
      ocNewLock = null;
    }
  })();
  await ocNewLock;
}

function startOcEvents(): void {
  ocCloser?.();
  ocCloser = oc.subscribeEvents((ev) => {
    const data = oc.parseEvent(ev.data);
    if (!data) return;
    let sid: string | null = null;
    oc.applyEvent(data, {
      onSession: (s) => {
        sid = s;
      },
      onPart: (p) => {
        const target = sid ?? p.sessionID ?? ocSession;
        if (!target) return;
        const v = ocEnsureView(target);
        if (p.type === 'text' && p.text != null) {
          const key = p.messageID || p.id;
          const prev = v.accum.get(key) ?? '';
          let display: string;
          if (!prev) display = p.text;
          else if (p.text.startsWith(prev)) display = p.text;
          else if (prev.endsWith(p.text)) display = prev;
          else if (p.text.length < prev.length && prev.includes(p.text)) display = prev;
          else display = prev + p.text;
          v.accum.set(key, display);
          ocUpsertLine(target, 'assistant', display, key);
          return;
        }
        if (p.type === 'tool') {
          const title = p.state?.title ?? p.tool ?? 'tool';
          const st = p.state?.status ?? '';
          const mark = st === 'error' ? '✗' : st === 'completed' ? '✓' : st === 'running' ? '⋯' : '⚙';
          ocUpsertLine(target, 'sys', `${mark} ${title}${st ? ` [${st}]` : ''}`, `tool-${p.id}`);
          if (p.state?.output) {
            const snippet = p.state.output.length > 1200 ? p.state.output.slice(0, 1200) + '\n…' : p.state.output;
            ocUpsertLine(target, 'sys', snippet, `tool-${p.id}-out`);
          }
          if (p.state?.error) ocUpsertLine(target, 'error', String(p.state.error), `tool-${p.id}-err`);
          return;
        }
        if (p.type === 'step-start') {
          ocUpsertLine(target, 'sys', '— step —', `step-${p.id}`);
          return;
        }
        if (p.type === 'step-finish') {
          const v = ocViews.get(target);
          if (v) {
            v.lines = v.lines.filter((l) => l.id !== `step-${p.id}`);
            if (target === ocSession) ocRenderActive();
          }
          return;
        }
        if (p.type === 'reasoning' && p.text) {
          ocUpsertLine(target, 'sys', `think: ${p.text.slice(0, 400)}`, `reason-${p.id}`);
        }
      },
      onStatus: (busy) => {
        if (sid) {
          ocSetBusyFor(sid, !busy ? false : true);
          if (!busy) {
            const v = ocViews.get(sid);
            if (v) void ocCacheView(v);
          }
        } else if (!busy) {
          ocSetBusy(false);
        }
      },
      onError: (msg) => {
        const target = sid ?? ocSession;
        if (target) ocUpsertLine(target, 'error', msg);
        else ocAppendLine('error', msg);
        if (target) ocSetBusyFor(target, false);
      },
      onTokens: (t) => {
        const target = sid ?? ocSession;
        if (!target) return;
        const v = ocEnsureView(target);
        v.tokens = t;
        if (target === ocSession) ocTokEl.textContent = `${t} tok`;
      },
    });
    // background session went idle → refresh sidebar
    if (sid && sid !== ocSession) ocRenderSessionList();
  }, () => {
    /* EventSource auto-reconnects */
  });
}

$('#oc-save').addEventListener('click', () => {
  const v = ($('#oc-endpoint') as HTMLInputElement).value;
  oc.setEndpoint(v);
  ocInited = false;
  void initOpencode();
});
$('#oc-retry').addEventListener('click', () => {
  ocInited = false;
  void initOpencode();
});
$('#oc-new').addEventListener('click', () => void newOcSession());

/* workspace pick */
$('#oc-ws').addEventListener('click', async () => {
  try {
    const ws = await workspace.pickWorkspace();
    if (ws) {
      ocUpdateWsLabel();
      ocAppendLine('sys', `workspace → ${ws.path}`);
      // Reload session list scoped to the new folder only
      if (ocServerOnline) {
        try {
          const remote = await oc.listSessions();
          // drop views that belong to other folders (keep local unsaved drafts)
          const remoteIds = new Set(remote.map((s) => s.id));
          for (const id of [...ocViews.keys()]) {
            const v = ocViews.get(id)!;
            if (v.onServer && !remoteIds.has(id) && id !== ocSession) {
              // was on server but not in this workspace's list → hide from this folder
              ocViews.delete(id);
            } else if (remoteIds.has(id)) {
              v.onServer = true;
              const hit = remote.find((s) => s.id === id);
              if (hit?.title) v.title = hit.title;
            }
          }
          for (const s of remote) {
            const v = ocEnsureView(s.id, s.title || s.id.slice(0, 10));
            v.onServer = true;
            if (s.title) v.title = s.title;
            v.updatedAt = s.time?.updated ?? v.updatedAt;
          }
          // Prefer an existing session in this folder — or start a fresh one
          const first = remote
            .slice()
            .sort((a, b) => (b.time?.updated ?? 0) - (a.time?.updated ?? 0))[0];
          if (first) {
            ocAppendLine('sys', `найдена сессия папки: ${first.title || first.id}`);
            await ocSwitchSession(first.id);
          } else {
            ocAppendLine('sys', `в ${ws.path} сессий нет — создаю…`);
            await newOcSession();
          }
          ocRenderSessionList();
        } catch (e) {
          ocAppendLine('error', `list after workspace: ${String(e)}`);
        }
      }
    }
  } catch (e) {
    ocAppendLine('error', `workspace: ${String(e)}`);
  }
});

/* export / import (PC ↔ phone via browser storage) */
$('#oc-export').addEventListener('click', () => {
  const v = ocActiveView();
  if (!v) return;
  void (async () => {
    const payload = {
      format: 'nebula-oc-session',
      version: 1,
      exportedAt: Date.now(),
      session: {
        id: v.id,
        title: v.title,
        updatedAt: Date.now(),
        busy: false,
        workspace: workspace.workspacePath(),
      },
      messages: ocSerializeLines(v),
    };
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `nebula-session-${v.id.slice(0, 12)}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
    ocAppendLine('sys', `exported ${v.id} (${v.lines.length} lines)`);
  })();
});

$('#oc-import').addEventListener('click', () => ocImportFile.click());
ocImportFile.addEventListener('change', () => {
  const f = ocImportFile.files?.[0];
  ocImportFile.value = '';
  if (!f) return;
  void (async () => {
    try {
      const text = await f.text();
      const data = JSON.parse(text) as unknown;
      // full backup or single session
      if ((data as { format?: string }).format === 'nebula-oc-backup') {
        const n = await store.importAll(data);
        ocAppendLine('sys', `imported backup: ${n} sessions`);
        // reload into views
        ocViews.clear();
        ocSession = null;
        await ocRestoreFromStore();
        await refreshOc();
        return;
      }
      const meta = await store.importSession(data);
      if (!meta) throw new Error('unknown JSON format');
      const v = ocEnsureView(meta.id, meta.title);
      v.onServer = false;
      v.updatedAt = Date.now();
      const json = await store.loadMessagesJson(meta.id);
      if (json) {
        v.lines = [];
        try {
          const arr = JSON.parse(json) as {
            info?: { role?: string };
            parts?: { text?: string; id?: string; messageID?: string }[];
            localRole?: string;
          }[];
          for (const env of arr) {
            const role = env.localRole ?? (env.info?.role === 'user' ? 'user' : 'assistant');
            for (const p of env.parts ?? []) {
              if (p.text) v.lines.push({ role, text: p.text, id: p.id ?? p.messageID });
            }
          }
        } catch { /* ignore */ }
      }
      await ocSwitchSession(meta.id);
      ocRenderSessionList();
      ocAppendLine('sys', `imported «${meta.title}» — история в браузере; отправка создаст новую сессию на сервере`);
    } catch (e) {
      ocAppendLine('error', `import: ${String(e)}`);
    }
  })();
});

async function ocSend(): Promise<void> {
  const text = ocInput.value.trim();
  if (!text || !ocSession) {
    if (!ocSession) await newOcSession();
    if (!ocSession) return;
  }
  const v = ocEnsureView(ocSession!);
  if (v.busy) {
    ocAppendLine('sys', 'эта сессия ещё работает — переключись на другую или подожди idle');
    return;
  }
  const sid = v.id;
  v.draft = '';
  ocInput.value = '';
  ocUpsertLine(sid, 'user', text);
  void store.saveDraft(sid, '');
  ocSetBusyFor(sid, true);

  // local-only (imported / offline cache): create a fresh server session and carry context
  let targetId = sid;
  if (!v.onServer || !ocServerOnline) {
    if (!ocServerOnline) {
      ocUpsertLine(sid, 'error', 'сервер opencode офлайн — сообщение сохранено в браузере, отправь после подключения');
      v.draft = text;
      ocInput.value = text;
      ocSetBusyFor(sid, false);
      void store.saveDraft(sid, text);
      return;
    }
    try {
      targetId = await oc.createSession();
      const nv = ocEnsureView(targetId, v.title);
      nv.onServer = true;
      nv.lines = [];
      nv.accum = new Map();
      // carry last context so the agent can continue
      const hist = v.lines
        .slice(-40)
        .map((l) => `${l.role === 'user' ? 'User' : 'Assistant'}: ${l.text}`)
        .join('\n')
        .slice(-6000);
      const preamble = hist
        ? `Продолжаем работу из импортированной сессии. Контекст:\n---\n${hist}\n---\n\n`
        : '';
      ocViews.set(targetId, nv);
      // switch UI to the live server session
      await ocSwitchSession(targetId);
      ocRenderSessionList();
      await oc.promptAsync({
        sessionID: targetId,
        parts: [{ type: 'text', text: preamble + text }],
      });
      startOcEvents();
      window.setTimeout(() => {
        if (ocViews.get(targetId)?.busy) ocSetBusyFor(targetId, false);
      }, 120_000);
      return;
    } catch (e) {
      ocUpsertLine(sid, 'error', String(e));
      ocSetBusyFor(sid, false);
      return;
    }
  }

  try {
    await oc.promptAsync({
      sessionID: targetId,
      parts: [{ type: 'text', text }],
    });
    startOcEvents();
    window.setTimeout(() => {
      if (ocViews.get(targetId)?.busy) ocSetBusyFor(targetId, false);
    }, 120_000);
    const active = ocActiveView();
    if (active) void ocCacheView(active);
  } catch (e) {
    ocUpsertLine(sid, 'error', String(e));
    ocSetBusyFor(sid, false);
  }
}

ocSendBtn.addEventListener('click', () => void ocSend());
ocInput.addEventListener('input', () => {
  const v = ocActiveView();
  if (!v) return;
  v.draft = ocInput.value;
  window.clearTimeout(ocDraftTimer);
  ocDraftTimer = window.setTimeout(() => {
    void store.saveDraft(v.id, v.draft);
  }, 400);
});
ocInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    void ocSend();
  }
  if (e.key === 'l' && (e.ctrlKey || e.metaKey)) {
    e.preventDefault();
    const v = ocActiveView();
    if (v) {
      v.lines = [];
      v.accum.clear();
      ocRenderActive();
      ocAppendLine('sys', 'cleared (Ctrl+L)');
    } else {
      ocLog.innerHTML = '';
    }
  }
});

/* ---------- boot ---------- */
void (async () => {
  log('[boot] Nebula Emu', 'ok');
  await workspace.initWorkspace();
  if (!wasm64Supported()) {
    $('#emu-badge').textContent = 'no memory64';
    $('#emu-badge').className = 'badge off';
  } else {
    // lazy: load wasm on first run to keep first paint fast
    $('#emu-badge').textContent = 'wasm idle';
    $('#emu-badge').className = 'badge';
  }

  // Android: start the REAL opencode binary bundled in the APK (no PC, no Termux).
  if (isNative()) {
    log('[opencode] native — starting in-app serve…', 'sys');
    $('#oc-badge').textContent = 'oc starting…';
    $('#oc-badge').className = 'badge warn';
    const r = await startOnDeviceOpencode();
    if (r.ok) {
      log(`[opencode] in-app serve ready at ${window.__NEBULA_OC_BASE ?? '127.0.0.1:4096'}`, 'ok');
      $('#oc-badge').textContent = 'oc on-device';
      $('#oc-badge').className = 'badge live';
    } else {
      logErr(`[opencode] ${r.error ?? 'start failed'}`);
      $('#oc-badge').textContent = 'oc error';
      $('#oc-badge').className = 'badge off';
    }
    // listen for plugin readiness events
    try {
      await NebulaEmu.addListener('opencodeReady', (e) => {
        if (e.ok && e.base) {
          window.__NEBULA_OC_BASE = e.base.replace(/\/$/, '');
          $('#oc-badge').textContent = 'oc on-device';
          $('#oc-badge').className = 'badge live';
        }
      });
    } catch { /* optional */ }
  } else {
    // probe bridge in background (desktop)
    void oc.probe().then((info) => {
      if (info.ok) {
        $('#oc-badge').textContent = `oc ${info.opencode ?? ''}`;
        $('#oc-badge').className = 'badge live';
      } else {
        $('#oc-badge').textContent = 'oc offline';
        $('#oc-badge').className = 'badge off';
      }
    });
  }
})();
