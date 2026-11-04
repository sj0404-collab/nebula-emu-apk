/**
 * Browser persistence (IndexedDB) — sessions, messages, drafts, workspace.
 * Survives reload; export/import moves work between PC and phone.
 */

export interface StoredSessionMeta {
  id: string;
  title: string;
  workspace?: string;
  updatedAt: number;
  /** true while a prompt is in flight (restored after reload) */
  busy?: boolean;
}

export interface StoredDraft {
  sessionID: string;
  text: string;
  updatedAt: number;
}

export interface StoredWorkspace {
  /** absolute path (native) or label for browser FS */
  path: string;
  kind: 'native' | 'browser' | 'default';
  /** File System Access directory handle name (browser); full handle is not IDB-safe (functions) */
  handleName?: string;
  updatedAt: number;
}

const DB_NAME = 'nebula-emu';
const DB_VER = 1;

let dbp: Promise<IDBDatabase> | null = null;

function openDb(): Promise<IDBDatabase> {
  if (dbp) return dbp;
  dbp = new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VER);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains('sessions')) {
        db.createObjectStore('sessions', { keyPath: 'id' });
      }
      if (!db.objectStoreNames.contains('messages')) {
        db.createObjectStore('messages', { keyPath: 'sessionID' });
      }
      if (!db.objectStoreNames.contains('drafts')) {
        db.createObjectStore('drafts', { keyPath: 'sessionID' });
      }
      if (!db.objectStoreNames.contains('meta')) {
        db.createObjectStore('meta', { keyPath: 'key' });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
  return dbp;
}

function tx<T>(
  store: string,
  mode: IDBTransactionMode,
  fn: (s: IDBObjectStore) => IDBRequest<T>,
): Promise<T> {
  return openDb().then(
    (db) =>
      new Promise<T>((resolve, reject) => {
        const t = db.transaction(store, mode);
        const r = fn(t.objectStore(store));
        r.onsuccess = () => resolve(r.result);
        r.onerror = () => reject(r.error);
      }),
  );
}

function getAll<T>(store: string): Promise<T[]> {
  return tx<T[]>(store, 'readonly', (s) => s.getAll() as IDBRequest<T[]>);
}

function put<T>(store: string, value: T): Promise<IDBValidKey> {
  return tx(store, 'readwrite', (s) => s.put(value));
}

function del(store: string, key: string): Promise<undefined> {
  return tx(store, 'readwrite', (s) => s.delete(key)) as Promise<undefined>;
}

function get<T>(store: string, key: string): Promise<T | undefined> {
  return tx(store, 'readonly', (s) => s.get(key) as IDBRequest<T | undefined>);
}

/* ---------- sessions ---------- */

export async function saveSessionMeta(meta: StoredSessionMeta): Promise<void> {
  await put('sessions', { ...meta, updatedAt: Date.now() });
}

export async function listStoredSessions(): Promise<StoredSessionMeta[]> {
  try {
    const all = await getAll<StoredSessionMeta>('sessions');
    return all.sort((a, b) => b.updatedAt - a.updatedAt);
  } catch {
    return [];
  }
}

export async function removeStoredSession(id: string): Promise<void> {
  await del('sessions', id);
  await del('messages', id);
  await del('drafts', id);
}

/* ---------- messages (full history cache) ---------- */

export interface StoredMessages {
  sessionID: string;
  /** JSON of OcMessageEnvelope[] */
  json: string;
  updatedAt: number;
}

export async function saveMessages(sessionID: string, json: string): Promise<void> {
  await put('messages', { sessionID, json, updatedAt: Date.now() } satisfies StoredMessages);
}

export async function loadMessagesJson(sessionID: string): Promise<string | null> {
  try {
    const row = await get<StoredMessages>('messages', sessionID);
    return row?.json ?? null;
  } catch {
    return null;
  }
}

/* ---------- drafts ---------- */

export async function saveDraft(sessionID: string, text: string): Promise<void> {
  if (!sessionID) return;
  await put('drafts', { sessionID, text, updatedAt: Date.now() } satisfies StoredDraft);
}

export async function loadDraft(sessionID: string): Promise<string> {
  try {
    const row = await get<StoredDraft>('drafts', sessionID);
    return row?.text ?? '';
  } catch {
    return '';
  }
}

export async function clearDraft(sessionID: string): Promise<void> {
  await del('drafts', sessionID);
}

