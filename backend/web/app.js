const $ = s => document.querySelector(s);
const $$ = s => document.querySelectorAll(s);

const fmtB = b => {
  if (!b || b <= 0) return '0 B';
  if (b >= 1<<30) return (b/(1<<30)).toFixed(2) + ' GB';
  if (b >= 1<<20) return (b/(1<<20)).toFixed(1) + ' MB';
  return (b/1024).toFixed(0) + ' KB';
};

const fmtT = ts => ts ? new Date(ts*1000).toLocaleString() : '–';
const fmt$ = c => '$' + ((c||0)/100).toFixed(2);

let allNodes = [];
let allFiles = [];
let allTasks = [];
let allPayouts = [];

// Tab Navigation
function switchTab(tabId) {
  $$('.tab-content').forEach(el => el.style.display = 'none');
  $$('.nav-tab').forEach(el => el.classList.remove('active'));
  
  const target = $('#tab-' + tabId);
  if (target) target.style.display = 'block';
  
  const activeBtn = $('#btn-tab-' + tabId);
  if (activeBtn) activeBtn.classList.add('active');
}

async function loadInfo() {
  try {
    const d = await (await fetch('/api/info')).json();
    $('#sNodes').textContent = (d.onlineNodes || 0) + ' / ' + (d.totalNodes || 0);
    $('#sSpace').textContent = fmtB(d.totalSpace || 0);
    $('#sFiles').textContent = d.totalFiles || 0;
    $('#sTasks').textContent = d.pendingTasks || 0;
    $('#sPaid').textContent = fmt$(d.paidCents || 0);
  } catch (e) {}
}

async function loadDash() {
  try {
    const d = await (await fetch('/api/dashboard')).json();
    allNodes = d.nodes || [];
    allFiles = d.files || [];
    allTasks = d.tasks || [];
    allPayouts = d.payouts || [];

    renderNodes();
    renderFiles();
    renderTasks();
    renderPayouts();
  } catch (e) {}
}

function renderNodes() {
  const query = ($('#searchNodes') ? $('#searchNodes').value : '').toLowerCase();
  const filtered = allNodes.filter(n => 
    n.id.toLowerCase().includes(query) || 
    (n.name || '').toLowerCase().includes(query) ||
    (n.os || '').toLowerCase().includes(query)
  );

  const html = `
    <thead>
      <tr>
        <th>Tugun ID</th>
        <th>Nom / Versiya</th>
        <th>OS / Platforma</th>
        <th>Holat</th>
        <th>Ishlatilgan Xotira</th>
        <th>Tasdiqlangan Vaqt</th>
        <th>Daromad (USD)</th>
        <th>Qo'shilgan</th>
      </tr>
    </thead>
    <tbody>
      ${filtered.length === 0 ? '<tr><td colspan="8" style="text-align:center; color: var(--text-secondary); padding: 32px;">Tugunlar topilmadi</td></tr>' : ''}
      ${filtered.map(n => `
        <tr>
          <td class="mono"><b>${n.id}</b></td>
          <td>${n.name || 'SDrive Node'}</td>
          <td>${n.os} / ${n.platform || 'x64'}</td>
          <td>
            <span class="badge">
              <span class="dot ${n.status === 'uninstalled' ? 'dot-off' : n.online ? 'dot-live' : 'dot-off'}"></span>
              ${n.status === 'uninstalled' ? 'O\'chirilgan' : n.online ? 'Onlayn' : 'Oflayn'}
            </span>
          </td>
          <td class="mono">${fmtB(n.spaceUsed)} / ${fmtB(n.spaceTotal)}</td>
          <td>${Math.floor((n.verifiedUptimeSec || 0)/3600)} soat</td>
          <td class="mono" style="font-weight:700;">${fmt$(n.earningsCents)}</td>
          <td class="mono">${fmtT(n.joinedAt)}</td>
        </tr>
      `).join('')}
    </tbody>
  `;
  if ($('#tNodes')) $('#tNodes').innerHTML = html;
}

function renderFiles() {
  const html = `
    <thead>
      <tr>
        <th>Fayl ID</th>
        <th>Fayl Nomi</th>
        <th>Hajmi</th>
        <th>Chunk Soni</th>
        <th>Replikatsiya</th>
        <th>Holat</th>
        <th>Yuklangan</th>
        <th>Amal</th>
      </tr>
    </thead>
    <tbody>
      ${allFiles.length === 0 ? '<tr><td colspan="8" style="text-align:center; color: var(--text-secondary); padding: 32px;">Saqlangan fayllar yo\'q</td></tr>' : ''}
      ${allFiles.map(f => `
        <tr>
          <td class="mono"><b>${f.id}</b></td>
          <td>${f.name}</td>
          <td class="mono">${fmtB(f.size)}</td>
          <td>${f.chunks ? f.chunks.length : 0} ta</td>
          <td>${f.replicasDone} / ${f.replicaTarget} (${Math.round((f.replicasDone/(f.replicaTarget||1))*100)}%)</td>
          <td>
            <span class="badge">
              <span class="dot ${f.status === 'degraded' ? 'dot-off' : 'dot-live'}"></span>
              ${f.status === 'degraded' ? 'Qisman' : 'Faol (30%+)'}
            </span>
          </td>
          <td class="mono">${fmtT(f.uploadedAt)}</td>
          <td>
            <a href="/api/files/download?id=${f.id}" class="btn btn-outline btn-sm" download>⬇ Yuklab olish</a>
          </td>
        </tr>
      `).join('')}
    </tbody>
  `;
  if ($('#tFiles')) $('#tFiles').innerHTML = html;
}

