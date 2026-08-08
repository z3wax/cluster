#pragma once
// Mahalliy UI — embedded HTML sahifa (server.cpp orqali ko'rsatiladi)
inline const char* UI_HTML = R"HTML(<!DOCTYPE html>
<html lang="uz">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SDrive Cluster — Boshqaruv Panel</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg: #000000;
  --card: #121215;
  --border: #27272a;
  --border-focus: #ffffff;
  --txt: #ffffff;
  --dim: #a1a1aa;
  --muted: #52525b;
}
*{box-sizing:border-box;margin:0;padding:0}
body{
  background: var(--bg);
  color: var(--txt);
  font-family: 'Inter', -apple-system, system-ui, sans-serif;
  line-height: 1.5;
  padding: 32px 16px;
  min-height: 100vh;
  -webkit-font-smoothing: antialiased;
}
.container{max-width:720px;margin:0 auto}
h1{font-size:22px;font-weight:800;letter-spacing:-0.02em;margin-bottom:4px;display:flex;align-items:center;gap:10px}
.logo-icon{width:26px;height:26px;background:#fff;color:#000;border-radius:6px;display:flex;align-items:center;justify-content:center;font-size:14px;font-weight:800}
.sub{color:var(--dim);font-size:13px;margin-bottom:24px}
.card{
  background: var(--card);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 24px;
  margin-bottom: 20px;
}
h2{font-size:16px;font-weight:700;margin-bottom:12px;display:flex;align-items:center;justify-content:space-between}
label{display:block;color:var(--dim);font-size:12px;font-weight:600;margin:14px 0 6px;text-transform:uppercase;letter-spacing:0.04em}
input,select{
  width:100%;
  background: #09090b;
  border: 1px solid var(--border);
  color: var(--txt);
  border-radius: 8px;
  padding: 10px 14px;
  font-size: 13px;
  outline: none;
  font-family: inherit;
  transition: all 0.15s ease;
}
input:focus, select:focus {
  border-color: var(--border-focus);
}
button{
  margin-top: 20px;
  width: 100%;
  padding: 12px;
  border: none;
  border-radius: 8px;
  font-weight: 700;
  font-size: 14px;
  font-family: inherit;
  cursor: pointer;
  background: #ffffff;
  color: #000000;
  transition: all 0.15s ease;
}
button:hover{ background: #e4e4e7; }
button.ghost{ background: transparent; border: 1px solid var(--border); color: var(--txt); }
button.ghost:hover{ background: #18181b; border-color: #ffffff; }
button.danger{ background: rgba(244, 63, 94, 0.1); border: 1px solid rgba(244, 63, 94, 0.3); color: #f43f5e; }
button.danger:hover{ background: #f43f5e; color: #ffffff; }
button:disabled{ opacity: .4; cursor: not-allowed; }
.stats{ display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; margin-bottom: 20px; }
.stat{
  background: #09090b;
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 14px;
  text-align: center;
}
.stat b{ display: block; font-size: 20px; color: #ffffff; font-family: 'JetBrains Mono', monospace; }
.stat span{ color: var(--dim); font-size: 12px; font-weight: 500; }
.agree{
  max-height: 280px;
  overflow-y: auto;
  background: #09090b;
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 16px;
  font-size: 12px;
  white-space: pre-wrap;
  color: var(--dim);
  line-height: 1.6;
}
.badge{ display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 6px; font-size: 12px; font-weight: 500; background: #18181b; color: #ffffff; border: 1px solid #3f3f46; }
.dot{ width: 6px; height: 6px; border-radius: 50%; background: #ffffff; }
.dot-live{ background: #22c55e; box-shadow: 0 0 6px rgba(34, 197, 94, 0.8); }
.dot-off{ background: #71717a; }
.note{ color: var(--muted); font-size: 12px; margin-top: 10px; }
.hidden{ display: none; }
#msg, #msg2, #msg3{ font-size: 13px; margin-top: 12px; font-weight: 600; }
</style>
</head>
<body>
<div class="container">
<h1><span class="logo-icon">⬡</span> SDrive <b>Cluster</b></h1>
<div class="sub" id="nodeInfo">Yuklanmoqda...</div>

<div id="pageSetup" class="hidden">
  <div class="card">
    <h2>1. SDrive Diskini Sozlash</h2>
    <p class="sub">Kompyuteringizdan qaysi disk va qancha bo'sh joy (GB) ajratmoqchisiz?</p>
    <label>Disk Harfi (Windows)</label>
    <select id="driveLetter"><option>S</option><option>D</option><option>E</option><option>F</option><option>G</option><option>H</option><option>I</option><option>J</option><option>K</option><option>L</option><option>M</option><option>N</option><option>O</option><option>P</option><option>Q</option><option>R</option><option>T</option><option>U</option><option>V</option><option>W</option><option>X</option><option>Y</option><option>Z</option></select>
    <label>Bo'sh Joy Hajmi (GB)</label>
    <input type="number" id="quotaGb" value="50" min="1" max="2000">
    <label>Saqlash Papkasi (Local Path)</label>
    <input type="text" id="sdrivePath" placeholder="">
    <button onclick="doSetup()">SDrive Diskini Yaratish</button>
    <div id="msg"></div>
  </div>
</div>

<div id="pageAgree" class="hidden">
  <div class="card">
    <h2>2. Foydalanuvchi Shartnomasi (EULA)</h2>
    <div class="agree" id="agreeText">Yuklanmoqda...</div>
    <button onclick="acceptAgreement()">ROZIMAN — Shartnomani Qabul Qilaman</button>
    <p class="note">Rozilik bildirmasangiz dastur ma'lumot saqlamaydi va daromad hisoblanmaydi.</p>
    <div id="msg2"></div>
  </div>
</div>

<div id="pageRun" class="hidden">
  <div class="card">
    <h2>Holat <span id="statusPill" class="badge"><span class="dot dot-off"></span> Oflayn</span></h2>
    <div class="stats">
      <div class="stat"><b id="stSpace">–</b><span>Band / Jami</span></div>
      <div class="stat"><b id="stTasks">–</b><span>Vazifalar</span></div>
      <div class="stat"><b id="stEarn">–</b><span>Daromad (USD)</span></div>
      <div class="stat"><b id="stUp">–</b><span>Tasdiqlangan Vaqt</span></div>
    </div>
    <p class="sub">Dastur fon rejimida muvaffaqiyatli ishlamoqda. Saqlanayotgan ma'lumotlar 256-bit shifrlangan.</p>
    <button class="ghost" onclick="claimPayout()">Cash Chiqarish (Oy Oxiri)</button>
    <button class="danger" onclick="doUninstall()">Dasturni O'chirish</button>
    <div id="msg3"></div>
  </div>
</div>

</div>
<script>
const $=s=>document.querySelector(s);
function show(p){['pageSetup','pageAgree','pageRun'].forEach(x=>$('#'+x).classList.add('hidden'));$('#'+p).classList.remove('hidden')}
async function j(method,url,body){const r=await fetch(url,{method,headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});return r.json()}

async function status(){
  const s=await j('GET','/api/status');
  $('#nodeInfo').textContent='Tugun ID: '+s.nodeId+' · Server: '+s.serverUrl;
  if(!s.setupDone){show('pageSetup');$('#sdrivePath').value=s.sdriveDir;return}
  if(!s.agreementAccepted){show('pageAgree');if(!window._agree){j('GET','/api/agreement').then(a=>{$('#agreeText').textContent=a.text});window._agree=true}return}
  show('pageRun');
  $('#stSpace').textContent=s.spaceUsedMB+' / '+s.quotaGB+' GB';
  $('#stTasks').textContent=s.tasksDue;
  $('#stEarn').textContent='$'+(s.earningsCents/100).toFixed(2);
  $('#stUp').textContent=Math.floor(s.uptimeSec/3600)+' soat';
  const p=$('#statusPill');
  p.innerHTML='<span class="dot '+(s.online?'dot-live':'dot-off')+'"></span> '+(s.online?'Onlayn':'Oflayn');
}
async function doSetup(){
  $('#msg').textContent='';
  const r=await j('POST','/api/setup',{driveLetter:$('#driveLetter').value,quotaGb:parseFloat($('#quotaGb').value),sdriveDir:$('#sdrivePath').value});
  if(r.ok){status()}else{$('#msg').textContent='Xato: '+r.error}
}
async function acceptAgreement(){
  const r=await j('POST','/api/accept');if(r.ok)status();
}
async function claimPayout(){
  const r=await j('POST','/api/claim');
  $('#msg3').textContent=r.ok?('Payout rasmiylashtirildi: $'+(r.amountCents/100).toFixed(2)+(r.forfeited?' (yo\'qotilgan — hisob o\'chirilgan)':'')):('Xato: '+r.error);
}
async function doUninstall(){
  if(!confirm('Diqqat! Oylik pulni oy oxirida chiqarib olmasangiz, hisob uchun pul to\'lanmaydi (shartnoma 6.2-band). Davom etasizmi?'))return;
  const r=await j('POST','/api/uninstall');
  $('#msg3').textContent=r.ok?'Dastur o\'chirildi. SDrive diskidagi ma\'lumotlar olib tashlanmadi.':'Xato: '+r.error;
}
status();setInterval(status,5000);
</script>
</body>
</html>)HTML";
