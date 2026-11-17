// Script of the web page. CMake embeds this file; webui.c (send_page) sends it inside <script>.
const $ = id => document.getElementById(id);

/* ---- tabs ---- */
function show(t) {
  document.querySelectorAll('section[data-t]').forEach(e => e.style.display = e.dataset.t == t ? '' : 'none');
  document.querySelectorAll('#nav a').forEach(a => a.classList.toggle('on', a.dataset.t == t));
  try { localStorage.setItem('tab', t); } catch (e) {}
}
document.querySelectorAll('#nav a').forEach(a => a.onclick = () => { show(a.dataset.t); scrollTo(0, 0); });
{
  let t = 'home';
  try { t = localStorage.getItem('tab') || t; } catch (e) {}
  if (!document.querySelector('section[data-t=' + t + ']')) t = 'home';
  show(t);
}

/* ---- overview: state, controls, boiler, chart ---- */
let S = null;       // last /api/status
let T = null, tt;   // target being stepped with - / +, and its send timer
let weekDrawn = false;
const SRC = {constant: 'Set to', comfort: 'Comfort', eco: 'Eco', program: 'Program', manual: 'Manual', boost: 'Boost',
             away: 'Away', summer: 'Summer: heating off'};
const DHW = {always: 'allowed', window: 'allowed (window)', guard: 'on (tank too cold)', piggy: 'on (after heating)',
             boost: 'on (boost)', pasteur: 'on (hot cycle)', noclock: 'allowed (no clock)',
             closed: 'off (outside windows)', off: 'off', away: 'off (away)'};
const dur = m => m >= 60 ? Math.floor(m / 60) + ' h ' + (m % 60) + ' min' : m + ' min';

// A console command (the same ones as on the System tab), then a fresh state
async function cmd(line) {
  try { await fetch('/api/cmd', {method: 'POST', body: line}); } catch (e) {}
  await upd();
}

function step(d) {
  if (!S) return;
  T = Math.min(30, Math.max(5, (T == null ? S.schedule.target : T) + d));
  $('tgt').textContent = T.toFixed(1) + ' °C';
  clearTimeout(tt);
  tt = setTimeout(async () => { const v = T; await cmd('TARGET ' + v.toFixed(1)); if (T == v) T = null; }, 700);
}

function chart(h) {
  const c = $('chart');
  let lo = 99, hi = -99;
  h.forEach(p => { if (p[0] != null) { lo = Math.min(lo, p[0], p[1]); hi = Math.max(hi, p[0], p[1]); } });
  if (lo > hi) { lo = 15; hi = 25; }
  lo -= 0.5; hi += 0.5;
  const y = v => 160 - (v - lo) / (hi - lo) * 160, x = i => i * 1440 / Math.max(h.length - 1, 1);
  let d = '', sp = '', band = '';
  h.forEach((p, i) => {
    if (p[0] != null) d += (d ? 'L' : 'M') + x(i) + ' ' + y(p[0]);
    sp += (sp ? 'L' : 'M') + x(i) + ' ' + y(p[1]);
    if (p[2]) band += `<rect x='${x(i)}' y='150' width='${1440 / h.length + 1}' height='10' fill='#ff7043'/>`;
  });
  c.innerHTML = band + `<path d='${sp}' stroke='#ffb74d' fill='none' stroke-width='3' vector-effect='non-scaling-stroke'/>` +
    `<path d='${d}' stroke='#4fc3f7' fill='none' stroke-width='3' vector-effect='non-scaling-stroke'/>` +
    `<text x='8' y='18' fill='#90a4ae' font-size='16'>${hi.toFixed(1)}</text>` +
    `<text x='8' y='154' fill='#90a4ae' font-size='16'>${lo.toFixed(1)}</text>`;
}

