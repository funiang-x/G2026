// _shot_doc.mjs — 打开工程包(.epro2) → 点选指定文档 → 按文档坐标设相机 → 截图（AI 的眼睛·完整版）
// 用法（cwd = viewer 仓库根）：
//   node .bridge-work/skill/_shot_doc.mjs <url> <out.png> <docName> [w] [h] [waitMs] [camX] [camY] [camScale]
//   - docName 空串 = 不动文档树；camX/camY 为**文档坐标**（同原理图里元件的 x/y），camScale=世界→像素倍率
//   - 不传 cam* 则整页 fit；camScale>0 时用 window.__ev.shell.camera.centerOn 精确定位
import puppeteer from 'puppeteer-core';
import { existsSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const CHROME = [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
].find(existsSync);
if (!CHROME) { console.log('ERR no chrome'); process.exit(1); }

const url = process.argv[2];
const out = process.argv[3];
const docName = process.argv[4] || '';
const W = Number(process.argv[5] || 1800);
const H = Number(process.argv[6] || 1000);
const WAIT = Number(process.argv[7] || 12000);
const CAMX = Number(process.argv[8] || 'NaN');
const CAMY = Number(process.argv[9] || 'NaN');
const CAMS = Number(process.argv[10] || 0);

mkdirSync(dirname(resolve(out)), { recursive: true });
const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--no-sandbox', '--disable-gpu', '--hide-scrollbars'],
});
try {
  const page = await browser.newPage();
  await page.setViewport({ width: W, height: H, deviceScaleFactor: 1 });
  page.on('pageerror', (e) => console.log('PAGEERR', String(e.message).slice(0, 160)));
  await page.goto(url, { waitUntil: 'networkidle2', timeout: 60000 });

  const fr = page.frames().find((f) => f.url().includes('viewer.html'));
  if (!fr) throw new Error('viewer iframe not found');
  await fr.waitForSelector('.ev-tree-row', { timeout: 40000 });
  await new Promise((r) => setTimeout(r, 2500));

  if (docName) {
    const clicked = await fr.evaluate((name) => {
      const rows = [...document.querySelectorAll('.ev-tree-row')];
      const hit = rows.find((r) => (r.textContent || '').includes(name));
      if (!hit) return false;
      hit.click();
      return true;
    }, docName);
    console.log('click "' + docName + '": ' + clicked);
    await new Promise((r) => setTimeout(r, Math.max(3500, WAIT - 4500)));
  } else {
    await new Promise((r) => setTimeout(r, WAIT));
  }

  if (CAMS > 0 && !Number.isNaN(CAMX) && !Number.isNaN(CAMY)) {
    // 观察：直接改 camera 会被 viewer 的 fit 重置；改用"算出该点的屏幕坐标 + 在其上派发滚轮"
    const info = await fr.evaluate((dx, dy, times) => {
      const cam = window.__ev && window.__ev.shell && window.__ev.shell.camera;
      if (!cam) return { err: 'no camera' };
      const px = cam.tx + dx * cam.scale;
      const py = cam.ty + (-dy) * cam.scale;
      const out = { px: px, py: py, scale0: cam.scale, times: times };
      const cv = document.querySelector('.ev-canvas') || document.querySelector('canvas');
      const rect = cv.getBoundingClientRect();
      out.inside = px >= 0 && px <= rect.width && py >= 0 && py <= rect.height;
      for (let i = 0; i < times; i++) {
        cv.dispatchEvent(new WheelEvent('wheel',
          { deltaY: -400, clientX: px, clientY: py, bubbles: true, cancelable: true }));
      }
      out.scale1 = cam.scale;
      return out;
    }, CAMX, CAMY, Math.max(1, Math.round(CAMS)));
    console.log('zoom: ' + JSON.stringify(info));
    await new Promise((r) => setTimeout(r, 1500));
  }

  const canvasBox = await fr.evaluate(() => {
    const c = document.querySelector('canvas');
    if (!c) return null;
    const r = c.getBoundingClientRect();
    return { x: r.x, y: r.y, width: r.width, height: r.height };
  });
  if (canvasBox && canvasBox.width > 200 && canvasBox.height > 200) {
    await page.screenshot({ path: resolve(out), clip: canvasBox });
  } else {
    await page.screenshot({ path: resolve(out) });
  }
  console.log('OK ' + out);
} catch (e) {
  console.log('ERR ' + String(e.message || e));
  process.exitCode = 1;
} finally {
  await browser.close();
}
