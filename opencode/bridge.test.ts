import { describe, expect, test } from 'bun:test';

/**
 * Bridge smoke test: requires a running bridge on BRIDGE_PORT (default 18787 in CI)
 * and a real opencode serve. Run: bun test opencode/bridge.test.ts
 */
const PORT = process.env.BRIDGE_PORT ?? '18787';
const BASE = `http://127.0.0.1:${PORT}/api/opencode`;

describe('opencode bridge', () => {
  test('GET /bridge reports a healthy real server', async () => {
    const r = await fetch(`${BASE}/bridge`);
    expect(r.ok).toBe(true);
    const j = (await r.json()) as { ok: boolean; opencode?: string; source?: string };
    expect(j.ok).toBe(true);
    expect(j.opencode).toBeTruthy();
    expect(typeof j.opencode).toBe('string');
  }, 15_000);

  test('GET /global/health proxies to opencode serve', async () => {
    const r = await fetch(`${BASE}/global/health`);
    expect(r.ok).toBe(true);
    const j = (await r.json()) as { healthy?: boolean };
    expect(j.healthy).not.toBe(false);
  }, 10_000);

  test('POST /session creates a real session', async () => {
    const r = await fetch(`${BASE}/session`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ title: 'bridge-test' }),
    });
    expect(r.ok).toBe(true);
    const j = (await r.json()) as { id: string };
    expect(j.id).toBeTruthy();
  }, 10_000);
});
