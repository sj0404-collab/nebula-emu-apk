#!/usr/bin/env bun
/**
 * opencode bridge — spawns the REAL `opencode serve` binary and proxies it to the browser.
 *
 *   browser ──/api/opencode/*──▶ bridge.ts (:8787) ──▶ opencode serve (:4096) ──▶ model provider
 *
 * Desktop only. On Android the APK embeds the same binary and serves on 127.0.0.1:4096
 * without any bridge (see android/.../OpencodeNativePlugin.java).
 *
 * Usage:
 *   bun run opencode/bridge.ts
 *   BRIDGE_PORT=8787 OPENCODE_PORT=4096 OPENCODE_BIN=/path/to/opencode OPENCODE_WORKSPACE=<dir>
 *
 * Single file: bun build --compile opencode/bridge.ts --outfile release/opencode-bridge
 */
import { existsSync, statSync } from 'node:fs';
import { homedir, platform } from 'node:os';
import { join, resolve, extname, dirname, sep } from 'node:path';

const IS_WIN = platform() === 'win32';

const BRIDGE_PORT = Number(process.env.BRIDGE_PORT ?? 8787);
const OC_PORT = Number(process.env.OPENCODE_PORT ?? 4096);
// Default model reported by /bridge (UI can override per-message via prompt body).
// Override: OPENCODE_MODEL=provider/model  e.g. OPENCODE_MODEL=opencode/mimo-v2.5-free
const MODEL = process.env.OPENCODE_MODEL ?? 'opencode/mimo-v2.5-free';

const COMPILED = import.meta.dir.includes('$bunfs') || /[\\/]~BUN[\\/]/.test(import.meta.dir);
function detectRoot(): string {
  if (!COMPILED) return resolve(import.meta.dir, '..');
  for (const c of [process.cwd(), dirname(process.execPath)]) if (existsSync(join(c, 'dist'))) return c;
  return process.cwd();
}
const ROOT = detectRoot();
const WORKSPACE = resolve(process.env.OPENCODE_WORKSPACE ?? ROOT);
const DIST = join(ROOT, 'dist');
const OC_URL = `http://127.0.0.1:${OC_PORT}`;
const API_PREFIX = '/api/opencode';

function cmdFor(bin: string): string[] {
  return IS_WIN && /\.(cmd|bat)$/i.test(bin) ? ['cmd.exe', '/d', '/s', '/c', bin] : [bin];
}

/** Find the real opencode the user already has installed — no reimplementation. */
function findOpencode(): string[] {
  if (process.env.OPENCODE_BIN) return cmdFor(process.env.OPENCODE_BIN);
  const inPath = Bun.which('opencode');
  if (inPath) return cmdFor(inPath);
  const exts = IS_WIN ? ['.exe', '.cmd', ''] : [''];
  const dirs = [
    join(homedir(), '.bun', 'bin'),
    join(ROOT, 'node_modules', '.bin'),
    ...(IS_WIN && process.env.APPDATA ? [join(process.env.APPDATA, 'npm')] : []),
    '/usr/local/bin',
    '/opt/homebrew/bin',
  ];
  for (const d of dirs) for (const e of exts) {
    const f = join(d, 'opencode' + e);
    if (existsSync(f)) return cmdFor(f);
  }
  const bunx = Bun.which('bunx') ?? (IS_WIN ? 'bunx.exe' : 'bunx');
  console.warn('[bridge] opencode not found — falling back to `bunx opencode-ai@latest`');
  return [bunx, 'opencode-ai@latest'];
}

async function health(): Promise<{ ok: boolean; version?: string }> {
  try {
    const r = await fetch(`${OC_URL}/global/health`, { signal: AbortSignal.timeout(1500) });
    if (!r.ok) return { ok: false };
    const j = (await r.json()) as { healthy?: boolean; version?: string };
    return { ok: j.healthy !== false, version: j.version };
  } catch {
    return { ok: false };
  }
}

let child: Bun.Subprocess | null = null;
const killChild = () => {
  try {
    child?.kill();
  } catch {
    /* ignore */
  }
};
let ocVersion = 'unknown';

async function ensureOpencode(): Promise<void> {
  const h = await health();
  if (h.ok) {
    ocVersion = h.version ?? ocVersion;
    console.log(`[bridge] reusing opencode server at ${OC_URL} (v${ocVersion})`);
    return;
  }
  const cmd = findOpencode();
  console.log(`[bridge] starting: ${cmd.join(' ')} serve --port ${OC_PORT} --hostname 127.0.0.1  (cwd ${WORKSPACE})`);
  child = Bun.spawn([...cmd, 'serve', '--port', String(OC_PORT), '--hostname', '127.0.0.1'], {
    cwd: WORKSPACE,
    stdout: 'inherit',
    stderr: 'inherit',
    env: { ...process.env, OPENCODE_SERVER_PASSWORD: process.env.OPENCODE_SERVER_PASSWORD ?? '' },
  });
  const deadline = Date.now() + 120_000;
  while (Date.now() < deadline) {
    await Bun.sleep(500);
    const hh = await health();
    if (hh.ok) {
      ocVersion = hh.version ?? ocVersion;
      console.log(`[bridge] opencode v${ocVersion} ready at ${OC_URL}`);
      return;
    }
    if (child.exitCode !== null) {
      throw new Error(`opencode exited with code ${child.exitCode} — install: npm i -g opencode-ai`);
    }
  }
  throw new Error('opencode serve did not become healthy within 120 s');
}

