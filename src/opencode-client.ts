/**
 * Real opencode client — talks to `opencode serve` HTTP API only.
 *
 * Hosts:
 *  - Desktop: Vite proxy /api/opencode → Bun bridge (opencode/bridge.ts) → local `opencode serve`
 *  - Android APK: native plugin starts the bundled binary → http://127.0.0.1:4096 (same WebView)
 *  - Explicit URL override for advanced setups
 *
 * No re-implemented agent loop: sessions, prompt_async and SSE /event are the real opencode protocol.
 */

export interface BridgeInfo {
  ok: boolean;
  opencode?: string;
  model?: string;
  workspace?: string;
  source?: 'proxy' | 'loopback' | 'custom';
  error?: string;
}

export interface OcPart {
  id: string;
  messageID: string;
  sessionID?: string;
  type: string;
  text?: string;
  tool?: string;
  callID?: string;
  state?: {
    status?: string;
    title?: string;
    input?: Record<string, unknown>;
    output?: string;
    error?: string;
  };
  reason?: string;
  tokens?: { total?: number; input?: number; output?: number; reasoning?: number };
}

export interface OcMessage {
  id: string;
  role: 'user' | 'assistant';
  parts: OcPart[];
  modelID?: string;
  error?: unknown;
}

type Listener = (e: MessageEvent) => void;

const LS_KEY = 'nebula.oc.endpoint';

/** Android in-app server (set by the Capacitor plugin after serve is healthy). */
declare global {
  interface Window {
    __NEBULA_OC_BASE?: string;
    Capacitor?: { isNativePlatform?: () => boolean };
  }
}

export function isNativeAndroid(): boolean {
  try {
    return window.Capacitor?.isNativePlatform?.() === true;
  } catch {
    return false;
  }
}

export function getEndpoint(): string {
  const custom = localStorage.getItem(LS_KEY);
  if (custom) return custom.replace(/\/$/, '');
  if (window.__NEBULA_OC_BASE) return window.__NEBULA_OC_BASE.replace(/\/$/, '');
  // desktop / vite dev → same-origin proxy
  return '';
}

export function setEndpoint(url: string): void {
  const v = url.trim().replace(/\/$/, '');
  if (v) localStorage.setItem(LS_KEY, v);
  else localStorage.removeItem(LS_KEY);
}

export function clearEndpoint(): void {
  localStorage.removeItem(LS_KEY);
}

function api(path: string): string {
  const base = getEndpoint();
  if (base) return base + path;
  return '/api/opencode' + path;
}

/** Probe bridge or loopback server. */
export async function probe(timeoutMs = 2500): Promise<BridgeInfo> {
  const ac = new AbortController();
  const t = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const r = await fetch(api('/bridge'), { signal: ac.signal });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const j = (await r.json()) as BridgeInfo;
    const source: BridgeInfo['source'] = getEndpoint() ? (window.__NEBULA_OC_BASE && getEndpoint() === window.__NEBULA_OC_BASE.replace(/\/$/, '') ? 'loopback' : 'custom') : 'proxy';
    return { ...j, ok: true, source };
  } catch (e) {
    // Direct loopback fallback (Android without proxy)
    if (!getEndpoint()) {
      try {
        const r2 = await fetch('http://127.0.0.1:4096/global/health', {
          signal: AbortSignal.timeout(1500),
        });
        if (r2.ok) {
          const j = (await r2.json()) as { healthy?: boolean; version?: string };
          if (j.healthy !== false) {
            window.__NEBULA_OC_BASE = 'http://127.0.0.1:4096';
            return { ok: true, opencode: j.version, source: 'loopback', model: 'opencode/big-pickle' };
          }
        }
      } catch { /* fallthrough */ }
    }
    return { ok: false, error: e instanceof Error ? e.message : String(e) };
  } finally {
    clearTimeout(t);
  }
}

export async function healthRaw(): Promise<Response> {
  return fetch(api('/global/health'));
}

export async function createSession(title = 'Nebula Emu'): Promise<string> {
  const dir = workspaceDir();
  // body.directory is ignored by opencode — stamp via header + query (query is authoritative)
  const q = dir ? `?directory=${encodeURIComponent(dir)}` : '';
  const r = await fetch(api('/session' + q), {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-opencode-directory': dir },
    body: JSON.stringify({ title }),
  });
  if (!r.ok) throw new Error(`session create: HTTP ${r.status}`);
  const j = (await r.json()) as { id: string; directory?: string };
  return j.id;
}

