/* SPDX-License-Identifier: GPL-2.0-or-later
 * Status polling is deliberately dumb: one /api/status every 500 ms, and the
 * pictures come over their own long-lived MJPEG connections so a slow poll can
 * never stutter the monitor.
 */
'use strict';

const $ = (sel) => document.querySelector(sel);

async function api(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) {
    const err = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(err.error || res.statusText);
  }
  return res.json();
}

const fmtSize = (b) => {
  if (!b) return '';
  const u = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let i = 0, v = b;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return v.toFixed(v < 10 && i > 0 ? 1 : 0) + ' ' + u[i];
};

const fmtTime = (s) => {
  if (!s || s < 0) return '--:--:--';
  const t = Math.floor(s);
  return [Math.floor(t / 3600), Math.floor(t / 60) % 60, t % 60]
    .map((n) => String(n).padStart(2, '0')).join(':');
};

/* Meter scale: -60 dBFS at the bottom, 0 at the top. */
const meterHeight = (db) => Math.max(0, Math.min(100, ((db + 60) / 60) * 100));

function renderMeters(el, meters) {
  if (!meters || !meters.length) { el.innerHTML = ''; return; }
  if (el.children.length !== meters.length) {
    el.innerHTML = meters.map(() => '<div class="meter"><i></i></div>').join('');
  }
  meters.forEach((m, i) => {
    const bar = el.children[i];
    bar.classList.toggle('over', !!m.over);
    bar.firstElementChild.style.height = meterHeight(m.peak_dbfs) + '%';
  });
}

/* --- players --------------------------------------------------------------- */

function playerCard(p) {
  const el = document.createElement('div');
  el.className = 'panel';
  el.id = 'player-' + p.name;
  el.innerHTML = `
    <div class="card-head">
      <span class="name">Player · ${p.name}</span>
      <span class="pill" data-state></span>
    </div>
    <div class="card-body">
      <div>
        <div class="monitor">
          <img src="/api/preview/player/${encodeURIComponent(p.name)}/mjpeg" alt="">
        </div>
        <div class="meters" data-meters></div>
      </div>
      <div>
        <dl class="kv">
          <dt>Format</dt><dd data-format></dd>
          <dt>Video</dt><dd data-vdest></dd>
          <dt>Audio</dt><dd data-adest></dd>
          <dt>Datei</dt><dd data-file></dd>
          <dt>Puffer</dt><dd data-buffer></dd>
          <dt>Frames</dt><dd data-frames></dd>
          <dt>Sync</dt><dd data-sync></dd>
        </dl>
        <progress data-progress max="100" value="0"></progress>
        <div data-times class="kv" style="grid-template-columns:auto auto;justify-content:space-between"></div>
        <div class="controls">
          <button class="primary" data-act="play">▶ Play</button>
          <button data-act="pause">⏸ Pause</button>
          <button data-act="stop">■ Stop</button>
          <button data-act="previous">⏮</button>
          <button data-act="next">⏭</button>
          <button data-act="loop">⟲ Loop</button>
        </div>
        <ul class="playlist" data-playlist></ul>
      </div>
    </div>`;

  el.querySelectorAll('button[data-act]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const act = btn.dataset.act;
      try {
        if (act === 'loop') {
          await api('POST', `/api/player/${p.name}/loop`, { enabled: !state.loop[p.name] });
        } else {
          await api('POST', `/api/player/${p.name}/${act}`);
        }
        refresh();
      } catch (e) { alert(e.message); }
    });
  });
  return el;
}