async function upd() {
  let s;
  try { s = await (await fetch('/api/status')).json(); } catch (e) { $('info').textContent = 'offline'; return; }
  S = s;
  const sc = s.schedule, heat = s.mode == 'heat';
  $('room').textContent = s.sensor_ok ? s.room.toFixed(1) + ' °C' : 'sensor error';
  $('hum').textContent = s.sensor_ok ? s.humidity.toFixed(0) + ' % humidity' : '';
  if (T == null) $('tgt').textContent = sc.target.toFixed(1) + ' °C';
  let src = heat ? (SRC[sc.source] || sc.source) : 'Heating off';
  if (heat && sc.source == 'boost') src += ', ' + Math.ceil(sc.boost_left_s / 60) + ' min left';
  else if (heat && sc.next_change_min >= 0 && ['comfort', 'eco', 'program'].includes(sc.source))
    src += ', next change in ' + dur(sc.next_change_min);
  $('src').textContent = src;
  $('bmode').textContent = heat ? 'Heating: on' : 'Heating: off';
  $('bmode').className = heat ? 'on' : 'g';
  $('bboost').textContent = sc.boost_left_s > 0 ? 'Stop boost' : 'Boost';
  $('bboost').hidden = !heat || !sc.boost_on;
  $('baway').textContent = sc.away ? "I'm back" : 'Away';
  $('baway').hidden = !heat || !sc.away_on;
  $('bprog').hidden = sc.source != 'manual';
  $('alert').textContent = s.alerts.count ? s.alerts.titles.map(t => '⚠ ' + t).join('\n') : '';
  const up = s.boiler_online, who = s.boiler_sim ? 'simulated boiler' : 'boiler';
  $('info').textContent = (s.demand ? 'Heating to ' + s.flow_setpoint.toFixed(0) + ' °C' : 'Idle') + ' · ' +
    (up ? who + ' answering' : who + ' not answering');
  $('c_flow').textContent = up ? s.flow.toFixed(0) + ' / ' + s.control.return.toFixed(0) + ' °C' : '–';
  $('c_mod').textContent = !up ? '–' : s.flame ? Math.min(100, s.mod).toFixed(0) + ' %' : 'off';
  $('c_pres').textContent = s.pressure == null ? '–' : s.pressure.toFixed(2) + ' bar';
  $('c_tank').textContent = s.dhw.tank_valid ? s.dhw.tank.toFixed(0) + ' °C' : '–';
  $('c_dhw').textContent = 'Hot water tank, ' + (DHW[s.dhw.reason] || s.dhw.reason);
  $('c_starts').textContent = s.control.starts_last_hour;
  chart(s.history);
  if (!weekDrawn) { weekDrawn = true; drawWeek(); }  // the bars show temperatures once the setpoint is known
}
$('tm').onclick = () => step(-0.5);
$('tp').onclick = () => step(0.5);
$('bmode').onclick = () => S && cmd(S.mode == 'heat' ? 'MODE off' : 'MODE heat');
$('bboost').onclick = () => S && cmd(S.schedule.boost_left_s > 0 ? 'BOOST off' : 'BOOST on');
$('baway').onclick = () => S && cmd(S.schedule.away ? 'AWAY off' : 'AWAY on');
$('bprog').onclick = () => cmd('RESUME');
upd();
setInterval(upd, 5000);

/* ---- weekly program: bars and switch-time rows over the text fields prog0..prog6 ---- */
const DN = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
const LV = {c: 'Comfort', e: 'Eco'};
const hhmm = t => ('0' + Math.floor(t / 60)).slice(-2) + ':' + ('0' + t % 60).slice(-2);
let P = [];  // per day: [{t: minutes, l: 'c' | 'e' | '21.5'}]

