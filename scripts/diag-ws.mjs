import { chromium } from 'playwright-core';

const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
const page = await browser.newPage();
page.on('console', (m) => console.log('CON', m.type(), m.text().slice(0, 300)));
page.on('pageerror', (e) => console.log('ERR', e.message));

await page.goto('http://127.0.0.1:5173/', { waitUntil: 'networkidle' });
await page.click('.tabs button[data-tab="oc"]');
await page.waitForSelector('#pane-oc.active');
await page.waitForTimeout(1200);

const r = await page.evaluate(async () => {
  const out = { steps: [] };
  try {
    const WS = 'D:\\Manga\\nebula-emu\\test-ws-browser';
    window.showDirectoryPicker = async () => {
      out.steps.push('picker-called');
      return { kind: 'directory', name: 'test-ws-browser' };
    };
    window.prompt = (msg) => {
      out.steps.push('prompt:' + String(msg).slice(0, 80));
      return WS;
    };
    window.confirm = () => true;

    out.labelBefore = document.querySelector('#oc-ws-path')?.textContent;
    out.hasBtn = !!document.querySelector('#oc-ws');
    out.state = document.querySelector('#oc-state')?.textContent;

    document.querySelector('#oc-ws').click();
    await new Promise((r) => setTimeout(r, 4000));

    out.label = document.querySelector('#oc-ws-path')?.textContent;
    out.win = window.__NEBULA_OC_WORKSPACE;
    out.logTail = (document.querySelector('#oc-log')?.textContent || '').slice(-700);
    out.activeSession = document.querySelector('#oc-session')?.textContent;
    out.sessionItems = [...document.querySelectorAll('.oc-sess-item')].map((el) =>
      (el.textContent || '').slice(0, 80),
    );
  } catch (e) {
    out.error = String(e && e.stack ? e.stack : e);
  }
  return out;
});

console.log(JSON.stringify(r, null, 2));
await browser.close();
