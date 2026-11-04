/**
 * Workspace binding — pick a folder on phone / PC / browser and work in it.
 *
 * - Android: native SAF tree picker → real path when possible (primary external)
 * - Desktop browser: File System Access API handle (browser FS) + optional absolute path for the bridge
 * - Fallback: prompt for absolute path (bridge/opencode needs a real path)
 */
import * as store from './store';
import { isNative, NebulaEmu } from './native';

export type WorkspaceInfo = store.StoredWorkspace;

declare global {
  interface Window {
    showDirectoryPicker?: (opts?: {
      mode?: 'read' | 'readwrite';
      id?: string;
      startIn?: string;
    }) => Promise<FileSystemDirectoryHandle>;
  }
}

let current: WorkspaceInfo | null = null;

export function getWorkspace(): WorkspaceInfo | null {
  return current;
}

export function workspacePath(): string {
  return current?.path ?? '';
}

export async function initWorkspace(): Promise<WorkspaceInfo | null> {
  current = await store.loadWorkspace();
  applyToWindow();
  return current;
}

function applyToWindow(): void {
  const w = window as unknown as { __NEBULA_OC_WORKSPACE?: string };
  if (current?.path) w.__NEBULA_OC_WORKSPACE = current.path;
}

export async function setWorkspace(ws: {
  path: string;
  kind: WorkspaceInfo['kind'];
  handle?: FileSystemDirectoryHandle | { name?: string };
}): Promise<WorkspaceInfo> {
  // Never persist the live handle (functions break IndexedDB structured clone)
  const handleName =
    ws.handle && typeof ws.handle === 'object' && 'name' in ws.handle
      ? String((ws.handle as { name?: string }).name ?? '')
      : undefined;
  const prev = current;
  current = {
    path: ws.path,
    kind: ws.kind,
    handleName: handleName || prev?.handleName,
    updatedAt: Date.now(),
  };
  try {
    await store.saveWorkspace(current);
  } catch (e) {
    console.warn('[workspace] save failed', e);
  }
  applyToWindow();
  return current;
}

export async function clearWorkspace(): Promise<void> {
  current = null;
  try {
    await store.saveWorkspace({
      path: '',
      kind: 'default',
      updatedAt: Date.now(),
    });
  } catch {
    /* ignore */
  }
  const w = window as unknown as { __NEBULA_OC_WORKSPACE?: string };
  delete w.__NEBULA_OC_WORKSPACE;
}

/**
 * Interactive folder pick. Strategy depends on platform.
 * Returns updated workspace or null if cancelled.
 */
export async function pickWorkspace(): Promise<WorkspaceInfo | null> {
  // 1) Android / iOS native
  if (isNative()) {
    try {
      const r = await NebulaEmu.pickDirectory();
      if (r?.path) {
        const ws = await setWorkspace({ path: r.path, kind: 'native' });
        if (!r.allFilesAccess && r.path && !r.path.includes('/Android/data/')) {
          const open = window.confirm(
            `Папка: ${r.path}\n\n` +
              'Для записи opencode нужен доступ «All files access».\n' +
              'Открыть настройки системы?',
          );
          if (open) {
            try {
              await NebulaEmu.openStorageSettings();
            } catch { /* ignore */ }
          }
        }
        return ws;
      }
      if (r?.cancelled) return null;
      if (r?.error) {
        window.alert(`Выбор папки: ${r.error}`);
        return null;
      }
    } catch (e) {
      console.warn('[workspace] native pick failed', e);
      // fall through to prompt
    }
  }

  // 2) Chromium File System Access — browser folder
  if (typeof window.showDirectoryPicker === 'function') {
    try {
      const handle = await window.showDirectoryPicker({ mode: 'readwrite' });
      const name = handle.name || 'folder';
      // For opencode/bridge we still need a real path — store handle name + path;
      // user can also type absolute path for the agent.
      const pathInput = window.prompt(
        `Браузер выбрал папку «${name}».\n` +
          `Абсолютный путь для opencode (ПК), пусто = только браузерный FS:`,
        '',
      );
      if (pathInput === null) {
        // user cancelled path prompt — still keep browser folder name
        return setWorkspace({ path: name, kind: 'browser', handle });
      }
      const path = pathInput.trim() || name;
      return setWorkspace({ path, kind: pathInput.trim() ? 'native' : 'browser', handle });
    } catch (e) {
      // AbortError → user closed picker
      if (e instanceof DOMException && e.name === 'AbortError') return null;
      console.warn('[workspace] showDirectoryPicker failed', e);
    }
  }

  // 3) Fallback: absolute path prompt (works with desktop bridge)
  const p = window.prompt(
    'Абсолютный путь к рабочей папке\n' +
      '(Windows: D:\\Projects\\myrepo · Linux/macOS: /home/me/proj):',
    current?.path || '',
  );
  if (p === null) return null;
  const path = p.trim();
  if (!path) return null;
  return setWorkspace({ path, kind: 'native' });
}

/** List folders on Android via native plugin (for in-app browser). */
export async function listNativeDirs(path?: string): Promise<{ path: string; name: string; dir: boolean }[]> {
  if (!isNative()) return [];
  try {
    const r = await NebulaEmu.listDirectory({ path: path || '' });
    return r?.entries ?? [];
  } catch {
    return [];
  }
}