function parseDay(v) {
  return v.trim().split(/\s+/).map(x => {
    const m = /^(\d\d?):(\d\d)=t?(c|e|[\d.]+)$/i.exec(x);
    return m ? {t: +m[1] * 60 + +m[2], l: m[3].toLowerCase()} : null;
  }).filter(x => x && x.t < 1440).sort((a, b) => a.t - b.t);
}
// The level a day starts with: the last switch of the nearest earlier day that has one
function levelBefore(d) {
  for (let k = 1; k <= 7; k++) {
    const q = P[(d - k + 7) % 7];
    if (q.length) return q[q.length - 1].l;
  }
  return 'c';
}
function levelTemp(l) {
  const eco = $('eco_setback_k') ? +$('eco_setback_k').value : NaN;
  if (l == 'c') return S ? S.setpoint : NaN;
  if (l == 'e') return S ? S.setpoint - eco : NaN;
  return +l;
}
function bar(d) {
  const segs = [];
  let t0 = 0, l = levelBefore(d);
  P[d].forEach(x => { if (x.t > t0) segs.push([t0, x.t, l]); t0 = x.t; l = x.l; });
  segs.push([t0, 1440, l]);
  return segs.map(s => {
    const t = levelTemp(s[2]), col = s[2] == 'c' ? '#ff7043' : s[2] == 'e' ? '#1565c0' : '#7e57c2';
    const txt = s[1] - s[0] >= 150 && !isNaN(t) ? +t.toFixed(1) + '°' : '';
    return `<i style='left:${s[0] / 14.4}%;width:${(s[1] - s[0]) / 14.4}%;background:${col}'>${txt}</i>`;
  }).join('');
}
function ptRow(d, i, x) {
  const fixed = !LV[x.l];
  return `<div class=pt data-d=${d} data-i=${i}><input type=time step=900 value=${hhmm(x.t)}>` +
    `<select><option value=c${x.l == 'c' ? ' selected' : ''}>Comfort</option>` +
    `<option value=e${x.l == 'e' ? ' selected' : ''}>Eco</option>` +
    `<option value=t${fixed ? ' selected' : ''}>Temperature</option></select>` +
    (fixed ? `<input type=number min=5 max=30 step=0.5 value=${x.l}>` : '') +
    `<button type=button data-del=1 title='Remove'>✕</button></div>`;
}
function drawWeek() {
  const w = $('week');
  if (!w) return;
  const open = [...w.children].findIndex(e => e.open);
  w.innerHTML = '';
  for (let d = 0; d < 7; d++) {
    const el = document.createElement('details');
    el.className = 'day';
    el.open = d == open;
    el.innerHTML = `<summary><span>${DN[d]}</span><div class=bar>${bar(d)}</div></summary>` +
      (P[d].length ? '' : '<p class=hint>No switch times: the day continues the day before.</p>') +
      P[d].map((x, i) => ptRow(d, i, x)).join('') +
      `<div class=row>` + (P[d].length < 6 ? `<button type=button class=g data-add=${d}>+ Add a switch time</button>` : '') +
      `<button type=button class=g data-all=${d}>Copy to all days</button></div>`;
    w.appendChild(el);
  }
}
// Writes the days back into the text fields (what the form sends) and redraws
function syncWeek() {
  for (let d = 0; d < 7; d++) {
    P[d].sort((a, b) => a.t - b.t);
    const f = $('prog' + d), v = P[d].map(x => hhmm(x.t) + '=' + x.l).join(' ');
    if (f.value != v) { f.value = v; f.dispatchEvent(new Event('input', {bubbles: true})); }
  }
  drawWeek();
}
if ($('week')) {
  for (let d = 0; d < 7; d++) {
    P[d] = parseDay($('prog' + d).value);
    $('prog' + d).addEventListener('change', () => { P[d] = parseDay($('prog' + d).value); drawWeek(); });
  }
  drawWeek();
  $('week').addEventListener('change', e => {
    const r = e.target.closest('.pt');
    if (!r) return;
    const x = P[+r.dataset.d][+r.dataset.i], [tm, sel, num] = [r.children[0], r.children[1], r.querySelector('input[type=number]')];
    if (tm.value) x.t = +tm.value.slice(0, 2) * 60 + +tm.value.slice(3, 5);
    if (sel.value != 't') x.l = sel.value;
    else x.l = String(Math.min(30, Math.max(5, num && num.value ? +num.value : (S ? S.setpoint : 21))));
    syncWeek();
  });
  $('week').addEventListener('click', e => {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.del) {
      const r = b.closest('.pt');
      P[+r.dataset.d].splice(+r.dataset.i, 1);
    } else if (b.dataset.add) {
      const p = P[+b.dataset.add], t = p.length ? Math.min(1425, p[p.length - 1].t + 120) : 360;
      p.push({t: t, l: p.length && p[p.length - 1].l == 'c' ? 'e' : 'c'});
    } else if (b.dataset.all) {
      const src = P[+b.dataset.all];
      for (let d = 0; d < 7; d++) P[d] = src.map(x => ({t: x.t, l: x.l}));
    } else return;
    syncWeek();
  });
}

/* ---- long forms: the Save button stays in view and counts the changes ---- */
document.querySelectorAll('form[data-dirty]').forEach(f => {
  const b = f.querySelector('.save button'), label = b.textContent;
  const check = () => {
    let n = 0;
    f.querySelectorAll('input[name],select[name],textarea[name]').forEach(e => {
      if (e.type == 'hidden') return;
      if (e.type == 'checkbox' ? e.checked != e.defaultChecked
          : e.tagName == 'SELECT' ? [...e.options].some(o => o.selected != o.defaultSelected) : e.value != e.defaultValue) n++;
    });
    b.textContent = n ? label + ' (' + n + ' changed)' : label;
    b.classList.toggle('dirty', n > 0);
    f.dataset.n = n;
  };
  f.addEventListener('input', check);
  f.addEventListener('change', check);
  f.addEventListener('submit', () => { f.dataset.n = 0; });
});
addEventListener('beforeunload', e => {
  if ([...document.querySelectorAll('form[data-dirty]')].some(f => +f.dataset.n > 0)) { e.preventDefault(); e.returnValue = ''; }
});

/* ---- system tab ---- */
async function upload() {
  const f = $('fw').files[0], m = $('fwmsg');
  if (!f) { m.textContent = 'Choose a .bin file first'; return; }
  m.textContent = 'Uploading ' + f.name + '...';
  try { const r = await fetch('/ota', {method: 'POST', body: f}); m.textContent = await r.text(); }
  catch (e) { m.textContent = 'Upload failed: ' + e; }
}
async function runcmd() {
  const c = $('cmd').value || 'STAT', o = $('cmdout');
  o.textContent = '...';
  try { const r = await fetch('/api/cmd', {method: 'POST', body: c}); o.textContent = await r.text() || '(no output)'; }
  catch (e) { o.textContent = 'failed: ' + e; }
}
