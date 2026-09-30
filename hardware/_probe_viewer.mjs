// _probe_viewer.mjs — 探测 easyeda-viewer 内部结构 / 文档树（AI 探索"眼睛"的能力）
// 用法（在 viewer 仓库根跑，node_modules 里有 puppeteer-core）：
//   node .bridge-work/skill/_probe_viewer.mjs <url>
import puppeteer from 'puppeteer-core';
import { existsSync } from 'node:fs';

const CHROME = [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
].find(existsSync);
if (!CHROME) { console.log('ERR no chrome'); process.exit(1); }

const url = process.argv[2];
const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--no-sandbox', '--disable-gpu', '--hide-scrollbars'],
});
const page = await browser.newPage();
await page.setViewport({ width: 1600, height: 1000 });
page.on('pageerror', (e) => console.log('PAGEERR', String(e.message).slice(0, 200)));
await page.goto(url, { waitUntil: 'networkidle2', timeout: 60000 });
await new Promise((r) => setTimeout(r, 12000));

const frames = page.frames();
console.log('frames:', frames.length, frames.map((f) => f.url().slice(0, 60)));

for (const fr of frames) {
  if (!fr.url().includes('viewer.html')) continue;
  const info = await fr.evaluate(() => {
    const out = {};
    out.has_ev = !!window.__ev;
    if (window.__ev) {
      out.ev_keys = Object.keys(window.__ev);
      const sh = window.__ev.shell;
      if (sh) out.shell_keys = Object.keys(sh).slice(0, 60);
      // 尝试常见 API
      for (const k of ['docs', 'docList', 'open', 'model', 'tree', 'nodes']) {
        try { if (window.__ev[k] !== undefined) out['ev_' + k] = String(typeof window.__ev[k]); } catch (e) {}
      }
    }
    // 文档树 DOM 扫描
    const seen = new Set();
    const nodes = [];
    for (const e of document.querySelectorAll('*')) {
      const t = (e.textContent || '').trim();
      if (!t || t.length > 40) continue;
      const cls = (e.className || '').toString();
      if (!/node|item|tree|doc|row/i.test(cls)) continue;
      const key = cls + '|' + t;
      if (seen.has(key)) continue;
      seen.add(key);
      nodes.push({ tag: e.tagName, cls: cls.slice(0, 50), text: t.slice(0, 40), ds: JSON.stringify(e.dataset || {}).slice(0, 80) });
      if (nodes.length > 50) break;
    }
    out.dom_nodes = nodes;
    return out;
  });
  console.log('==== iframe info ====');
  console.log(JSON.stringify(info, null, 1).slice(0, 6000));
}
await browser.close();