async function uploadFileWeb(inputEl) {
  const file = inputEl.files[0];
  if (!file) return;

  const statusEl = $('#uploadStatus');
  if (statusEl) statusEl.textContent = 'Fayl klasterga yuklanmoqda...';

  try {
    const res = await fetch(`/api/files/upload?name=${encodeURIComponent(file.name)}&size=${file.size}`, {
      method: 'POST',
      body: file
    });
    const data = await res.json();
    if (data.ok) {
      if (statusEl) statusEl.textContent = `✅ Fayl klasterga saqlandi! ID: ${data.fileId} (${data.chunks} ta chunk yaratildi)`;
      inputEl.value = '';
      loadInfo();
      loadDash();
    } else {
      if (statusEl) statusEl.textContent = `❌ Xato: ${data.error}`;
    }
  } catch (e) {
    if (statusEl) statusEl.textContent = `❌ Xato: ${e.message}`;
  }
}

function renderTasks() {
  const html = `
    <thead>
      <tr>
        <th>Vazifa ID</th>
        <th>Target Tugun</th>
        <th>Fayl ID</th>
        <th>Tur</th>
        <th>Status</th>
        <th>Tayinlangan</th>
      </tr>
    </thead>
    <tbody>
      ${allTasks.length === 0 ? '<tr><td colspan="6" style="text-align:center; color: var(--text-secondary); padding: 32px;">Kutilayotgan vazifalar yo'q</td></tr>' : ''}
      ${allTasks.map(t => `
        <tr>
          <td class="mono">${t.id}</td>
          <td class="mono">${t.nodeId}</td>
          <td class="mono">${t.fileId}</td>
          <td><span class="badge">${t.kind}</span></td>
          <td>
            <span class="badge">
              <span class="dot ${t.status === 'done' ? 'dot-live' : 'dot-off'}"></span>
              ${t.status}
            </span>
          </td>
          <td class="mono">${fmtT(t.assignedAt)}</td>
        </tr>
      `).join('')}
    </tbody>
  `;
  if ($('#tTasks')) $('#tTasks').innerHTML = html;
}

function renderPayouts() {
  const html = `
    <thead>
      <tr>
        <th>To'lov ID</th>
        <th>Tugun ID</th>
        <th>Davr</th>
        <th>Summa</th>
        <th>Holat</th>
        <th>Sana</th>
      </tr>
    </thead>
    <tbody>
      ${allPayouts.length === 0 ? '<tr><td colspan="6" style="text-align:center; color: var(--text-secondary); padding: 32px;">To'lovlar tarixi yo'q</td></tr>' : ''}
      ${allPayouts.map(p => `
        <tr>
          <td class="mono">${p.id}</td>
          <td class="mono">${p.nodeId}</td>
          <td>${p.period}</td>
          <td class="mono" style="font-weight:700;">${fmt$(p.amountCents)}</td>
          <td>
            <span class="badge">
              <span class="dot ${p.status === 'paid' ? 'dot-live' : 'dot-off'}"></span>
              ${p.status === 'paid' ? 'To\'langan' : 'Yo\'qotilgan (EULA)'}
            </span>
          </td>
          <td class="mono">${fmtT(p.createdAt)}</td>
        </tr>
      `).join('')}
    </tbody>
  `;
  if ($('#tPayouts')) $('#tPayouts').innerHTML = html;
}

// Node Simulator (10+ concurrent nodes)
async function simulateNodes(count) {
  const btn = event.target;
  const origText = btn.textContent;
  btn.textContent = 'Simulyatsiya qilinmoqda...';
  btn.disabled = true;

  for (let i = 1; i <= count; i++) {
    const nodeId = 'sim-node-' + Math.floor(100000 + Math.random() * 900000);
    const oses = ['Windows 11', 'macOS Sonoma', 'Ubuntu 24.04', 'Debian 12'];
    const selectedOs = oses[Math.floor(Math.random() * oses.length)];
    const spaceGB = [50, 100, 200, 500, 1000][Math.floor(Math.random() * 5)];

    try {
      await fetch('/api/nodes/register', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          nodeId: nodeId,
          os: selectedOs,
          platform: 'x64',
          spaceTotal: spaceGB * 1024 * 1024 * 1024,
          name: 'Simulated Node #' + i
        })
      });

      // Send initial heartbeat
      await fetch('/api/nodes/heartbeat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          nodeId: nodeId,
          spaceUsed: Math.floor(Math.random() * spaceGB * 0.4) * 1024 * 1024 * 1024
        })
      });
    } catch (e) {}
  }

  btn.textContent = origText;
  btn.disabled = false;
  loadInfo();
  loadDash();
}

function updateCalc() {
  const gb = parseInt($('#gbRange').value);
  const hrs = parseInt($('#hrsRange').value);
  $('#gbLabel').textContent = gb + ' GB';
  $('#hrsLabel').textContent = hrs + ' soat';

  const usd = gb * (hrs * 30) * 0.0005;
  const uzs = Math.round(usd * 12800);

  if ($('#calcUsd')) $('#calcUsd').textContent = '$' + usd.toFixed(2);
  if ($('#calcUzs')) $('#calcUzs').textContent = '~ ' + uzs.toLocaleString() + ' UZS';
}

function openAgreementModal() {
  const modal = $('#agreeModal');
  if (modal) modal.classList.add('active');
  fetch('/api/agreement')
    .then(r => r.json())
    .then(d => {
      if ($('#modalText')) $('#modalText').textContent = d.text || 'Shartnoma yuklanmadi';
    });
}

function closeAgreementModal() {
  const modal = $('#agreeModal');
  if (modal) modal.classList.remove('active');
}

// Initial Load
loadInfo();
loadDash();
setInterval(() => {
  loadInfo();
  loadDash();
}, 5000);