export async function listSessions(): Promise<
  { id: string; title?: string; time?: { updated?: number; created?: number }; directory?: string }[]
> {
  const dir = workspaceDir();
  // opencode filters GET /session only by ?directory= (header is ignored on list)
  const q = dir ? `?directory=${encodeURIComponent(dir)}` : '';
  const r = await fetch(api('/session' + q), {
    headers: { 'x-opencode-directory': dir },
  });
  if (!r.ok) throw new Error(`session list: HTTP ${r.status}`);
  const list = (await r.json()) as {
    id: string;
    title?: string;
    time?: { updated?: number; created?: number };
    directory?: string;
  }[];
  // belt-and-suspenders: drop rows from other folders when a workspace is active
  if (dir) {
    const norm = dir.replace(/[\\/]+$/, '').toLowerCase();
    return list.filter((s) => {
      if (!s.directory) return true; // older rows without stamp — keep, create path always stamps
      const sd = s.directory.replace(/[\\/]+$/, '').toLowerCase();
      return sd === norm || sd.startsWith(norm + '\\') || sd.startsWith(norm + '/');
    });
  }
  return list;
}

/**
 * Find an existing server session for this workspace (so "new/run" does not
 * spawn a second chat in the same folder and confuse the list).
 * Queries the server with the explicit directory (not just the window global).
 */
export async function findSessionForWorkspace(
  dir: string,
): Promise<{ id: string; title?: string; time?: { updated?: number } } | null> {
  if (!dir) return null;
  try {
    const q = `?directory=${encodeURIComponent(dir)}`;
    const r = await fetch(api('/session' + q), {
      headers: { 'x-opencode-directory': dir },
    });
    if (!r.ok) return null;
    const list = (await r.json()) as {
      id: string;
      title?: string;
      time?: { updated?: number };
      directory?: string;
    }[];
    const norm = dir.replace(/[\\/]+$/, '').toLowerCase();
    const hits = list
      .filter((s) => {
        if (!s.directory) return false;
        const sd = s.directory.replace(/[\\/]+$/, '').toLowerCase();
        return sd === norm;
      })
      .sort((a, b) => (b.time?.updated ?? 0) - (a.time?.updated ?? 0));
    return hits[0] ?? null;
  } catch {
    return null;
  }
}

export async function deleteSession(id: string): Promise<void> {
  const r = await fetch(api(`/session/${encodeURIComponent(id)}`), {
    method: 'DELETE',
    headers: { 'x-opencode-directory': workspaceDir() },
  });
  if (!r.ok && r.status !== 404) throw new Error(`session delete: HTTP ${r.status}`);
}

export async function renameSession(id: string, title: string): Promise<void> {
  const r = await fetch(api(`/session/${encodeURIComponent(id)}`), {
    method: 'PATCH',
    headers: {
      'content-type': 'application/json',
      'x-opencode-directory': workspaceDir(),
    },
    body: JSON.stringify({ title }),
  });
  if (!r.ok) throw new Error(`session rename: HTTP ${r.status}`);
}

export async function getSession(id: string): Promise<{ id: string; messages?: OcMessage[] }> {
  // GET /session/:id returns metadata only; history lives at /session/:id/message
  const r = await fetch(api(`/session/${encodeURIComponent(id)}`), {
    headers: { 'x-opencode-directory': workspaceDir() },
  });
  if (!r.ok) throw new Error(`session get: HTTP ${r.status}`);
  return (await r.json()) as { id: string; messages?: OcMessage[] };
}

/** One chat message as returned by GET /session/:id/message */
export interface OcMessageEnvelope {
  info: {
    id: string;
    sessionID?: string;
    role: 'user' | 'assistant' | string;
    modelID?: string;
    model?: { providerID?: string; modelID?: string };
    error?: unknown;
    time?: { created?: number; completed?: number };
  };
  parts: OcPart[];
}

/** Full chat history (user + assistant turns). */
export async function listMessages(sessionID: string): Promise<OcMessageEnvelope[]> {
  const r = await fetch(api(`/session/${encodeURIComponent(sessionID)}/message`), {
    headers: { 'x-opencode-directory': workspaceDir() },
  });
  if (!r.ok) throw new Error(`messages: HTTP ${r.status}`);
  const j = (await r.json()) as OcMessageEnvelope[] | { info?: unknown };
  if (Array.isArray(j)) return j;
  return [];
}

export interface PromptOptions {
  sessionID: string;
  parts: { type: 'text'; text: string }[];
  model?: { providerID?: string; modelID?: string };
  agent?: string;
}

