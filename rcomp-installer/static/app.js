'use strict';
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

async function api(path, body) {
  const opt = body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const data = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw Object.assign(new Error(data.error || `HTTP ${r.status}`), { data });
  return data;
}

// ---- tabs -----------------------------------------------------------------------------------------
function showTab(name) {
  document.querySelectorAll('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + name));
  if (name === 'console') refreshConsole();
  try { localStorage.setItem('rci-tab', name); } catch (e) { /* storage unavailable */ }
}
document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));

// ---- path picker ----------------------------------------------------------------------------------
const picker = { target: null, mode: 'dir', path: '', selected: null };
async function pkLoad(path) {
  const d = await api('/api/fs?path=' + encodeURIComponent(path));
  picker.path = d.path; picker.parent = d.parent; picker.selected = null;
  $('pk-path').value = d.path;
  const ul = $('pk-list'); ul.innerHTML = '';
  if (d.error) ul.innerHTML = `<li>${esc(d.error)}</li>`;
  for (const e of d.entries) {
    if (picker.mode === 'dir' && !e.dir) continue;
    const li = document.createElement('li');
    li.className = e.dir ? 'dir' : '';
    li.innerHTML = `<span>${esc(e.name)}</span><span class="sz">${e.dir ? '' : fmtSize(e.size)}</span>`;
    const full = d.path ? joinPath(d.path, e.name) : e.name;
    li.onclick = () => {
      if (e.dir) return pkLoad(full);
      ul.querySelectorAll('li').forEach((x) => x.classList.remove('sel'));
      li.classList.add('sel'); picker.selected = full; pkHint();
    };
    li.ondblclick = () => { if (!e.dir) { picker.selected = full; pkChoose(); } };
    ul.appendChild(li);
  }
  pkHint();
}
function pkHint() {
  $('pk-choose').textContent = picker.mode === 'dir' ? 'Choose this folder' : 'Choose file';
  $('pk-choose').disabled = picker.mode === 'file' ? !picker.selected : !picker.path;
  $('pk-hint').textContent = picker.mode === 'file' ? (picker.selected || 'select a file') : picker.path;
}
function joinPath(dir, name) { return dir.endsWith('\\') || dir.endsWith('/') ? dir + name : dir + (dir.includes('\\') ? '\\' : '/') + name; }
function parentOf(p) { const m = p.replace(/[\\/]+$/, '').match(/^(.*)[\\/][^\\/]*$/); return m ? (m[1].match(/^[A-Za-z]:$/) ? m[1] + '\\' : m[1]) : ''; }
function pkChoose() {
  const v = picker.mode === 'file' ? picker.selected : picker.path;
  if (!v) return;
  const input = $(picker.target); input.value = v; input.dispatchEvent(new Event('change'));
  $('picker').close();
}
document.querySelectorAll('[data-pick]').forEach((b) => b.addEventListener('click', (ev) => {
  ev.preventDefault();
  picker.target = b.dataset.pick; picker.mode = b.dataset.mode;
  const cur = $(picker.target).value.trim();
  $('picker').showModal();
  pkLoad(cur ? (picker.mode === 'file' ? parentOf(cur) : cur) : '').catch(() => pkLoad(''));
}));
$('pk-up').onclick = () => pkLoad(picker.parent ?? '');
$('pk-go').onclick = () => pkLoad($('pk-path').value);
$('pk-path').addEventListener('keydown', (e) => { if (e.key === 'Enter') pkLoad($('pk-path').value); });
$('pk-cancel').onclick = () => $('picker').close();
$('pk-choose').onclick = pkChoose;