/* ---------- workspace ---------- */

const WS_KEY = 'workspace';

export async function saveWorkspace(ws: StoredWorkspace): Promise<void> {
  // handle is structured-cloneable → can live in IDB
  await put('meta', { key: WS_KEY, value: ws });
}

export async function loadWorkspace(): Promise<StoredWorkspace | null> {
  try {
    const row = await get<{ key: string; value: StoredWorkspace }>('meta', WS_KEY);
    return row?.value ?? null;
  } catch {
    return null;
  }
}

/* ---------- export / import (PC ↔ phone) ---------- */

export interface SessionExport {
  format: 'nebula-oc-session';
  version: 1;
  exportedAt: number;
  session: StoredSessionMeta;
  messages: unknown[];
}

export async function exportSession(id: string): Promise<SessionExport | null> {
  const sessions = await listStoredSessions();
  const session = sessions.find((s) => s.id === id);
  if (!session) return null;
  const json = await loadMessagesJson(id);
  let messages: unknown[] = [];
  try {
    messages = json ? (JSON.parse(json) as unknown[]) : [];
  } catch {
    messages = [];
  }
  return {
    format: 'nebula-oc-session',
    version: 1,
    exportedAt: Date.now(),
    session,
    messages,
  };
}

export async function importSession(data: unknown): Promise<StoredSessionMeta | null> {
  const d = data as SessionExport;
  if (!d || d.format !== 'nebula-oc-session' || !d.session?.id) return null;
  const meta: StoredSessionMeta = {
    ...d.session,
    updatedAt: Date.now(),
    busy: false,
  };
  await saveSessionMeta(meta);
  await saveMessages(meta.id, JSON.stringify(d.messages ?? []));
  return meta;
}

/** Export everything (all cached sessions). */
export async function exportAll(): Promise<{
  format: 'nebula-oc-backup';
  version: 1;
  exportedAt: number;
  sessions: StoredSessionMeta[];
  messages: Record<string, unknown[]>;
  workspace: StoredWorkspace | null;
  model: string;
}> {
  const sessions = await listStoredSessions();
  const messages: Record<string, unknown[]> = {};
  for (const s of sessions) {
    const json = await loadMessagesJson(s.id);
    try {
      messages[s.id] = json ? (JSON.parse(json) as unknown[]) : [];
    } catch {
      messages[s.id] = [];
    }
  }
  const workspace = await loadWorkspace();
  const model = localStorage.getItem('nebula.oc.model') ?? '';
  return {
    format: 'nebula-oc-backup',
    version: 1,
    exportedAt: Date.now(),
    sessions,
    messages,
    workspace,
    model,
  };
}

export async function importAll(data: unknown): Promise<number> {
  const d = data as {
    format?: string;
    sessions?: StoredSessionMeta[];
    messages?: Record<string, unknown[]>;
    workspace?: StoredWorkspace | null;
    model?: string;
  };
  if (d?.format !== 'nebula-oc-backup' || !Array.isArray(d.sessions)) {
    throw new Error('неизвестный формат бэкапа');
  }
  let n = 0;
  for (const s of d.sessions) {
    if (!s?.id) continue;
    await saveSessionMeta({ ...s, updatedAt: Date.now(), busy: false });
    const msgs = d.messages?.[s.id];
    if (msgs) await saveMessages(s.id, JSON.stringify(msgs));
    n++;
  }
  if (d.workspace?.path) await saveWorkspace({ ...d.workspace, updatedAt: Date.now() });
  if (d.model) localStorage.setItem('nebula.oc.model', d.model);
  return n;
}

/* ---------- open tabs (parallel sessions UI) ---------- */

export async function saveOpenTabs(ids: string[]): Promise<void> {
  await put('meta', { key: 'openTabs', value: ids });
}

export async function loadOpenTabs(): Promise<string[]> {
  try {
    const row = await get<{ key: string; value: string[] }>('meta', 'openTabs');
    return Array.isArray(row?.value) ? row.value : [];
  } catch {
    return [];
  }
}

export async function saveActiveTab(id: string): Promise<void> {
  await put('meta', { key: 'activeTab', value: id });
}

export async function loadActiveTab(): Promise<string | null> {
  try {
    const row = await get<{ key: string; value: string }>('meta', 'activeTab');
    return row?.value ?? null;
  } catch {
    return null;
  }
}
