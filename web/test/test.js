// Тест страницы с имитацией датчика (mock.js вместо настоящего Web Bluetooth).
// Запуск: npm i -D playwright && npx playwright install chromium && node web/test/test.js
let chromium;
try { ({ chromium } = require('playwright')); } catch (e) { ({ chromium } = require('/opt/node22/lib/node_modules/playwright')); }
const path = require('path'); const fs = require('fs');
const PAGE = 'file://' + path.resolve(__dirname, '..', 'index.html');
const OUT = process.env.SHOTS || require('os').tmpdir() + '/knifeangle-shots'; fs.mkdirSync(OUT, { recursive: true });
let fails = 0;
function check(name, cond, info) { console.log((cond ? 'PASS ' : 'FAIL ') + name + (cond ? '' : '  -> ' + JSON.stringify(info))); if (!cond) fails++; }
(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2 });
  await ctx.addInitScript({ path: __dirname + '/mock.js' });
  const page = await ctx.newPage();
  const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(PAGE);
  const st = () => page.evaluate(() => ({ cls: document.body.className, angle: document.getElementById('angle').textContent,
    hint: document.getElementById('hintText').textContent, arrow: document.getElementById('arrow').textContent,
    delta: document.getElementById('delta').textContent, status: document.getElementById('statusText').textContent,
    btn: document.getElementById('connect').textContent, batt: document.getElementById('battery').textContent,
    target: document.getElementById('target').value, tol: document.getElementById('tol').value,
    calDisabled: document.getElementById('calibrate').disabled }));
  const send = async (deg, flags) => { await page.evaluate(([d, f]) => __mock.send(d, f), [deg, flags]); await page.waitForTimeout(30); return st(); };

  let s = await st();
  check('idle: no angle', s.angle === '--.-°' && s.cls === '' && s.target === '15.0' && s.tol === '1.0', s);
  check('idle: calibrate disabled', s.calDisabled, s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/0-idle.png' });

  await page.click('#connect'); await page.waitForTimeout(100);
  s = await st();
  check('connected', s.status.startsWith('Подключено') && s.btn === 'Отключиться' && s.batt.includes('87'), s);
  const log1 = await page.evaluate(() => __btlog);
  check('requestDevice filters by name + service', log1[0].includes('KnifeAngle') && log1[0].includes('8c900001'), log1);

  s = await send(12.3, 0);
  check('uncalibrated: gray + message', s.cls === 'st-gray' && s.hint.startsWith('Сначала откалибруй') && s.angle === '12.3°' && s.delta === '', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/1-uncalibrated.png' });

  s = await send(15.4, 1);
  check('in target: green', s.cls === 'st-green' && s.hint === 'В цели', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/2-green.png' });
  s = await send(16.2, 1);
  check('hysteresis: 1.2 stays green', s.cls === 'st-green', s);
  s = await send(16.3, 1);
  check('hysteresis: 1.3 stays green', s.cls === 'st-green', s);
  s = await send(16.4, 1);
  check('exit at 1.4: yellow, tilt less', s.cls === 'st-yellow' && s.hint === 'наклони меньше' && s.arrow === '▼' && s.delta === 'Δ +1.4°', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/3-yellow.png' });
  s = await send(16.1, 1);
  check('1.1 outside: no re-entry (needs <=1.0)', s.cls === 'st-yellow', s);
  s = await send(16.0, 1);
  check('re-enter at 1.0', s.cls === 'st-green', s);
  s = await send(13.5, 1);
  check('exit low: yellow, tilt more', s.cls === 'st-yellow' && s.hint === 'наклони больше' && s.arrow === '▲' && s.delta === 'Δ −1.5°', s);
  s = await send(11.0, 1);
  check('4.0 = tol+3: still yellow', s.cls === 'st-yellow', s);
  s = await send(10.9, 1);
  check('4.1: red', s.cls === 'st-red' && s.hint === 'наклони больше', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/4-red.png' });
  s = await send(20, 3);
  check('sensor error flag: gray', s.cls === 'st-gray' && s.hint.startsWith('Ошибка датчика'), s);

  // устаревание
  s = await send(15, 1);
  await page.waitForTimeout(2100);
  s = await st();
  check('stale after 1.5 s: gray, no data', s.cls === 'st-gray' && s.hint === 'Нет данных от датчика', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/5-stale.png' });

  // пресет и сохранение
  await page.click('#presets button[data-v="20"]');
  s = await send(20.5, 1);
  check('preset 20 -> target 20, green', s.target === '20.0' && s.cls === 'st-green', s);
  await page.fill('#tol', '0,5'); await page.press('#tol', 'Tab');
  s = await send(20.5, 1);
  check('tolerance comma input 0,5 accepted', s.tol === '0.5', s);
  await page.click('#targetPlus');
  s = await st();
  check('+ button: 20.5', s.target === '20.5', s);
  const saved = await page.evaluate(() => localStorage.getItem('knifeAngle.settings.v1'));
  check('settings saved', saved && JSON.parse(saved).target === 20.5 && JSON.parse(saved).tol === 0.5, saved);

  // калибровка
  await page.click('#calibrate'); await page.waitForTimeout(50);
  const log2 = await page.evaluate(() => __btlog);
  check('calibrate writes 0x01', log2.includes('write:1'), log2);

  // потеря связи и переподключение
  await page.evaluate(() => { __failConnect = true; __mock.drop(); });
  await page.waitForTimeout(50);
  s = await st();
  check('drop: reconnect scheduled', s.status.startsWith('Связь потеряна') && s.btn === 'Отменить' && s.cls === '', s);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/6-reconnect.png' });
  await page.waitForTimeout(1200); // первая попытка (1 с) падает
  await page.evaluate(() => { __failConnect = false; });
  await page.waitForTimeout(2300); // вторая (2 с) удаётся
  s = await st();
  check('reconnected after backoff', s.status.startsWith('Подключено') && s.btn === 'Отключиться', s);
  s = await send(20.4, 1);
  check('data after reconnect (no duplicate handlers)', s.angle === '20.4°' && s.cls === 'st-green', s);

  // ручное отключение
  await page.click('#connect'); await page.waitForTimeout(50);
  s = await st();
  check('user disconnect: no reconnect', s.status === 'Не подключено' && s.btn === 'Подключиться', s);
  await page.waitForTimeout(1500);
  s = await st();
  check('still disconnected', s.status === 'Не подключено', s);

  // перезагрузка: настройки восстановлены
  await page.reload(); await page.waitForTimeout(100);
  s = await st();
  check('reload keeps settings', s.target === '20.5' && s.tol === '0.5', s);
  check('no page errors', errors.length === 0, errors);

  // localStorage сломан
  const ctx2 = await browser.newContext({ viewport: { width: 390, height: 844 } });
  await ctx2.addInitScript(() => {
    Storage.prototype.getItem = () => { throw new Error('denied'); };
    Storage.prototype.setItem = () => { throw new Error('denied'); };
  });
  await ctx2.addInitScript({ path: __dirname + '/mock.js' });
  const p2 = await ctx2.newPage(); const err2 = []; p2.on('pageerror', e => err2.push(e.message));
  await p2.goto(PAGE); await p2.click('#presets button[data-v="25"]');
  const t2 = await p2.evaluate(() => document.getElementById('target').value);
  check('works without localStorage', t2 === '25.0' && err2.length === 0, { t2, err2 });

  // нет Web Bluetooth
  const ctx3 = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2 });
  const p3 = await ctx3.newPage(); await p3.goto(PAGE);
  const u = await p3.evaluate(() => ({ shown: getComputedStyle(document.getElementById('unsupported')).display !== 'none', dis: document.getElementById('connect').disabled }));
  check('no Web Bluetooth: message + button disabled', u.shown && u.dis, u);
  await p3.screenshot({ path: OUT + '/7-unsupported.png' });

  // широкий экран
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.click('#connect'); await page.waitForTimeout(100); await send(22.6, 1);
  await page.waitForTimeout(400); await page.screenshot({ path: OUT + '/8-desktop.png' });

  await browser.close();
  console.log(fails ? `\n${fails} FAILED` : '\nALL PASSED');
  process.exit(fails ? 1 : 0);
})();