const CORS: Record<string, string> = {
  'access-control-allow-origin': '*',
  'access-control-allow-methods': 'GET,POST,PUT,PATCH,DELETE,OPTIONS',
  'access-control-allow-headers':
    'content-type,accept,authorization,x-opencode-directory,x-opencode-client,x-opencode-project,x-opencode-request,x-opencode-session,x-opencode-session-id',
};

async function proxy(req: Request, url: URL): Promise<Response> {
  // Preserve client query (e.g. ?directory= for session list scoping)
  const target = new URL(OC_URL + url.pathname.slice(API_PREFIX.length) + url.search);
  const headers = new Headers();
  for (const h of ['content-type', 'accept', 'last-event-id', 'x-opencode-directory', 'x-opencode-workspace']) {
    const v = req.headers.get(h);
    if (v) headers.set(h, v);
  }
  // client-chosen workspace wins; otherwise the bridge default
  if (!headers.get('x-opencode-directory')) {
    headers.set('x-opencode-directory', WORKSPACE);
  }
  // Ensure list/create are scoped when client sent directory but forgot query
  const clientDir = req.headers.get('x-opencode-directory');
  if (clientDir && !target.searchParams.has('directory') && /\/session\/?$/.test(url.pathname)) {
    target.searchParams.set('directory', clientDir);
  }
  const init: RequestInit = { method: req.method, headers, redirect: 'manual' };
  if (req.method !== 'GET' && req.method !== 'HEAD') init.body = await req.arrayBuffer();
  let up: Response;
  try {
    up = await fetch(target, init);
  } catch (e) {
    return Response.json({ error: 'opencode server unreachable', detail: String(e) }, { status: 502, headers: CORS });
  }
  const out = new Headers(CORS);
  for (const h of ['content-type', 'cache-control', 'content-disposition']) {
    const v = up.headers.get(h);
    if (v) out.set(h, v);
  }
  if (up.headers.get('content-type')?.includes('text/event-stream')) {
    out.set('cache-control', 'no-cache');
    out.set('x-accel-buffering', 'no');
  }
  return new Response(up.body, { status: up.status, headers: out });
}

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.map': 'application/json',
  '.txt': 'text/plain; charset=utf-8',
  '.exe': 'application/octet-stream',
};

function serveStatic(pathname: string): Response {
  if (!existsSync(DIST)) {
    return new Response(
      `Nebula Emu bridge.\nAPI: ${API_PREFIX}/* → ${OC_URL}\nRun \`npm run build\` to serve dist/ from here.\n`,
      { headers: { 'content-type': 'text/plain; charset=utf-8' } },
    );
  }
  let decoded: string;
  try {
    decoded = decodeURIComponent(pathname);
  } catch {
    return new Response('bad request', { status: 400 });
  }
  let file = resolve(DIST, '.' + decoded);
  if (file !== DIST && !file.startsWith(DIST + sep)) return new Response('forbidden', { status: 403 });
  if (!existsSync(file) || statSync(file).isDirectory()) file = join(DIST, 'index.html');
  const type = MIME[extname(file).toLowerCase()] ?? 'application/octet-stream';
  return new Response(Bun.file(file), {
    headers: {
      'content-type': type,
      'cache-control': /index\.html$/.test(file) ? 'no-cache' : 'public, max-age=3600',
    },
  });
}

try {
  await ensureOpencode();
} catch (e) {
  console.error(`[bridge] ${e instanceof Error ? e.message : e}`);
  process.exit(1);
}

let server: ReturnType<typeof Bun.serve>;
try {
  server = Bun.serve({
    port: BRIDGE_PORT,
    hostname: '0.0.0.0',
    idleTimeout: 255,
    async fetch(req) {
      const url = new URL(req.url);
      if (req.method === 'OPTIONS') return new Response(null, { status: 204, headers: CORS });
      if (url.pathname === `${API_PREFIX}/bridge`) {
        const h = await health();
        return Response.json(
          {
            ok: h.ok,
            opencode: h.version ?? ocVersion,
            model: MODEL,
            workspace: WORKSPACE,
            server: OC_URL,
            bun: Bun.version,
            source: 'proxy',
          },
          { headers: CORS },
        );
      }
      if (url.pathname.startsWith(API_PREFIX + '/')) return proxy(req, url);
      return serveStatic(url.pathname);
    },
  });
} catch (e) {
  console.error(
    `[bridge] cannot listen on port ${BRIDGE_PORT}: ${e instanceof Error ? e.message : e}\n         set BRIDGE_PORT=<free port>`,
  );
  killChild();
  process.exit(1);
}

console.log(
  `[bridge] Bun ${Bun.version} · http://localhost:${server.port}  (model ${MODEL}, workspace ${WORKSPACE})`,
);

const shutdown = () => {
  console.log('\n[bridge] shutting down');
  killChild();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
if (IS_WIN) process.on('SIGBREAK', shutdown);
process.on('exit', killChild);