const LS_MODEL = 'nebula.oc.model';

/** Saved model as `providerID/modelID` (empty = use server default). */
export function getSavedModel(): string {
  try {
    return localStorage.getItem(LS_MODEL) ?? '';
  } catch {
    return '';
  }
}

export function setSavedModel(id: string): void {
  try {
    if (id) localStorage.setItem(LS_MODEL, id);
    else localStorage.removeItem(LS_MODEL);
  } catch {
    /* ignore */
  }
}

export function parseModelRef(ref: string): { providerID: string; modelID: string } | null {
  const v = ref.trim();
  if (!v) return null;
  const i = v.indexOf('/');
  if (i <= 0 || i === v.length - 1) return null;
  return { providerID: v.slice(0, i), modelID: v.slice(i + 1) };
}

export interface ModelChoice {
  ref: string; // providerID/modelID  (modelID may itself contain `/`, e.g. openrouter/nvidia/foo:free)
  providerID: string;
  modelID: string;
  providerName: string;
  name: string;
  context?: number;
  output?: number;
  status?: string;
  /** true when free by naming (…-free / :free / free) or zero cost */
  free: boolean;
}

/** Heuristic: model looks free (id/name marker or zero price). */
function looksFree(providerID: string, modelID: string, name: string, cost?: { input?: number; output?: number }): boolean {
  const blob = `${providerID}/${modelID} ${name}`.toLowerCase();
  if (/[:\-/_]free\b|:free$|\bfree\b/.test(blob)) return true;
  if (providerID === 'opencode' && /free/.test(modelID)) return true;
  if (providerID === 'pollinations') return true;
  if (cost && cost.input === 0 && cost.output === 0) return true;
  return false;
}

/**
 * Flatten GET /config/providers → pickable models (grouped later by provider).
 * API keys are intentionally stripped — never sent to the UI.
 */
export async function listModels(): Promise<ModelChoice[]> {
  const r = await fetch(api('/config/providers'), {
    headers: { 'x-opencode-directory': workspaceDir() },
  });
  if (!r.ok) throw new Error(`providers: HTTP ${r.status}`);
  const j = (await r.json()) as {
    providers?: {
      id: string;
      name?: string;
      models?: Record<
        string,
        {
          id?: string;
          name?: string;
          status?: string;
          cost?: { input?: number; output?: number };
          limit?: { context?: number; output?: number };
        }
      >;
    }[];
  };
  const out: ModelChoice[] = [];
  for (const p of j.providers ?? []) {
    for (const [mid, m] of Object.entries(p.models ?? {})) {
      const id = m.id ?? mid;
      out.push({
        ref: `${p.id}/${id}`,
        providerID: p.id,
        modelID: id,
        providerName: p.name ?? p.id,
        name: m.name ?? id,
        context: m.limit?.context,
        output: m.limit?.output,
        status: m.status,
        free: looksFree(p.id, id, m.name ?? id, m.cost),
      });
    }
  }
  // free first, then active, then name
  out.sort((a, b) => {
    if (a.free !== b.free) return a.free ? -1 : 1;
    const as = a.status === 'active' ? 0 : 1;
    const bs = b.status === 'active' ? 0 : 1;
    if (as !== bs) return as - bs;
    return a.name.localeCompare(b.name);
  });
  return out;
}

/** Merge static Pollinations fallback if the server has not reloaded config yet. */
export function withPollinationsFallback(list: ModelChoice[]): ModelChoice[] {
  if (list.some((m) => m.providerID === 'pollinations')) return list;
  const extras: ModelChoice[] = [
    {
      ref: 'pollinations/openai-fast',
      providerID: 'pollinations',
      modelID: 'openai-fast',
      providerName: 'Pollinations',
      name: 'GPT-OSS 20B (free fallback)',
      context: 128000,
      output: 8000,
      status: 'active',
      free: true,
    },
    {
      ref: 'pollinations/openai',
      providerID: 'pollinations',
      modelID: 'openai',
      providerName: 'Pollinations',
      name: 'openai alias (free fallback)',
      context: 128000,
      output: 8000,
      status: 'active',
      free: true,
    },
  ];
  return [...extras, ...list];
}

