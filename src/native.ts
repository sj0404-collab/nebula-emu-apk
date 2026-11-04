/**
 * Capacitor surface of the in-app REAL opencode serve (OpencodeNativePlugin.java).
 * On web/desktop these calls no-op — the Bun bridge / vite proxy is used instead.
 */
import { Capacitor, registerPlugin } from '@capacitor/core';

export interface NativeInfo {
  platform: string;
  workspace: string;
  data: string;
  abi: string;
  sdk: number;
  base: string;
  webview?: string;
}

export interface StartResult {
  ok: boolean;
  already?: boolean;
  base?: string;
  error?: string;
}

export interface PickDirectoryResult {
  path?: string;
  name?: string;
  uri?: string;
  cancelled?: boolean;
  error?: string;
  allFilesAccess?: boolean;
}

export interface DirEntry {
  path: string;
  name: string;
  dir: boolean;
}

export interface NebulaEmuPlugin {
  info(): Promise<NativeInfo>;
  start(): Promise<StartResult>;
  stop(): Promise<{ ok: boolean }>;
  status(): Promise<{ healthy: boolean; base: string; running: boolean; pid: number }>;
  httpGet(opts: { path: string }): Promise<{ status: number; body: string; error?: string }>;
  /** Android folder picker (SAF tree → real path when possible). */
  pickDirectory(): Promise<PickDirectoryResult>;
  listDirectory(opts: { path?: string }): Promise<{ entries: DirEntry[]; path: string; error?: string }>;
  /** Open system «All files access» settings for this app. */
  openStorageSettings(): Promise<{ ok: boolean }>;
  addListener(
    name: 'opencodeReady',
    fn: (e: { base?: string; ok?: boolean }) => void,
  ): Promise<{ remove: () => Promise<void> }>;
}

export const NebulaEmu = registerPlugin<NebulaEmuPlugin>('NebulaEmu');

export function isNative(): boolean {
  try {
    return Capacitor.isNativePlatform();
  } catch {
    return false;
  }
}

/**
 * Start the bundled opencode serve on 127.0.0.1:4096 (Android only).
 * Sets window.__NEBULA_OC_BASE so opencode-client talks to it directly.
 */
export async function startOnDeviceOpencode(): Promise<StartResult> {
  if (!isNative()) return { ok: false, error: 'not native' };
  try {
    const info = await NebulaEmu.info();
    // default workspace for this device — user pick overrides via __NEBULA_OC_WORKSPACE
    (window as unknown as { __NEBULA_OC_DEFAULT_WS?: string }).__NEBULA_OC_DEFAULT_WS = info.workspace;
    if (!(window as unknown as { __NEBULA_OC_WORKSPACE?: string }).__NEBULA_OC_WORKSPACE) {
      // no explicit pick yet → use device default as the active workspace
      (window as unknown as { __NEBULA_OC_WORKSPACE?: string }).__NEBULA_OC_WORKSPACE = info.workspace;
    }
    const r = await NebulaEmu.start();
    if (r.ok && r.base) {
      window.__NEBULA_OC_BASE = r.base.replace(/\/$/, '');
    } else if (r.ok && info.base) {
      window.__NEBULA_OC_BASE = info.base.replace(/\/$/, '');
    }
    return r;
  } catch (e) {
    return { ok: false, error: String(e) };
  }
}