function fmtSize(n) {
  if (n == null) return '';
  const u = ['B', 'KiB', 'MiB', 'GiB', 'TiB']; let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

// ---- jobs and live log ----------------------------------------------------------------------------
const jobs = { current: null, source: null, onEnd: {} };
function levelClass(l) { return l === 'NOT TESTED' ? 'NT' : l; }
function appendLine(line) {
  const log = $('log');
  const span = document.createElement('span');
  span.className = levelClass(line.level);
  const t = new Date(line.t * 1000).toLocaleTimeString();
  const tag = ['PASS', 'FAIL', 'BLOCKED', 'NOT TESTED', 'WARN'].includes(line.level) ? `${line.level}: ` : line.level === 'STEP' ? '▶ ' : '';
  span.textContent = `${t}  ${tag}${line.text}\n`;
  log.appendChild(span);
  while (log.childNodes.length > 20000) log.removeChild(log.firstChild);
  if ($('log-follow').checked) log.scrollTop = log.scrollHeight;
}
function setStatus(status, summary) {
  const b = $('job-status');
  b.textContent = status; b.className = 'badge ' + (status === 'NOT TESTED' ? 'NT' : status);
  $('job-summary').textContent = summary || '';
  $('job-cancel').classList.toggle('hidden', status !== 'RUNNING');
}
function watchJob(id, onEnd) {
  if (onEnd) jobs.onEnd[id] = onEnd;
  if (jobs.source) jobs.source.close();
  jobs.current = id;
  $('log').innerHTML = '';
  setStatus('RUNNING', '');
  const es = new EventSource(`/api/jobs/${id}/events?since=0`);
  jobs.source = es;
  es.addEventListener('line', (e) => appendLine(JSON.parse(e.data)));
  es.addEventListener('end', (e) => {
    const v = JSON.parse(e.data);
    es.close(); jobs.source = null;
    setStatus(v.status, v.summary);
    refreshJobList();
    const cb = jobs.onEnd[id]; delete jobs.onEnd[id];
    if (cb) cb(v);
  });
  es.onerror = () => { /* the browser retries; 'end' closes it */ };
  refreshJobList();
}
async function refreshJobList() {
  const list = await api('/api/jobs').catch(() => []);
  const sel = $('job-select');
  sel.innerHTML = list.length ? '' : '<option>no job yet</option>';
  for (const j of list) {
    const o = document.createElement('option');
    o.value = j.id; o.textContent = `#${j.id} ${j.title} — ${j.status}`;
    if (j.id === jobs.current) o.selected = true;
    sel.appendChild(o);
  }
}
$('job-select').onchange = () => { const id = parseInt($('job-select').value, 10); if (id) watchJob(id); };
$('job-cancel').onclick = () => { if (jobs.current) api(`/api/jobs/${jobs.current}/cancel`, {}); };
async function startJob(path, body, onEnd) {
  try {
    const r = await api(path, body);
    watchJob(r.job, onEnd);
  } catch (e) {
    alert(e.message);
  }
}

// ---- check a game ------------------------------------------------------------------------------------
async function loadChecks(select) {
  const list = await api('/api/checks').catch(() => []);
  const sel = $('k-history');
  sel.innerHTML = '<option value="">Previous checks…</option>' + list.map((c) =>
    `<option value="${esc(c.check)}">${esc(c.checked)} · ${esc(c.name)} · ${esc(c.verdict)}</option>`).join('');
  if (select) { sel.value = select; showCheck(select); }
  loadBacklog();
}
async function loadBacklog() {
  const b = await api('/api/backlog').catch(() => null);
  const box = $('k-backlog');
  if (!b || !b.games.length) { box.innerHTML = '<p class="note">No game checked yet.</p>'; return; }
  const games = b.games.map((g) => {
    const t = g.last_try;
    return `<tr><td><a href="#" data-check="${esc(g.check)}">${esc(g.name)}</a></td><td>${esc(g.verdict)}</td>
      <td>${g.missing_functions}</td><td>${t ? `<b>${esc(t.result)}</b>: ${esc(t.reason)}` : 'not tried'}</td></tr>`;
  }).join('');
  const top = b.functions.slice(0, 25).map((f) => `<tr><td><code>${esc(f.name)}</code></td><td>${esc(f.category)}</td>
      <td>${f.hit_on_ps5.length ? `<b>called on the PS5 by ${esc(f.hit_on_ps5.join(', '))}</b>` : ''}</td>
      <td>${f.games.length}</td></tr>`).join('');
  box.innerHTML = `<h3>${b.games.length} game(s)</h3>
    <table class="grid"><tr><th>Game</th><th>Check</th><th>Missing</th><th>Last PS5 try</th></tr>${games}</table>
    <h3>Most needed functions (${b.functions.length} in all)</h3>
    <table class="grid"><tr><th>Function</th><th>Family</th><th>Seen on the PS5</th><th>Games</th></tr>${top}</table>`;
  box.querySelectorAll('a[data-check]').forEach((a) => { a.onclick = (e) => {
    e.preventDefault(); $('k-history').value = a.dataset.check; showCheck(a.dataset.check); }; });
}
$('k-all').onclick = () => startJob('/api/jobs/check-all', { folder: $('k-isos').value.trim(), force: $('k-force').checked },
  () => loadChecks());
$('k-export').onclick = () => { window.location.href = '/api/backlog/export'; };
async function showCheck(id) {
  if (!id) return;
  const r = await api('/api/checks/' + encodeURIComponent(id)).catch((e) => ({ error: e.message }));
  const box = $('k-report');
  if (r.error) { box.innerHTML = `<span class="note warn">${esc(r.error)}</span>`; box.classList.remove('hidden'); return; }
  const badge = r.verdict === 'TRY IT' ? 'TRY' : r.verdict;
  const cats = Object.entries(r.missing_by_category || {}).map(([cat, names]) =>
    `<details><summary>${esc(cat)} — ${names.length}</summary><ul>${names.map((n) => `<li><code>${esc(n)}</code></li>`).join('')}</ul></details>`).join('');
  const services = Object.entries(r.services || {}).map(([m, s]) => `<li><code>${esc(m)}</code>: ${s.implemented} of ${s.total} functions implemented</li>`).join('');
  box.innerHTML = `
    <div class="verdict"><span class="badge ${badge}">${esc(r.verdict)}</span><h2 style="margin:0">${esc(r.name)}</h2></div>
    <p>${esc(r.verdict_text)}.</p>
    <dl class="facts"><dt>Xbox title / media</dt><dd>${esc(r.title_id)} / ${esc(r.media_id)} (disc ${esc(r.disc)})</dd>
      <dt>Recompiled code</dt><dd>${esc(r.functions)} functions</dd>
      <dt>Kernel</dt><dd>${esc(r.kernel.status)} — ${esc(r.kernel.reason || '')}</dd>
      <dt>Build profile</dt><dd>${esc(r.profile_label)} (${esc(r.profile_reason)})</dd>
      <dt>Executables on the disc</dt><dd>${esc((r.modules || []).join(', '))}</dd></dl>
    ${r.blockers.length ? `<h3>Blockers</h3><ul>${r.blockers.map((b) => `<li>${esc(b)}</li>`).join('')}</ul>` : ''}
    <h3>Missing console functions: ${r.missing_functions}</h3>${cats || '<p>None.</p>'}
    <h3>Implemented today</h3><ul>${services}</ul>
    ${(r.tries || []).length ? `<h3>PS5 results</h3><ul>${r.tries.map((t) => `<li>${esc(t.when)} (${esc(t.how || '')}):
      <b>${esc(t.result)}</b>: ${esc(t.reason)}${(t.evidence || []).length ? `<pre>${esc(t.evidence.join('\n'))}</pre>` : ''}</li>`).join('')}</ul>` : ''}
    <div class="actions">
      <button class="primary" id="k-try" ${r.verdict === 'BLOCKED' ? 'disabled' : ''}>Try it on my PS5</button>
      <button id="k-record" title="After playing it from the PS5 home screen">Record the last run</button>
      <button id="k-order">Open work order</button>
      <span class="note">${esc(r.work_order_path)}</span>
    </div>`;
  box.classList.remove('hidden');
  $('k-order').onclick = () => window.open('/api/checks/' + encodeURIComponent(id) + '/work-order', '_blank');
  $('k-try').onclick = () => startJob('/api/jobs/try-game', { check: id }, () => { refreshConsole(); loadChecks(id); });
  $('k-record').onclick = () => startJob('/api/jobs/record-run', { check: id }, () => loadChecks(id));
}
$('k-go').onclick = () => {
  const source = $('k-src').value.trim();
  if (!source) return alert('Choose a disc image or an extracted disc folder first.');
  startJob('/api/jobs/check-game', { source }, (v) => { if (v.result && v.result.check) loadChecks(v.result.check); });
};
$('k-history').onchange = () => showCheck($('k-history').value);

// ---- console state (also used for title id checks) ------------------------------------------------
let consoleTitles = null;
async function loadTitles() {
  try { consoleTitles = (await api('/api/console/titles')).titles; } catch (e) { consoleTitles = null; }
  return consoleTitles;
}
function nextFreeId() {
  if (!consoleTitles) return '';
  const used = new Set(consoleTitles.map((t) => t.id));
  for (let n = 88370; n < 89999; n++) if (!used.has('PPSA' + n)) return 'PPSA' + n;
  return '';
}
function checkTid(inputId, noteId, expectedName) {
  const v = $(inputId).value.trim().toUpperCase();
  const note = $(noteId);
  if (!v) { note.textContent = ''; return; }
  if (!/^PPSA\d{5}$/.test(v)) { note.className = 'note warn'; note.textContent = 'PPSA + 5 digits'; return; }
  if (!consoleTitles) { note.className = 'note'; note.textContent = 'console not reachable: uniqueness NOT TESTED'; return; }
  const t = consoleTitles.find((x) => x.id === v);
  if (!t) { note.className = 'note ok'; note.textContent = 'free on the console'; return; }
  const same = expectedName && t.name === expectedName;
  note.className = same ? 'note ok' : 'note warn';
  note.textContent = same ? `already installed as "${t.name}" (update)` : `taken on the console by "${t.name || t.id}" — pick another id`;
}

// ---- 1. install -----------------------------------------------------------------------------------
async function inspectApp() {
  const p = $('i-app').value.trim(); const box = $('i-preview');
  if (!p) return box.classList.add('hidden');
  try {
    const a = await api('/api/app/inspect', { path: p });
    box.innerHTML = `<img src="/api/app/icon?path=${encodeURIComponent(p)}&t=${Date.now()}" alt="">
      <dl><dt>Title id</dt><dd>${esc(a.title_id)}</dd><dt>Name</dt><dd>${esc(a.name)}</dd>
      <dt>Files</dt><dd>${a.files} (${fmtSize(a.bytes)})${a.has_manifest ? ', manifest.sha256 present' : ', no manifest.sha256'}</dd>
      <dt>Console</dt><dd id="i-console-note">…</dd></dl>`;
    box.classList.remove('hidden');
    await loadTitles();
    const t = consoleTitles && consoleTitles.find((x) => x.id === a.title_id);
    $('i-console-note').textContent = !consoleTitles ? 'not reachable' : !t ? 'id free' :
      `${t.folder ? 'folder present' : 'no folder'}; ${t.name ? '"' + t.name + '"' : ''} ${t.registered ? '(registered)' : '(not registered)'}`;
  } catch (e) {
    box.innerHTML = `<span class="note warn">${esc(e.message)}</span>`; box.classList.remove('hidden');
  }
}
$('i-app').addEventListener('change', inspectApp);
$('i-go').onclick = () => startJob('/api/jobs/install', {
  app: $('i-app').value.trim(), overwrite: $('i-overwrite').checked, register: $('i-register').checked,
});

// ---- 2. package -----------------------------------------------------------------------------------
let xexInfo = null;
async function readXex() {
  const p = $('p-xex').value.trim(); const box = $('p-xexinfo');
  if (!p) return box.classList.add('hidden');
  try {
    xexInfo = await api('/api/xex/info', { path: p });
    const img = xexInfo.icon_png_bytes ? `<img src="/api/xex/icon?path=${encodeURIComponent(p)}" alt="">` : '<img alt="">';
    const names = Object.entries(xexInfo.names || {}).map(([k, v]) => `${k}: ${v}`).join(' · ');
    box.innerHTML = `${img}<dl><dt>Xbox title id</dt><dd>${esc(xexInfo.title_id)} (media ${esc(xexInfo.media_id)}, disc ${xexInfo.disc_number}/${xexInfo.disc_count})</dd>
      <dt>Name</dt><dd>${esc(xexInfo.name || '(none in XDBF)')}</dd><dt>Languages</dt><dd>${esc(names || '-')}</dd>
      <dt>Plain image</dt><dd>${xexInfo.plain ? 'yes' : 'NO — encrypted or compressed'}</dd>
      ${xexInfo.warnings.length ? `<dt>Warnings</dt><dd>${esc(xexInfo.warnings.join('; '))}</dd>` : ''}
      <dt>x360db</dt><dd><button id="p-x360db">Look up name</button></dd></dl>`;
    box.classList.remove('hidden');
    if (!$('p-name').value && xexInfo.name) $('p-name').value = xexInfo.name;
    $('p-x360db').onclick = async (ev) => {
      ev.preventDefault();
      const r = await api('/api/x360db', { title_id: xexInfo.title_id }).catch((e) => ({ error: e.message }));
      const title = r.info && (r.info.title?.full || r.info.title || r.info.name);
      if (title && typeof title === 'string') { $('p-name').value = title; } else { alert(r.error || 'no name in x360db'); }
    };
    await loadTitles();
    if (!$('p-tid').value) $('p-tid').value = nextFreeId();
    checkTid('p-tid', 'p-tid-note', $('p-name').value);
  } catch (e) {
    box.innerHTML = `<span class="note warn">${esc(e.message)}</span>`; box.classList.remove('hidden');
  }
}
$('p-xex').addEventListener('change', readXex);
$('p-tid').addEventListener('input', () => checkTid('p-tid', 'p-tid-note', $('p-name').value));
let lastApp = null;
$('p-go').onclick = () => startJob('/api/jobs/package', {
  eboot: $('p-eboot').value.trim(), xex: $('p-xex').value.trim(), game: $('p-game').value.trim(),
  libc: $('p-libc').value.trim(), out: $('p-out').value.trim(), title_id: $('p-tid').value.trim(),
  name: $('p-name').value.trim(), icon: $('p-icon').value.trim(), replace: $('p-replace').checked,
}, (v) => { if (v.status === 'PASS') { lastApp = v.result.app; $('p-install').classList.remove('hidden'); } });
$('p-install').onclick = () => { $('i-app').value = lastApp; showTab('install'); inspectApp(); };

// ---- 3. recompile ---------------------------------------------------------------------------------
let lastRun = '';
function recompile(resume) {
  $('r-resume').classList.add('hidden');
  startJob('/api/jobs/recompile', {
    source: $('r-src').value.trim(), title_id: $('r-tid').value.trim(), name: $('r-name').value.trim(),
    scale: $('r-scale').value, jobs: $('r-jobs').value, out: $('r-out').value.trim(), exploratory: $('r-exploratory').checked,
    replace: $('r-replace').checked, resume, profile: $('r-profile').value,
  }, (v) => {
    if (v.status === 'PASS') { lastApp = v.result.app; $('r-install').classList.remove('hidden'); }
    else if (v.result && v.result.run) { lastRun = v.result.run; $('r-resume').textContent = `Resume ${lastRun}`; $('r-resume').classList.remove('hidden'); }
  });
}
$('r-go').onclick = () => recompile('');
$('r-resume').onclick = () => recompile(lastRun);
$('r-install').onclick = () => { $('i-app').value = lastApp; showTab('install'); inspectApp(); };
$('r-tid').addEventListener('input', () => checkTid('r-tid', 'r-tid-note', ''));
$('r-tools').onclick = async () => {
  const r = await api('/api/tools');
  const rows = Object.entries(r.tools).map(([k, v]) => `<tr><td>${esc(k)}</td><td><span class="badge ${v.status}">${v.status}</span></td><td class="mono">${esc(v.path)}${v.hint ? `<div class="note warn">${esc(v.hint)}</div>` : ''}</td></tr>`);
  rows.push(`<tr><td>chmod payload</td><td><span class="badge ${r.payload.chmod_built ? 'PASS' : r.payload.sdk ? 'NT' : 'BLOCKED'}">${r.payload.chmod_built ? 'PASS' : r.payload.sdk ? 'NOT BUILT' : 'BLOCKED'}</span></td><td class="mono">${esc(r.payload.chmod_elf)}</td></tr>`);
  $('r-toolreport').innerHTML = `<table class="tooltable"><tbody>${rows.join('')}</tbody></table>`;
  $('r-toolreport').classList.remove('hidden');
};

let PROFILES = {};
async function loadProfiles() {
  PROFILES = await api('/api/profiles').catch(() => ({}));
  const sel = $('r-profile');
  for (const [k, p] of Object.entries(PROFILES)) {
    const o = document.createElement('option'); o.value = k; o.textContent = p.label; sel.appendChild(o);
  }
  const note = () => { const p = PROFILES[sel.value]; $('r-profile-note').textContent = p ? 'measured: ' + p.measured : 'GTA IV / Episodes from Liberty City discs get their tuned profile'; };
  sel.onchange = note; note();
}

// ---- console tab ----------------------------------------------------------------------------------
async function refreshConsole() {
  const st = await api('/api/console/status').catch((e) => ({ error: e.message }));
  pill(st);
  $('c-status').innerHTML = `<div>host <b>${esc(st.host)}</b></div><div>FTP <b>${esc(st.ftp || '?')}</b></div>
    <div>ps5vkctl <b>${st.ctl ? 'up' : 'not reachable'}</b></div><div>procs <b>${esc(st.procs || '-')}</b></div>`;
  const tb = $('c-titles').querySelector('tbody');
  tb.innerHTML = '<tr><td colspan="7">…</td></tr>';
  const titles = await loadTitles();
  if (!titles) { tb.innerHTML = '<tr><td colspan="7">console not reachable over FTP</td></tr>'; return; }
  const yes = (b, text) => `<span class="${b ? 'yes' : 'no'}">${b ? (text || 'yes') : 'no'}</span>`;
  const shown = titles.filter((t) => !t.system_only);
  const hidden = titles.length - shown.length;
  tb.innerHTML = shown.map((t) => `<tr><td class="mono">${esc(t.id)}</td><td>${esc(t.name || '')}</td>
    <td class="mono">${esc(t.folder || t.status_path || '')}</td><td>${yes(t.listed)}</td><td>${yes(t.status, esc(t.status))}</td>
    <td>${yes(t.registered)}</td><td>
    ${t.folder && !t.registered && /^PPSA\d{5}$/.test(t.id) ? `<button data-act="register" data-id="${esc(t.id)}">Register</button>` : ''}
    ${t.registered ? `<button data-act="launch" data-id="${esc(t.id)}">Launch</button><button data-act="kill" data-id="${esc(t.id)}">Close</button>` : ''}
    </td></tr>`).join('') + (hidden ? `<tr><td colspan="7" class="no">${hidden} other apps in /system_ex/app (system or installed by other means) are hidden; their ids are never offered.</td></tr>` : '');
  tb.querySelectorAll('button').forEach((b) => b.onclick = () => startJob(`/api/jobs/${b.dataset.act}`, { title_id: b.dataset.id },
    () => refreshConsole()));
}
function pill(st) {
  const p = $('console-pill');
  if (st.ftp === 'PASS') { p.textContent = `console ${st.host}: FTP up${st.ctl ? ', ps5vkctl up' : ''}`; p.className = 'pill ok'; }
  else { p.textContent = `console ${st.host || ''}: not reachable`; p.className = 'pill bad'; }
}
$('c-refresh').onclick = refreshConsole;
$('c-ctl').onclick = () => startJob('/api/jobs/load-ctl', {}, () => refreshConsole());
$('c-klog').onclick = () => startJob('/api/jobs/klog', { seconds: 30 });

// ---- settings -------------------------------------------------------------------------------------
const SETTING_LABELS = {
  ps5_host: 'PS5 address', ftp_port: 'FTP port', loader_port: 'ELF loader port', ctl_port: 'ps5vkctl port',
  klog_port: 'Kernel log port', rcomp_root: 'R-comp checkout', deps_root: 'Reference deps (never written)',
  cygwin_bash: 'Cygwin bash.exe (empty: R-comp\'s full Cygwin)', payload_sdk: 'ps5-payload SDK (empty: R-comp\'s)',
  output_dir: 'Default output folder', work_dir: 'Recompile work folder', registration_wait: 'ShadowMount+ wait (s)',
  ftp_connections: 'Parallel FTP connections', isos_dir: 'Disc images folder (Check all)',
  watch_seconds: 'Try: watch the game for (s)',
};
async function loadSettings() {
  const s = await api('/api/settings');
  $('s-form').innerHTML = Object.keys(SETTING_LABELS).map((k) =>
    `<div class="field"><label>${esc(SETTING_LABELS[k])}</label><input name="${k}" value="${esc(s[k])}"></div>`).join('');
  if (!$('p-out').value) $('p-out').value = s.output_dir;
  if (!$('r-out').value) $('r-out').value = s.output_dir;
  if (!$('p-libc').value) { const g = await api('/api/guess-libc', {}); $('p-libc').value = g.path || ''; }
}
$('s-save').onclick = async () => {
  const body = {};
  new FormData($('s-form')).forEach((v, k) => { body[k] = v; });
  try { await api('/api/settings', body); await loadSettings(); refreshConsole(); } catch (e) { alert(e.message); }
};
$('s-payload').onclick = () => startJob('/api/jobs/build-payload', {});

// ---- boot -----------------------------------------------------------------------------------------
(async () => {
  let tab = 'check';
  try { tab = localStorage.getItem('rci-tab') || tab; } catch (e) { /* storage unavailable */ }
  showTab(tab);
  await loadSettings().catch(() => {});
  loadChecks();
  loadProfiles();
  api('/api/console/status').then(pill).catch(() => {});
  const list = await api('/api/jobs').catch(() => []);
  if (list.length) watchJob(list[0].id); else refreshJobList();
})();