function updatePlayer(el, p) {
  state.loop[p.name] = p.loop;
  const pill = el.querySelector('[data-state]');
  pill.textContent = p.state;
  pill.className = 'pill ' + (p.state === 'playing' ? 'ok' : p.state === 'paused' ? 'warn' : '');

  el.querySelector('[data-format]').textContent = `${p.format}, ${p.channels} ch`;
  el.querySelector('[data-vdest]').textContent = p.video_dest;
  el.querySelector('[data-adest]').textContent = p.audio_dest;
  el.querySelector('[data-file]').textContent = p.file ? p.file.split('/').pop() : '—';
  el.querySelector('[data-buffer]').textContent =
    `${p.video_queue}/${p.video_queue_capacity} Frames · ${p.audio_queue_ms} ms Audio`;
  el.querySelector('[data-frames]').textContent =
    `${p.frames_sent} gesendet, ${p.frames_repeated} wiederholt, ${p.audio_underruns} Audio-Underruns`;

  el.querySelector('[data-sync]').textContent =
    (p.av_skew_ms === null ? 'A/V-Versatz —' : `A/V-Versatz ${p.av_skew_ms.toFixed(1)} ms`) +
    ` · ${p.video_epochs_skipped} Epochen ausgelassen · ${p.audio_gap_ms} ms Audiolücke`;

  const prog = el.querySelector('[data-progress]');
  prog.value = p.duration_s ? (p.position_s / p.duration_s) * 100 : 0;
  el.querySelector('[data-times]').innerHTML =
    `<dd>${fmtTime(p.position_s)}</dd><dd>${fmtTime(p.duration_s)}</dd>`;

  el.querySelector('[data-act="loop"]').classList.toggle('on', p.loop);
  el.querySelector('[data-playlist]').innerHTML = p.playlist
    .map((it, i) => `<li class="${i === p.index ? 'current' : ''}">${i + 1}. ${it.label}</li>`)
    .join('');
  renderMeters(el.querySelector('[data-meters]'), p.meters);
}

/* --- recorders ------------------------------------------------------------- */

function recorderCard(r) {
  const el = document.createElement('div');
  el.className = 'panel';
  el.id = 'recorder-' + r.name;
  el.innerHTML = `
    <div class="card-head">
      <span class="name">Recorder · ${r.name}</span>
      <span class="pill" data-signal></span>
    </div>
    <div class="card-body">
      <div>
        <div class="monitor">
          <img src="/api/preview/recorder/${encodeURIComponent(r.name)}/mjpeg" alt="">
          <div class="nosignal" data-nosignal hidden>kein Signal</div>
        </div>
        <div class="meters" data-meters></div>
      </div>
      <div>
        <dl class="kv">
          <dt>Erkannt</dt><dd data-detected></dd>
          <dt>Video</dt><dd data-vsrc></dd>
          <dt>Audio</dt><dd data-asrc></dd>
          <dt>Empfangen</dt><dd data-received></dd>
          <dt>Codec</dt><dd data-codec></dd>
          <dt>Aufnahme</dt><dd data-recinfo></dd>
        </dl>
        <div class="controls">
          <button class="danger" data-act="record">● Aufnahme</button>
          <button data-act="stop">■ Stop</button>
        </div>
        <div data-error style="color:var(--bad);font-size:12px"></div>
      </div>
    </div>`;

  el.querySelector('[data-act="record"]').addEventListener('click', async () => {
    try { await api('POST', `/api/recorder/${r.name}/start`, {}); refresh(); }
    catch (e) { alert(e.message); }
  });
  el.querySelector('[data-act="stop"]').addEventListener('click', async () => {
    try { await api('POST', `/api/recorder/${r.name}/stop`); refresh(); }
    catch (e) { alert(e.message); }
  });
  return el;
}

function updateRecorder(el, r) {
  const pill = el.querySelector('[data-signal]');
  if (r.recording) { pill.textContent = 'REC'; pill.className = 'pill bad'; }
  else if (r.signal_present) { pill.textContent = 'Signal'; pill.className = 'pill ok'; }
  else { pill.textContent = 'kein Signal'; pill.className = 'pill warn'; }

  el.querySelector('[data-nosignal]').hidden = r.signal_present;
  el.querySelector('[data-detected]').textContent = r.detected_format || '—';
  el.querySelector('[data-vsrc]').textContent = r.video_source;
  el.querySelector('[data-asrc]').textContent = r.audio_source;
  el.querySelector('[data-received]').textContent =
    `${r.frames_received} Frames, ${r.frames_incomplete} unvollständig, ${r.frames_dropped} verworfen` +
    (r.audio_gap_ms ? `, ${r.audio_gap_ms} ms Audiolücke` : '');
  el.querySelector('[data-codec]').textContent = r.codec;
  el.querySelector('[data-recinfo]').textContent = r.recording
    ? `${r.recording_path.split('/').pop()} · ${fmtTime(r.recorded_seconds)} · ${r.recorded_mib.toFixed(0)} MiB`
    : '—';
  el.querySelector('[data-error]').textContent = r.last_error || '';
  renderMeters(el.querySelector('[data-meters]'), r.meters);
}

