'use strict';

const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));

// Barcha dinamik qiymatlar HTML ga faqat shu orqali qo'yiladi (XSS himoyasi)
const esc = v => String(v ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const fmtB = b => {
  b = Number(b) || 0;
  if (b <= 0) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
  let i = 0;
  while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
  return (i === 0 ? b : b.toFixed(b >= 100 ? 0 : b >= 10 ? 1 : 2)) + ' ' + u[i];
};
const fmtT = ts => ts ? new Date(ts * 1000).toLocaleString() : '–';
const fmtDate = ts => ts ? new Date(ts * 1000).toLocaleDateString() : '–';
const fmtUSD = cents => {
  const usd = (Number(cents) || 0) / 100;
  if (usd > 0 && usd < 0.01) return '$' + usd.toFixed(6);
  return '$' + usd.toFixed(2);
};
const fmtDur = sec => {
  sec = Number(sec) || 0;
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
  return h > 0 ? `${h} soat ${m} daq` : `${m} daq`;
};
const ago = (ts, now) => {
  if (!ts) return '–';
  const d = Math.max(0, (now || Date.now() / 1000) - ts);
  if (d < 60) return `${Math.floor(d)} s oldin`;
  if (d < 3600) return `${Math.floor(d / 60)} daq oldin`;
  if (d < 86400) return `${Math.floor(d / 3600)} soat oldin`;
  return `${Math.floor(d / 86400)} kun oldin`;
};
const badge = (kind, text) => `<span class="badge"><span class="dot dot-${kind}"></span>${esc(text)}</span>`;
const emptyRow = (cols, text) => `<tr><td colspan="${cols}" class="empty">${esc(text)}</td></tr>`;

const ADMIN_TABS = ['nodes', 'files', 'tasks', 'payouts', 'compute', 'sim'];
const UZS_PER_USD = 12800;

const state = { admin: false, info: null, dash: null, tab: 'overview' };

// ---------------- API ----------------

async function api(method, url, body) {
  const headers = { 'X-Requested-With': 'sdrive' };
  let payload;
  if (body !== undefined) {
    payload = JSON.stringify(body);
    headers['Content-Type'] = 'application/json';
  }
  const r = await fetch(url, { method, headers, body: payload, credentials: 'same-origin' });
  let data = {};
  try { data = await r.json(); } catch (e) { /* JSON bo'lmagan javob */ }
  if (r.status === 401 && url !== '/api/admin/login') setAdmin(false);
  if (!r.ok) {
    const err = new Error(data.error || ('HTTP ' + r.status));
    err.status = r.status;
    err.data = data;
    throw err;
  }
  return data;
}

function flash(el, text, ok) {
  el.textContent = text;
  el.classList.toggle('msg-ok', !!ok);
  el.classList.toggle('msg-err', ok === false);
}

// ---------------- Navigatsiya ----------------

function setAdmin(v) {
  const changed = state.admin !== v;
  state.admin = v;
  $('#adminBadge').classList.toggle('hidden', !v);
  $('#btnLogout').classList.toggle('hidden', !v);
  if (changed) route();
}

function route() {
  let tab = (location.hash || '#overview').slice(1);
  let scrollTo = null;
  if (tab === 'download') { scrollTo = 'download'; tab = 'overview'; }
  if (tab !== 'overview' && !ADMIN_TABS.includes(tab)) tab = 'overview';
  state.tab = tab;
  $$('.nav-tab').forEach(a => a.classList.toggle('active', a.dataset.tab === tab));
  $$('.tab-content').forEach(s => s.classList.add('hidden'));
  if (ADMIN_TABS.includes(tab) && !state.admin) {
    $('#tab-login').classList.remove('hidden');
    $('#loginToken').focus();
  } else {
    $('#tab-' + tab).classList.remove('hidden');
  }
  if (scrollTo) document.getElementById(scrollTo).scrollIntoView({ behavior: 'smooth' });
  if (state.admin && ADMIN_TABS.includes(tab)) refreshAdmin();
}

// ---------------- Ochiq ma'lumotlar ----------------

async function loadInfo() {
  try {
    const d = await api('GET', '/api/info');
    state.info = d;
    $('#sNodes').textContent = `${d.onlineNodes} / ${d.totalNodes}`;
    $('#sSpace').textContent = fmtB(d.totalSpace);
    $('#sFiles').textContent = `${d.healthyFiles} / ${d.totalFiles}`;
    $('#sPaid').textContent = fmtUSD(d.paidCents);
    $('#netDot').className = 'dot ' + (d.onlineNodes > 0 ? 'dot-live' : 'dot-warn');
    $('#netState').textContent = d.onlineNodes > 0 ? 'Tarmoq faol' : 'Tugunlar kutilmoqda';
    $('#ratioBadge').textContent = `${Math.round(d.replicaRatio * 100)}% replikatsiya · har fayl ${d.replicaTarget} ta tugunda`;
    const pw = d.payoutWindow || {};
    $('#clusterMeta').innerHTML = [
      `Saqlangan nusxalar: <b>${esc(fmtB(d.storedBytes))}</b>`,
      `Kutilayotgan vazifalar: <b>${esc(d.pendingTasks)}</b>`,
      `Chunk hajmi: <b>${esc(fmtB(d.chunkSize))}</b>`,
      pw.open ? 'Payout oynasi: <b class="ok">ochiq</b>' : `Payout oynasi: <b>${esc(fmtDate(pw.opensAt))}</b> dan`,
    ].map(x => `<span class="meta">${x}</span>`).join('');
    $('#uploadHint').textContent = `Fayl ${fmtB(d.chunkSize)} lik chunk'larga bo'linadi va onlayn tugunlarning ${Math.round(d.replicaRatio * 100)}% iga (hozir ${d.replicaTarget} ta) nusxalanadi.`;
    updateCalc();
  } catch (e) {
    $('#netDot').className = 'dot dot-off';
    $('#netState').textContent = 'Server bilan aloqa yo\'q';
  }
}

function updateCalc() {
  const gb = parseInt($('#gbRange').value, 10);
  const hrs = parseInt($('#hrsRange').value, 10);
  const rate = state.info ? state.info.rate : 0.05; // sent / GB / soat
  $('#gbLabel').textContent = gb + ' GB';
  $('#hrsLabel').textContent = hrs + ' soat';
  const usd = gb * hrs * 30 * rate / 100;
  $('#calcUsd').textContent = '$' + usd.toFixed(2);
  $('#calcUzs').textContent = '~ ' + Math.round(usd * UZS_PER_USD).toLocaleString() + ' UZS';
  $('#rateNote').textContent = `Tarif: 1 GB × 1 soat = ${rate} sent. Internet yoki elektr uzilgan vaqt hisoblanmaydi.`;
}

const OS_ICONS = { windows: '🪟', macos: '🍎', 'linux-deb': '🐧', linux: '🐧' };
const OS_NAMES = { windows: 'Windows', macos: 'macOS', 'linux-deb': 'Linux (.deb)', linux: 'Linux' };

async function loadDownloads() {
  try {
    const d = await api('GET', '/api/downloads');
    $('#downloadGrid').innerHTML = d.downloads.map(x => {
      const head = `<div><div class="download-icon">${OS_ICONS[x.os] || '💾'}</div>
        <div class="download-name">${esc(OS_NAMES[x.os] || x.os)}</div>
        <div class="download-desc">${esc(x.label)}</div></div>`;
      if (x.available) {
        return `<a class="download-tile" href="${esc(x.url)}" download>${head}
          <span class="btn btn-primary btn-sm">Yuklab olish · ${esc(fmtB(x.size))}</span></a>`;
      }
      return `<div class="download-tile disabled">${head}
        <span class="btn btn-outline btn-sm">Hali tayyorlanmagan</span></div>`;
    }).join('');
  } catch (e) {
    $('#downloadGrid').textContent = 'Yuklab olish ro\'yxati olinmadi: ' + e.message;
  }
}

async function openAgreement() {
  $('#agreeModal').classList.add('active');
  try {
    const d = await api('GET', '/api/agreement');
    $('#agreeVer').textContent = 'v' + d.version;
    $('#modalText').textContent = d.text;
  } catch (e) {
    $('#modalText').textContent = 'Shartnoma yuklanmadi: ' + e.message;
  }
}

// ---------------- Admin ----------------

async function refreshAdmin() {
  if (!state.admin) return;
  try {
    state.dash = await api('GET', '/api/dashboard');
    renderNodes();
    renderFiles();
    renderTasks();
    renderPayouts();
    if (state.tab === 'sim') loadSim();
    if (state.tab === 'compute') loadCompute();
  } catch (e) { /* 401 bo'lsa setAdmin(false) chaqirilgan */ }
}

function nodeStatus(n) {
  if (n.status === 'uninstalled') return badge('off', 'O\'chirilgan');
  if (!n.online) return badge('off', 'Oflayn');
  if (n.userActive) return badge('warn', 'Onlayn · foydalanuvchi faol');
  return badge('live', 'Onlayn');
}

function renderNodes() {
  const d = state.dash;
  if (!d) return;
  const q = $('#searchNodes').value.trim().toLowerCase();
  const list = d.nodes.filter(n => !q || [n.id, n.name, n.os, n.platform].some(v => (v || '').toLowerCase().includes(q)));
  const online = d.nodes.filter(n => n.online).length;
  const sims = d.nodes.filter(n => n.simulated).length;
  $('#nodesSummary').textContent = `${d.nodes.length} ta tugun · ${online} onlayn` + (sims ? ` · ${sims} simulyator` : '');
  $('#tNodes').innerHTML = `
    <thead><tr>
      <th>Tugun</th><th>OS / Versiya</th><th>Holat</th><th>Fayllar</th><th>Xotira (band / ajratilgan)</th>
      <th>Tasdiqlangan vaqt</th><th>Daromad</th><th>Audit</th><th>Oxirgi aloqa</th>
    </tr></thead>
    <tbody>${list.length === 0 ? emptyRow(9, 'Tugunlar topilmadi') : list.map(n => `
      <tr>
        <td><div class="mono strong">${esc(n.id)}${n.simulated ? ' <span class="tag">SIM</span>' : ''}</div><div class="muted small">${esc(n.name || '—')}</div></td>
        <td>${esc(n.os || '—')}<div class="muted small">${esc(n.platform || '')} ${esc(n.version || '')}${n.cpuCores ? ' · ' + esc(n.cpuCores) + ' yadro' : ''}</div></td>
        <td>${nodeStatus(n)}</td>
        <td class="mono">${(n.storedFiles || []).length}</td>
        <td class="mono">${esc(fmtB(n.spaceUsed))} / ${esc(fmtB(n.spaceTotal))}</td>
        <td>${esc(fmtDur(n.verifiedUptimeSec))}</td>
        <td class="mono strong">${esc(fmtUSD(n.earningsCents))}</td>
        <td class="mono"><span class="ok">+${esc(n.auditsPassed)}</span> / <span class="err">−${esc(n.auditsFailed)}</span></td>
        <td class="mono">${esc(ago(n.lastSeen, d.serverTime))}</td>
      </tr>`).join('')}
    </tbody>`;
}

const FILE_STATUS = {
  healthy: ['live', 'Sog\'lom'],
  replicating: ['warn', 'Nusxalanmoqda'],
  degraded: ['warn', 'Tugunlar yetarli emas'],
  restoring: ['warn', 'Serverda tiklanmoqda'],
  unavailable: ['err', 'Hozircha mavjud emas'],
};

function renderFiles() {
  const d = state.dash;
  if (!d) return;
  $('#tFiles').innerHTML = `
    <thead><tr>
      <th>Fayl</th><th>Hajmi</th><th>Chunk</th><th>Nusxalar</th><th>Holat</th><th>Yuklangan</th><th>Amallar</th>
    </tr></thead>
    <tbody>${d.files.length === 0 ? emptyRow(7, 'Hali fayl yuklanmagan') : d.files.map(f => {
      const st = FILE_STATUS[f.status] || ['off', f.status];
      const pct = f.replicaTarget ? Math.min(100, Math.round(f.replicasDone / f.replicaTarget * 100)) : 0;
      return `<tr>
        <td><div class="strong">${esc(f.name)}</div><div class="mono small">${esc(f.id)}</div></td>
        <td class="mono">${esc(fmtB(f.size))}</td>
        <td class="mono">${esc(f.chunks ? f.chunks.length : 0)}${f.missing && f.missing.length ? ` <span class="err">(${esc(f.missing.length)} yo'q)</span>` : ''}</td>
        <td class="mono">${esc(f.replicasDone)} / ${esc(f.replicaTarget)} <span class="muted">(${pct}%)</span></td>
        <td>${badge(st[0], st[1])}</td>
        <td class="mono">${esc(fmtT(f.uploadedAt))}</td>
        <td class="actions">
          <button class="btn btn-outline btn-sm" data-download="${esc(f.id)}">⬇ Yuklab olish</button>
          <button class="btn btn-danger btn-sm" data-delete="${esc(f.id)}" data-name="${esc(f.name)}">O'chirish</button>
        </td>
      </tr>`;
    }).join('')}
    </tbody>`;
}

async function downloadFile(id) {
  const msg = $('#uploadStatus');
  try {
    const r = await fetch(`/api/files/${encodeURIComponent(id)}/download`, { method: 'HEAD', credentials: 'same-origin' });
    if (r.status === 503) {
      flash(msg, '⏳ Fayl serverda to\'liq emas — tugunlardagi nusxadan tiklanmoqda. Birozdan so\'ng qayta urinib ko\'ring.', false);
      return;
    }
    if (!r.ok) throw new Error('HTTP ' + r.status);
    location.href = `/api/files/${encodeURIComponent(id)}/download`;
  } catch (e) {
    flash(msg, '❌ Yuklab olib bo\'lmadi: ' + e.message, false);
  }
}

async function deleteFile(id, name) {
  if (!confirm(`"${name}" faylini klasterdan butunlay o'chirasizmi? Barcha tugunlardagi nusxalar ham o'chiriladi.`)) return;
  try {
    const r = await api('DELETE', '/api/files/' + encodeURIComponent(id));
    flash($('#uploadStatus'), `🗑 O'chirildi (${r.deleteTasks} ta tugundan nusxa o'chiriladi)`, true);
    refreshAdmin();
    loadInfo();
  } catch (e) {
    flash($('#uploadStatus'), '❌ ' + e.message, false);
  }
}

function uploadFile(file) {
  const status = $('#uploadStatus');
  const prog = $('#uploadProgress');
  const bar = $('#uploadBar');
  const btn = $('#btnPick');
  btn.disabled = true;
  prog.classList.remove('hidden');
  bar.style.width = '0%';
  flash(status, `${file.name} yuklanmoqda...`);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', `/api/files/upload?name=${encodeURIComponent(file.name)}&size=${file.size}`);
  xhr.setRequestHeader('X-Requested-With', 'sdrive');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = e => {
    if (e.lengthComputable) {
      const p = Math.round(e.loaded / e.total * 100);
      bar.style.width = p + '%';
      flash(status, `${file.name}: ${p}% (${fmtB(e.loaded)} / ${fmtB(e.total)})`);
    }
  };
  const done = () => {
    btn.disabled = false;
    prog.classList.add('hidden');
    $('#filePicker').value = '';
  };
  xhr.onload = () => {
    done();
    let d = {};
    try { d = JSON.parse(xhr.responseText); } catch (e) { /* bo'sh */ }
    if (xhr.status === 401) { setAdmin(false); return; }
    if (xhr.status >= 200 && xhr.status < 300 && d.ok) {
      flash(status, `✅ ${d.name} saqlandi: ${d.chunks} chunk, ${d.tasksCreated} ta tugunga nusxalanmoqda (maqsad: ${d.replicaTarget})`, true);
      refreshAdmin();
      loadInfo();
    } else {
      flash(status, '❌ Xato: ' + (d.error || ('HTTP ' + xhr.status)), false);
    }
  };
  xhr.onerror = () => { done(); flash(status, '❌ Tarmoq xatosi — yuklash uzildi', false); };
  xhr.send(file);
}

const TASK_KIND = { replica: 'Nusxa', repair: 'Qayta yuklash', restore: 'Serverni tiklash', delete: 'O\'chirish' };
const TASK_STATUS = {
  pending: ['warn', 'Kutilmoqda'],
  done: ['live', 'Bajarildi'],
  failed: ['err', 'Xato'],
  cancelled: ['off', 'Bekor qilindi'],
};

function renderTasks() {
  const d = state.dash;
  if (!d) return;
  const c = d.taskCounts || {};
  $('#taskCounts').innerHTML = Object.keys(TASK_STATUS)
    .map(k => `<span class="meta">${esc(TASK_STATUS[k][1])}: <b>${esc(c[k] || 0)}</b></span>`).join('');
  $('#tTasks').innerHTML = `
    <thead><tr><th>Vazifa</th><th>Tur</th><th>Tugun</th><th>Fayl</th><th>Holat</th><th>Tayinlangan</th><th>Tugagan</th></tr></thead>
    <tbody>${d.tasks.length === 0 ? emptyRow(7, 'Vazifalar yo\'q') : d.tasks.map(t => {
      const st = TASK_STATUS[t.status] || ['off', t.status];
      return `<tr>
        <td class="mono">${esc(t.id)}</td>
        <td><span class="badge">${esc(TASK_KIND[t.kind] || t.kind)}</span></td>
        <td class="mono">${esc(t.nodeId)}</td>
        <td class="mono">${esc(t.fileId)}</td>
        <td>${badge(st[0], st[1])}${t.error ? `<div class="muted small">${esc(t.error)}</div>` : ''}</td>
        <td class="mono">${esc(fmtT(t.assignedAt))}</td>
        <td class="mono">${esc(fmtT(t.doneAt))}</td>
      </tr>`;
    }).join('')}
    </tbody>`;
}

function renderPayouts() {
  const d = state.dash;
  if (!d) return;
  $('#tPayouts').innerHTML = `
    <thead><tr><th>To'lov</th><th>Tugun</th><th>Davr</th><th>Summa</th><th>Holat</th><th>Sana</th></tr></thead>
    <tbody>${d.payouts.length === 0 ? emptyRow(6, 'To\'lovlar tarixi yo\'q') : d.payouts.map(p => `
      <tr>
        <td class="mono">${esc(p.id)}</td>
        <td class="mono">${esc(p.nodeId)}</td>
        <td>${esc(p.period)}</td>
        <td class="mono strong">${esc(fmtUSD(p.amountCents))}</td>
        <td>${p.status === 'paid' ? badge('live', 'To\'langan') : badge('err', 'Yo\'qotilgan (6.2-band)')}</td>
        <td class="mono">${esc(fmtT(p.createdAt))}</td>
      </tr>`).join('')}
    </tbody>`;
}

async function loadSim() {
  try {
    const s = await api('GET', '/api/admin/sim');
    $('#simState').textContent = `${s.running} ishlamoqda · ${s.stopped} to'xtatilgan · jami ${s.total}/${s.max}`;
  } catch (e) { /* e'tiborsiz */ }
}

async function simAction(btn) {
  const action = btn.dataset.sim;
  const count = btn.dataset.count ? parseInt(btn.dataset.count, 10) : undefined;
  const msg = $('#simMsg');
  if (action === 'remove' && !confirm('Barcha simulyator tugunlari va ulardagi nusxalar o\'chiriladi. Davom etasizmi?')) return;
  const orig = btn.textContent;
  btn.disabled = true;
  btn.textContent = 'Bajarilmoqda...';
  try {
    const r = await api('POST', '/api/admin/sim/' + action, count ? { count } : {});
    const texts = {
      start: `✅ ${r.started} ta virtual tugun qo'shildi`,
      offline: `⏻ ${r.stopped} ta tugun uzildi. ${r.note || ''}`,
      online: `↺ ${r.resumed} ta tugun qayta ulandi`,
      remove: `🗑 ${r.removed} ta simulyator tuguni o'chirildi`,
    };
    flash(msg, texts[action], true);
  } catch (e) {
    flash(msg, '❌ ' + e.message, false);
  } finally {
    btn.disabled = false;
    btn.textContent = orig;
    loadSim();
    refreshAdmin();
    loadInfo();
  }
}

// ---------------- AI hisoblash ----------------

const GPU_BADGE = g => g && g.present
  ? `<span class="badge"><span class="dot dot-live"></span>${esc(g.name || 'GPU')}${g.vramBytes ? ' · ' + fmtB(g.vramBytes) : ''} (${esc(g.backend || '')})</span>`
  : '<span class="muted small">CPU</span>';

let computeData = null;

async function loadCompute() {
  try {
    const d = await api('GET', '/api/compute/workers');
    computeData = d;
    $('#computeMeta').innerHTML = [
      `Hisoblash tugunlari: <b>${esc(d.workers.filter(w => w.online).length)}</b> onlayn / ${esc(d.workers.length)}`,
      `GPU tugunlar: <b>${esc(d.workers.filter(w => w.online && w.gpu && w.gpu.present).length)}</b>`,
      `Modellar: <b>${esc(d.models.length)}</b>`,
      `Navbatdagi so'rovlar: <b>${esc(d.queued)}</b>`,
    ].map(x => `<span class="meta">${x}</span>`).join('');

    const sel = $('#chatModel');
    const cur = sel.value;
    sel.innerHTML = d.models.map(m => `<option>${esc(m)}</option>`).join('') ||
      '<option value="">(model yo\'q)</option>';
    if (d.models.includes(cur)) sel.value = cur;
    $('#chatSend').disabled = d.models.length === 0;

    $('#tCompute').innerHTML = `
      <thead><tr><th>Tugun</th><th>OS</th><th>Dvigatel</th><th>GPU / CPU</th><th>RAM</th>
        <th>Modellar</th><th>Holat</th><th>Yuk</th></tr></thead>
      <tbody>${d.workers.length === 0 ? emptyRow(8, 'Hisoblash tuguni yo\'q — mijozni --compute bilan ishga tushiring')
        : d.workers.map(w => `
        <tr>
          <td class="mono strong">${esc(w.id)}</td>
          <td>${esc(w.os || '—')}</td>
          <td><span class="badge">${esc((w.engines || []).join(', ') || '—')}</span></td>
          <td>${GPU_BADGE(w.gpu)}${w.cpuCores ? ` <span class="muted small">${esc(w.cpuCores)} yadro</span>` : ''}</td>
          <td class="mono">${fmtB(w.ramBytes)}</td>
          <td class="small">${(w.models || []).map(m => esc(m)).join('<br>') || '—'}</td>
          <td>${badge(w.online ? 'live' : 'off', w.online ? 'Onlayn' : 'Oflayn')}</td>
          <td class="mono">${esc(w.running)} / ${esc(w.maxParallel)}</td>
        </tr>`).join('')}
      </tbody>`;

    const origin = location.origin;
    $('#apiBase').textContent = origin + '/v1/chat/completions';
    $('#curlExample').textContent =
      `curl ${origin}/v1/chat/completions \\\n` +
      `  -H "Authorization: Bearer <ADMIN-TOKEN yoki -compute-keys>" \\\n` +
      `  -H "Content-Type: application/json" \\\n` +
      `  -d '{"model":"${d.models[0] || 'gemma:2b'}","messages":[{"role":"user","content":"Salom"}]}'`;
  } catch (e) { /* 401 bo'lsa setAdmin(false) */ }
}

async function sendChat() {
  const model = $('#chatModel').value;
  const text = $('#chatInput').value.trim();
  if (!model || !text) return;
  const out = $('#chatOut');
  const meta = $('#chatMeta');
  $('#chatSend').disabled = true;
  $('#chatInput').value = '';
  out.innerHTML = `<div class="chat-q">${esc(text)}</div><div class="chat-a" id="chatA"><span class="muted">…</span></div>`;
  const ans = $('#chatA');
  meta.textContent = 'Klasterga yuborilmoqda...';
  const t0 = Date.now();
  let content = '';
  try {
    const r = await fetch('/api/compute/jobs?stream=1', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'sdrive' },
      credentials: 'same-origin',
      body: JSON.stringify({ model, messages: [{ role: 'user', content: text }] }),
    });
    if (!r.ok || !r.body) {
      let msg = 'HTTP ' + r.status;
      try { msg = (await r.json()).error || msg; } catch (e) { /* */ }
      ans.innerHTML = `<span class="err">❌ ${esc(msg)}</span>`;
      meta.textContent = '';
      return;
    }
    const reader = r.body.getReader();
    const dec = new TextDecoder();
    let buf = '', ev = '';
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        if (line.startsWith('event: ')) ev = line.slice(7).trim();
        else if (line.startsWith('data: ')) {
          const data = line.slice(6);
          if (data === '[DONE]') continue;
          try {
            const j = JSON.parse(data);
            if (ev === 'token') { content += j.token; ans.textContent = content; }
            else if (ev === 'error') { ans.innerHTML = `<span class="err">❌ ${esc(j.error || 'xato')}</span>`; }
            else if (ev === 'done' && !content) { content = j.result || ''; ans.textContent = content; }
          } catch (e) { /* */ }
        }
      }
    }
    meta.textContent = `Javob ${((Date.now() - t0) / 1000).toFixed(1)}s da klaster tugunida bajarildi`;
  } catch (e) {
    ans.innerHTML = `<span class="err">❌ ${esc(e.message)}</span>`;
    meta.textContent = '';
  } finally {
    $('#chatSend').disabled = false;
  }
}

