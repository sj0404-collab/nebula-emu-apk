/**
 * E2E: browser workspace manager
 * 1) create a real folder on disk
 * 2) open UI, pick that folder via mocked File System Access + path prompt
 * 3) press "new" (запустить) → session must be created IN that folder
 * 4) press again → must REUSE existing session (no twin / no confusion)
 * 5) listSessions must only show sessions for that workspace
 */
import { chromium } from 'playwright-core';
import { mkdirSync, existsSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';

const ROOT = 'D:/Manga/nebula-emu';
const WS = resolve(ROOT, 'test-ws-browser');
const BASE = 'http://127.0.0.1:5173';
const OC = 'http://127.0.0.1:8787/api/opencode';

function log(step, msg) {
  console.log(`[${step}] ${msg}`);
}

function fail(msg) {
  console.error(`FAIL: ${msg}`);
  process.exitCode = 1;
}

async function api(path, init) {
  const r = await fetch(OC + path, init);
  if (!r.ok) throw new Error(`${init?.method ?? 'GET'} ${path} → ${r.status}`);
  const t = await r.text();
  try {
    return t ? JSON.parse(t) : null;
  } catch {
    return t;
  }
}

async function main() {
  // --- prepare real folder ---
  if (existsSync(WS)) rmSync(WS, { recursive: true, force: true });
  mkdirSync(WS, { recursive: true });
  log('setup', `created folder ${WS}`);

  // clean any leftover sessions stamped to this folder (via query list)
  const q = `?directory=${encodeURIComponent(WS)}`;
  try {
    const before = await api('/session' + q);
    for (const s of before ?? []) {
      await api(`/session/${s.id}`, { method: 'DELETE' });
      log('setup', `removed old session ${s.id}`);
    }
  } catch (e) {
    log('setup', `list/cleanup skip: ${e.message}`);
  }

  const browser = await chromium.launch({
    headless: true,
    executablePath: undefined, // use playwright chromium
    args: ['--no-sandbox', '--disable-dev-shm-usage'],
  });
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  page.on('console', (m) => {
    if (m.type() === 'error') console.log('  browser-error:', m.text());
  });
  page.on('pageerror', (e) => console.log('  pageerror:', e.message));

  await page.goto(BASE + '/', { waitUntil: 'networkidle', timeout: 30000 });
  log('ui', 'page loaded');

  // go to opencode tab
  await page.click('.tabs button[data-tab="oc"]');
  await page.waitForSelector('#pane-oc.active', { timeout: 10000 });
  log('ui', 'opencode pane open');

  // wait until bridge live badge or offline
  await page.waitForFunction(
    () => {
      const el = document.querySelector('#oc-state');
      return el && /live|offline/.test(el.textContent || '');
    },
    { timeout: 15000 },
  );
  const state = (await page.textContent('#oc-state')) || '';
  log('ui', `oc-state=${state.trim()}`);
  if (!/live/.test(state)) {
    fail(`bridge not live in UI: ${state}`);
    await browser.close();
    return;
  }

  // Mock File System Access API + path prompt BEFORE clicking 📁
  await page.evaluate(
    ({ wsPath, folderName }) => {
      const handle = {
        kind: 'directory',
        name: folderName,
        queryPermission: async () => 'granted',
        requestPermission: async () => 'granted',
      };
      window.showDirectoryPicker = async () => handle;
      window.prompt = (msg) => {
        const m = String(msg);
        if (m.includes('Абсолютный путь') || m.includes('opencode') || m.includes('папку')) {
          return wsPath;
        }
        return null;
      };
      window.confirm = () => true;
      window.alert = (m) => {
        console.error('ALERT', m);
      };
    },
    { wsPath: WS, folderName: 'test-ws-browser' },
  );
  log('picker', 'showDirectoryPicker + prompt mocked');

  // click via evaluate (more reliable than playwright hit-testing on compact bar)
  await page.evaluate(() => {
    document.querySelector('#oc-ws')?.click();
  });

  try {
    await page.waitForFunction(
      () => {
        const el = document.querySelector('#oc-ws-path');
        return !!el && (el.textContent || '').includes('test-ws-browser');
      },
      undefined,
      { timeout: 15000 },
    );
  } catch {
    const dbg = await page.evaluate(() => ({
      label: document.querySelector('#oc-ws-path')?.textContent,
      win: window.__NEBULA_OC_WORKSPACE,
      log: (document.querySelector('#oc-log')?.textContent || '').slice(-500),
      state: document.querySelector('#oc-state')?.textContent,
      btn: !!document.querySelector('#oc-ws'),
    }));
    fail(`workspace label not updated: ${JSON.stringify(dbg)}`);
    await browser.close();
    return;
  }
  const wsLabel = (await page.textContent('#oc-ws-path')) || '';
  log('picker', `workspace label: ${wsLabel.trim()}`);

  // window.__NEBULA_OC_WORKSPACE must be set
  const winWs = await page.evaluate(() => window.__NEBULA_OC_WORKSPACE);
  log('picker', `window.__NEBULA_OC_WORKSPACE=${winWs}`);
  if (winWs !== WS) fail(`workspace window var mismatch: ${winWs} !== ${WS}`);

  // After pick, UI should open-or-create session in this folder.
  // Wait until a real session id appears (10+ chars) or timeout
  try {
    await page.waitForFunction(
      () => {
        const el = document.querySelector('#oc-session');
        const t = (el && el.textContent) || '';
        return t.length >= 10 && t !== '—';
      },
      undefined,
      { timeout: 20000 },
    );
  } catch {
    log('session', 'no auto session → click + new');
    await page.evaluate(() => document.querySelector('#oc-new')?.click());
    await page.waitForFunction(
      () => {
        const t = (document.querySelector('#oc-session')?.textContent) || '';
        return t.length >= 10 && t !== '—';
      },
      undefined,
      { timeout: 15000 },
    ).catch(() => {});
  }
  let sid = await page.evaluate(() => document.querySelector('#oc-session')?.textContent || '');
  log('session', `active after pick: ${sid}`);

  // Read full session id from title attr of active list item or window state via list API
  // The badge only shows first 10 chars — query server for folder sessions
  const list1 = await api('/session' + q);
  log('api', `list for workspace: ${JSON.stringify((list1 ?? []).map((s) => ({ id: s.id, dir: s.directory, title: s.title })))}`);

  if (!list1 || list1.length === 0) {
    fail('no session on server for this workspace after new/pick');
  } else {
    const s0 = list1[0];
    if (s0.directory !== WS) fail(`session directory mismatch: ${s0.directory} !== ${WS}`);
    else log('api', `session directory OK: ${s0.directory}`);

    // --- press + new again → must NOT create a twin ---
    await page.evaluate(() => document.querySelector('#oc-new')?.click());
    await page.waitForTimeout(2000);
    const list2 = await api('/session' + q);
    log('api', `list after 2nd new: count=${list2.length}`);
    if (list2.length !== 1) {
      fail(`expected 1 session in folder after re-new, got ${list2.length} (twins!)`);
    } else {
      log('api', 'no twin — reuse OK');
    }

    // session id in UI should match the server one (first 10 chars)
    const sid2 = ((await page.evaluate(() => document.querySelector('#oc-session')?.textContent)) || '').trim();
    if (sid2 && s0.id.startsWith(sid2)) log('session', `UI id matches server: ${sid2}…`);
    else log('session', `UI shows ${sid2}, server has ${s0.id} (prefix ${s0.id.slice(0, 10)})`);

    // sys line should mention reuse or existing
    const logText = (await page.textContent('#oc-log')) || '';
    if (/уже есть сессия|не создаю дубликат|найдена сессия/i.test(logText)) {
      log('session', 'UI logged reuse path');
    } else {
      log('session', 'no explicit reuse log (may have auto-switched on pick)');
    }

    // --- global list must NOT mix other folders into the sidebar while WS active ---
    // (client filters; verify via API that query works)
    const all = await api('/session');
    const other = (all ?? []).filter((s) => s.directory && s.directory !== WS);
    log('api', `global sessions=${all.length}, other-folder=${other.length} (sidebar must hide these)`);

    // switch workspace label still correct
    const wsLabel2 = (await page.textContent('#oc-ws-path')) || '';
    if (!wsLabel2.includes('test-ws-browser')) fail('workspace label lost');
  }

  // --- IndexedDB draft/session persistence smoke ---
  const idb = await page.evaluate(async () => {
    const req = indexedDB.open('nebula-emu');
    return await new Promise((res) => {
      req.onsuccess = () => {
        const db = req.result;
        res({ ok: true, stores: [...db.objectStoreNames] });
      };
      req.onerror = () => res({ ok: false, error: String(req.error) });
    });
  });
  log('idb', JSON.stringify(idb));
  // stores are: sessions, messages, drafts, meta (workspace lives under meta)
  if (!idb.ok || !idb.stores?.includes('meta')) fail('IndexedDB meta store missing');
  if (!idb.stores?.includes('sessions')) fail('IndexedDB sessions store missing');

  const storedWs = await page.evaluate(async () => {
    const req = indexedDB.open('nebula-emu');
    return await new Promise((res) => {
      req.onsuccess = () => {
        const db = req.result;
        const t = db.transaction('meta', 'readonly');
        const g = t.objectStore('meta').get('workspace');
        g.onsuccess = () => res(g.result?.value ?? null);
        g.onerror = () => res(null);
      };
      req.onerror = () => res(null);
    });
  });
  log('idb', `stored workspace=${JSON.stringify(storedWs)}`);
  if (!storedWs || storedWs.path !== WS) fail('workspace not persisted to IndexedDB');

  // cleanup server session for this folder
  try {
    const list = await api('/session' + q);
    for (const s of list ?? []) {
      await api(`/session/${s.id}`, { method: 'DELETE' });
      log('cleanup', `deleted ${s.id}`);
    }
  } catch (e) {
    log('cleanup', e.message);
  }

  await browser.close();
  log('done', process.exitCode ? 'WITH FAILURES' : 'ALL OK');
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