/* --- media ----------------------------------------------------------------- */

async function loadMedia() {
  const files = await api('GET', '/api/media');
  const players = state.status ? state.status.players : [];
  $('#media-list').innerHTML = files.map((f) => `
    <tr class="${f.playable ? '' : 'bad'}">
      <td>${f.name}</td>
      <td>${f.playable ? f.format : (f.note || 'nicht abspielbar')}</td>
      <td>${f.playable ? fmtTime(f.duration_s) : ''}</td>
      <td>${fmtSize(f.size_bytes)}</td>
      <td>${f.playable && players.length
        ? `<button data-load="${encodeURIComponent(f.name)}">laden</button>` : ''}</td>
    </tr>`).join('');

  $('#media-list').querySelectorAll('button[data-load]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const target = state.status.players[0].name;
      try {
        await api('POST', `/api/player/${target}/load`,
                  { file: decodeURIComponent(btn.dataset.load) });
        refresh();
      } catch (e) { alert(e.message); }
    });
  });

  const recs = await api('GET', '/api/recordings');
  $('#recording-list').innerHTML = recs
    .map((f) => `<tr><td>${f.name}</td><td>${fmtSize(f.size_bytes)}</td></tr>`).join('');
}

/* --- main loop ------------------------------------------------------------- */

const state = { status: null, loop: {} };

async function refresh() {
  let s;
  try {
    s = await api('GET', '/api/status');
  } catch (e) {
    $('#pill-device').textContent = 'nicht erreichbar';
    $('#pill-device').className = 'pill bad';
    return;
  }
  state.status = s;

  const d = s.device;
  const dp = $('#pill-device');
  dp.textContent = d.up ? `${d.port}${d.redundant ? ' (2022-7)' : ''}` : 'Device down';
  dp.className = 'pill ' + (d.up ? 'ok' : 'bad');

  const pp = $('#pill-ptp');
  pp.textContent = d.ptp_locked
    ? `PTP locked${d.ptp_have_offset ? ' · ' + (d.ptp_offset_ns / 1000).toFixed(1) + ' µs' : ''}`
    : 'PTP unlocked';
  pp.className = 'pill ' + (d.ptp_locked ? 'ok' : 'bad');
  pp.title = d.clock_source;

  const np = $('#pill-nmos');
  np.textContent = d.nmos_up ? 'NMOS' : 'NMOS aus';
  np.className = 'pill ' + (d.nmos_up ? 'ok' : '');
  $('#tai').textContent = d.tai_utc ? d.tai_utc + ' UTC' : '';

  syncCards($('#players'), s.players, playerCard, updatePlayer, 'player-');
  syncCards($('#recorders'), s.recorders, recorderCard, updateRecorder, 'recorder-');
}

function syncCards(container, items, create, update, prefix) {
  items.forEach((item) => {
    let el = document.getElementById(prefix + item.name);
    if (!el) { el = create(item); container.appendChild(el); }
    update(el, item);
  });
}

async function loadLog() {
  try {
    const lines = await api('GET', '/api/log?lines=120');
    const el = $('#log');
    const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
    el.textContent = lines.join('\n');
    if (atBottom) el.scrollTop = el.scrollHeight;
  } catch (e) { /* ignore */ }
}

refresh().then(loadMedia);
loadLog();
setInterval(refresh, 500);
setInterval(loadMedia, 10000);
setInterval(loadLog, 2000);