// ---------------- Hodisalar ----------------

function bind() {
  window.addEventListener('hashchange', route);
  $('#gbRange').addEventListener('input', updateCalc);
  $('#hrsRange').addEventListener('input', updateCalc);
  $('#btnAgreement').addEventListener('click', openAgreement);
  $('#footerAgreement').addEventListener('click', e => { e.preventDefault(); openAgreement(); });
  $('#btnCloseModal').addEventListener('click', () => $('#agreeModal').classList.remove('active'));
  $('#agreeModal').addEventListener('click', e => { if (e.target.id === 'agreeModal') e.target.classList.remove('active'); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') $('#agreeModal').classList.remove('active'); });

  $('#loginForm').addEventListener('submit', async e => {
    e.preventDefault();
    const msg = $('#loginMsg');
    try {
      await api('POST', '/api/admin/login', { token: $('#loginToken').value });
      $('#loginToken').value = '';
      flash(msg, '');
      setAdmin(true);
    } catch (err) {
      flash(msg, '❌ ' + err.message, false);
    }
  });
  $('#btnLogout').addEventListener('click', async () => {
    try { await api('POST', '/api/admin/logout', {}); } catch (e) { /* e'tiborsiz */ }
    setAdmin(false);
  });

  $('#searchNodes').addEventListener('input', renderNodes);
  $('#btnPick').addEventListener('click', () => $('#filePicker').click());
  $('#filePicker').addEventListener('change', e => { if (e.target.files[0]) uploadFile(e.target.files[0]); });
  $('#btnRefresh').addEventListener('click', refreshAdmin);
  $('#tFiles').addEventListener('click', e => {
    const dl = e.target.closest('[data-download]');
    if (dl) return downloadFile(dl.dataset.download);
    const del = e.target.closest('[data-delete]');
    if (del) return deleteFile(del.dataset.delete, del.dataset.name);
  });
  $$('[data-sim]').forEach(b => b.addEventListener('click', () => simAction(b)));
  $('#btnRefreshCompute').addEventListener('click', loadCompute);
  $('#chatSend').addEventListener('click', sendChat);
  $('#chatInput').addEventListener('keydown', e => { if (e.key === 'Enter') sendChat(); });
  $('#btnReconcile').addEventListener('click', async () => {
    try {
      await api('POST', '/api/admin/reconcile', {});
      flash($('#simMsg'), '⟳ Replikatsiya tekshirildi', true);
      refreshAdmin();
    } catch (e) {
      flash($('#simMsg'), '❌ ' + e.message, false);
    }
  });
}

async function init() {
  bind();
  updateCalc();
  route();
  loadInfo();
  loadDownloads();
  try {
    const me = await api('GET', '/api/admin/me');
    setAdmin(!!me.admin);
  } catch (e) { /* e'tiborsiz */ }
  setInterval(() => {
    loadInfo();
    if (state.admin && ADMIN_TABS.includes(state.tab)) refreshAdmin();
  }, 5000);
}

init();