/** Fire-and-forget async prompt; events arrive on /event SSE. */
export async function promptAsync(opts: PromptOptions): Promise<void> {
  const saved = parseModelRef(getSavedModel());
  const model = opts.model ?? saved ?? { providerID: 'opencode', modelID: 'mimo-v2.6-flash-free' };
  const body = {
    model,
    parts: opts.parts,
    agent: opts.agent,
  };
  const r = await fetch(api(`/session/${encodeURIComponent(opts.sessionID)}/prompt_async`), {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'x-opencode-directory': workspaceDir(),
    },
    body: JSON.stringify(body),
  });
  if (!r.ok) {
    const text = await r.text().catch(() => '');
    throw new Error(`prompt: HTTP ${r.status} ${text.slice(0, 200)}`);
  }
}

export function workspaceDir(): string {
  // Prefer the user-picked workspace (phone/PC/browser) over the plugin default.
  const picked = (window as unknown as { __NEBULA_OC_WORKSPACE?: string }).__NEBULA_OC_WORKSPACE;
  if (picked) return picked;
  return (window as unknown as { __NEBULA_OC_DEFAULT_WS?: string }).__NEBULA_OC_DEFAULT_WS ?? '';
}

/** Subscribe to the real opencode SSE event bus. Returns a closer. */
export function subscribeEvents(onMessage: Listener, onError?: (e: Event) => void): () => void {
  const url = api('/event');
  const es = new EventSource(url);
  es.onmessage = onMessage;
  es.onerror = onError ?? (() => undefined);
  return () => es.close();
}

export interface StreamEvent {
  type: string;
  [key: string]: unknown;
}

export function parseEvent(data: string): StreamEvent | null {
  try {
    return JSON.parse(data) as StreamEvent;
  } catch {
    return null;
  }
}

/** Extract sessionID from any SSE payload shape. */
export function eventSessionID(ev: StreamEvent): string | null {
  const direct = ev.sessionID ?? ev.sessionId;
  if (typeof direct === 'string' && direct) return direct;
  const bag = (ev.properties ?? {}) as {
    sessionID?: string;
    sessionId?: string;
    session?: { id?: string } | string;
    part?: { sessionID?: string };
    message?: { sessionID?: string; info?: { sessionID?: string } };
    info?: { sessionID?: string };
  };
  if (typeof bag.sessionID === 'string' && bag.sessionID) return bag.sessionID;
  if (typeof bag.sessionId === 'string' && bag.sessionId) return bag.sessionId;
  if (typeof bag.session === 'string' && bag.session) return bag.session;
  if (bag.session && typeof bag.session === 'object' && bag.session.id) return bag.session.id;
  if (bag.part?.sessionID) return bag.part.sessionID;
  if (bag.message?.sessionID) return bag.message.sessionID;
  if (bag.message?.info?.sessionID) return bag.message.info.sessionID;
  if (bag.info?.sessionID) return bag.info.sessionID;
  return null;
}

/** Map SSE payload → incremental UI update. */
export function applyEvent(ev: StreamEvent, sink: {
  onText?: (messageID: string, text: string) => void;
  onPart?: (part: OcPart) => void;
  onStatus?: (busy: boolean) => void;
  onError?: (msg: string) => void;
  onTokens?: (total: number) => void;
  /** sessionID owning this event (null = unknown / global) */
  onSession?: (sessionID: string | null) => void;
}): void {
  const t = ev.type as string;
  const sid = eventSessionID(ev);
  sink.onSession?.(sid);
  if (t === 'message.part.updated' || t === 'message.part') {
    const bag = (ev.properties ?? {}) as { part?: unknown };
    const part = (ev.part ?? bag.part) as OcPart | undefined;
    if (part && sink.onPart) sink.onPart(part);
    if (part?.type === 'text' && part.text && sink.onText) sink.onText(part.messageID, part.text);
    if (part?.tokens?.total && sink.onTokens) sink.onTokens(part.tokens.total);
    return;
  }
  if (t === 'message.updated' || t === 'message') {
    const bag = (ev.properties ?? {}) as { message?: unknown };
    const msg = (ev.message ?? bag.message) as OcMessage | undefined;
    if (msg?.error) {
      const err = typeof msg.error === 'string' ? msg.error : JSON.stringify(msg.error);
      sink.onError?.(err);
      sink.onStatus?.(false);
    }
    return;
  }
  if (t === 'session.idle' || t === 'session.deleted' || t === 'session.updated' || t === 'session.created') {
    if (t !== 'session.updated' && t !== 'session.created') sink.onStatus?.(false);
    return;
  }
  if (t === 'server.connected') return;
  if (t === 'error') {
    sink.onError?.(String(ev.error ?? ev.message ?? 'unknown error'));
    sink.onStatus?.(false);
  }
  // step-start / step-finish / tool.* — already covered via message.part
}
