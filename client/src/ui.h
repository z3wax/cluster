#pragma once
// Mahalliy UI — embedded HTML sahifa (server.cpp orqali ko'rsatiladi). Tashqi resurslarsiz.
inline const char* UI_HTML = R"HTML(<!DOCTYPE html>
<html lang="uz">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SDrive Cluster — Boshqaruv paneli</title>
<style>
:root{--bg:#000;--card:#121215;--sub:#09090b;--border:#27272a;--txt:#fff;--dim:#a1a1aa;--muted:#71717a;--ok:#22c55e;--warn:#f59e0b;--err:#f43f5e}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,sans-serif;line-height:1.5;padding:32px 16px;min-height:100vh;-webkit-font-smoothing:antialiased}
.container{max-width:760px;margin:0 auto}
h1{font-size:22px;font-weight:800;letter-spacing:-.02em;margin-bottom:4px;display:flex;align-items:center;gap:10px}
.logo{width:26px;height:26px;background:#fff;color:#000;border-radius:6px;display:flex;align-items:center;justify-content:center;font-size:14px;font-weight:800}
.sub{color:var(--dim);font-size:13px;margin-bottom:20px;overflow-wrap:anywhere}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:24px;margin-bottom:20px}
h2{font-size:16px;font-weight:700;margin-bottom:12px;display:flex;align-items:center;justify-content:space-between;gap:12px}
label{display:block;color:var(--dim);font-size:12px;font-weight:600;margin:16px 0 6px;text-transform:uppercase;letter-spacing:.04em}
input,select{width:100%;background:var(--sub);border:1px solid var(--border);color:var(--txt);border-radius:8px;padding:10px 14px;font-size:13px;outline:none;font-family:inherit}
input:focus,select:focus{border-color:#fff}
.hint{color:var(--muted);font-size:12px;margin-top:6px}
button{margin-top:20px;width:100%;padding:12px;border:none;border-radius:8px;font-weight:700;font-size:14px;font-family:inherit;cursor:pointer;background:#fff;color:#000}
button:hover{background:#e4e4e7}
button.ghost{background:transparent;border:1px solid var(--border);color:var(--txt)}
button.ghost:hover{background:#18181b;border-color:#fff}
button.danger{background:rgba(244,63,94,.1);border:1px solid rgba(244,63,94,.3);color:var(--err)}
button.danger:hover{background:var(--err);color:#fff}
button:disabled{opacity:.4;cursor:not-allowed}
.row{display:flex;gap:12px}.row>*{flex:1}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:16px}
.stat{background:var(--sub);border:1px solid var(--border);border-radius:8px;padding:14px;text-align:center}
.stat b{display:block;font-size:18px;font-family:ui-monospace,Menlo,Consolas,monospace}
.stat span{color:var(--dim);font-size:12px}
.bar{height:6px;background:var(--border);border-radius:3px;overflow:hidden;margin:6px 0 14px}
.bar div{height:100%;background:#fff;width:0}
.agree{max-height:320px;overflow-y:auto;background:var(--sub);border:1px solid var(--border);border-radius:8px;padding:16px;font-size:12px;white-space:pre-wrap;color:var(--dim);line-height:1.6}
.check{display:flex;gap:10px;align-items:center;margin-top:14px;font-size:13px;color:var(--dim);text-transform:none;letter-spacing:0;font-weight:500}
.check input{width:auto}
.badge{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:6px;font-size:12px;font-weight:500;background:#18181b;border:1px solid #3f3f46}
.dot{width:7px;height:7px;border-radius:50%;background:var(--muted)}
.dot.ok{background:var(--ok);box-shadow:0 0 6px rgba(34,197,94,.8)}.dot.warn{background:var(--warn)}.dot.err{background:var(--err)}
.note{color:var(--muted);font-size:12px;margin-top:10px}
.msg{font-size:13px;margin-top:12px;font-weight:600}.msg.ok{color:var(--ok)}.msg.err{color:var(--err)}
.kv{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;font-size:12px;color:var(--dim)}.kv b{color:var(--txt);font-weight:500;overflow-wrap:anywhere}
.hidden{display:none!important}
.steps{display:flex;gap:8px;margin-bottom:16px}.steps span{flex:1;height:4px;border-radius:2px;background:var(--border)}.steps span.on{background:#fff}
</style>
</head>
<body>
<div class="container">
<h1><span class="logo">⬡</span> SDrive <b>Cluster</b></h1>
<div class="sub" id="nodeInfo">Yuklanmoqda...</div>

<div id="pageSetup" class="hidden">
  <div class="steps"><span class="on"></span><span></span></div>
  <div class="card">
    <h2>1. SDrive diskini sozlash</h2>
    <p class="sub">Kompyuteringizdan qaysi diskdan va qancha bo'sh joy ajratmoqchisiz? (shartnoma 2.1)</p>
    <div id="letterBox">
      <label for="driveLetter">Disk harfi</label>
      <select id="driveLetter"></select>
      <div class="hint">Windows'da SDrive shu harf bilan alohida disk sifatida ko'rinadi.</div>
    </div>
    <label for="sdrivePath">Saqlash papkasi</label>
    <input type="text" id="sdrivePath">
    <label for="quotaGb">Ajratiladigan joy (GB)</label>
    <input type="number" id="quotaGb" value="50" min="1" step="1">
    <div class="hint" id="freeHint"></div>
    <label for="serverUrl">Klaster serveri</label>
    <input type="text" id="serverUrl" placeholder="http://server:8080">
    <button id="btnSetup">SDrive diskini yaratish</button>
    <div class="msg" id="msgSetup"></div>
  </div>
</div>

<div id="pageAgree" class="hidden">
  <div class="steps"><span class="on"></span><span class="on"></span></div>
  <div class="card">
    <h2>2. Foydalanuvchi shartnomasi (EULA) <span class="badge" id="agreeVer"></span></h2>
    <div class="agree" id="agreeText">Yuklanmoqda...</div>
    <label class="check"><input type="checkbox" id="agreeCheck"> Shartnomani to'liq o'qib chiqdim</label>
    <button id="btnAccept" disabled>ROZIMAN — shartnomani qabul qilaman</button>
    <p class="note">Rozilik bildirilmasa, dastur fon ishini boshlamaydi va hech qanday ma'lumot saqlamaydi (9.3-band).</p>
    <div class="msg" id="msgAgree"></div>
  </div>
</div>

<div id="pageRun" class="hidden">
  <div class="card">
    <h2>Holat <span class="badge" id="statusPill"><span class="dot"></span> —</span></h2>
    <div class="stats">
      <div class="stat"><b id="stFiles">–</b><span>Fayllar (tasdiqlangan / lokal)</span></div>
      <div class="stat"><b id="stEarn">–</b><span>Balans (USD)</span></div>
      <div class="stat"><b id="stUp">–</b><span>Tasdiqlangan vaqt</span></div>
      <div class="stat"><b id="stTasks">–</b><span>Vazifalar</span></div>
    </div>
    <div class="kv"><span>SDrive band</span><b id="stSpace">–</b></div>
    <div class="bar"><div id="spaceBar"></div></div>
    <div class="kv">
      <span>Faoliyat</span><b id="stActivity">–</b>
      <span>Saqlash papkasi</span><b id="stDir">–</b>
      <span>Avtostart</span><b id="stAuto">–</b>
      <span>Oxirgi aloqa</span><b id="stLast">–</b>
    </div>
    <p class="msg err hidden" id="stError"></p>
    <p class="note">Barcha nusxalar 256-bit kalit bilan shifrlangan (ChaCha20 + HMAC-SHA256). Kalit faqat shu kompyuterda saqlanadi. Siz kompyuterdan faol foydalanayotganingizda yuklab olish sekinlashtiriladi (2.4-band).</p>
  </div>
  <div class="card">
    <h2>Cash chiqarish</h2>
    <p class="sub" id="payoutInfo">–</p>
    <div class="row">
      <button class="ghost" id="btnClaim">Cash chiqarish (oy oxiri)</button>
      <button class="ghost" id="btnSettings">Sozlamalarni o'zgartirish</button>
    </div>
    <div class="msg" id="msgRun"></div>
  </div>
  <div class="card">
    <h2>Dasturni o'chirish</h2>
    <p class="sub">Oylik pulni oy oxirida chiqarib olmasdan dasturni o'chirsangiz, balans to'lanmaydi (6.2-band). SDrive papkasidagi shifrlangan klaster fayllari ham o'chiriladi.</p>
    <button class="danger" id="btnUninstall">Dasturni o'chirish</button>
  </div>
</div>

<div id="pageDone" class="hidden">
  <div class="card">
    <h2>Dastur o'chirildi</h2>
    <p class="sub" id="doneText">SDrive Cluster to'xtatildi va avtostartdan olib tashlandi. Endi dastur faylini o'chirishingiz mumkin.</p>
  </div>
</div>
</div>
<script>
'use strict';
const $=s=>document.querySelector(s);
const GB=1024*1024*1024;
const fmtB=b=>{b=Number(b)||0;if(b<=0)return'0 B';const u=['B','KB','MB','GB','TB'];let i=0;while(b>=1024&&i<u.length-1){b/=1024;i++}return(i?b.toFixed(b>=100?0:1):b)+' '+u[i]};
const fmtUSD=c=>{const u=(Number(c)||0)/100;return '$'+(u>0&&u<0.01?u.toFixed(6):u.toFixed(2))};
const fmtDur=s=>{s=Number(s)||0;const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+' soat '+m+' daq':m+' daq'};
let S=null, editing=false, agreeVersion='';
function msg(el,t,ok){el.textContent=t;el.className='msg '+(ok===true?'ok':ok===false?'err':'')}
async function api(method,url,body){
  const opt={method,headers:{'X-SDrive-UI':'1'}};
  if(body!==undefined){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body)}
  const r=await fetch(url,opt);let d={};try{d=await r.json()}catch(e){}
  if(!r.ok&&d.ok===undefined)d.ok=false;
  if(!r.ok&&!d.error)d.error='HTTP '+r.status;
  return d;
}
function show(p){['pageSetup','pageAgree','pageRun','pageDone'].forEach(x=>$('#'+x).classList.toggle('hidden',x!==p))}
function fillSetup(){
  const sel=$('#driveLetter');
  if(!sel.options.length){
    const letters=(S.freeLetters||'').split('');
    if(S.driveLetter&&!letters.includes(S.driveLetter))letters.unshift(S.driveLetter);
    if(!letters.length)letters.push('S');
    letters.forEach(l=>{const o=document.createElement('option');o.value=o.textContent=l;sel.appendChild(o)});
    sel.value=letters.includes(S.driveLetter)?S.driveLetter:(letters.includes('S')?'S':letters[0]);
    $('#sdrivePath').value=S.sdriveDir||S.defaultDir;
    $('#serverUrl').value=S.serverUrl;
    if(S.quotaBytes>0)$('#quotaGb').value=Math.round(S.quotaBytes/GB);
  }
  $('#letterBox').classList.toggle('hidden',S.platform!=='windows');
  $('#freeHint').textContent=S.diskFreeBytes>=0?('Diskda bo\'sh joy: '+fmtB(S.diskFreeBytes)):'';
}
async function loadAgreement(){
  const a=await api('GET','/api/agreement');
  if(a.text){agreeVersion=a.version;$('#agreeText').textContent=a.text;$('#agreeVer').textContent='v'+a.version;msg($('#msgAgree'),'')}
  else{$('#agreeText').textContent='Shartnoma yuklanmadi: '+(a.error||'')+'\n\nServer manzilini tekshiring va sahifani yangilang.';}
}
async function status(){
  let s;
  try{s=await(await fetch('/api/status')).json()}catch(e){
    if(S&&S.state==='uninstalled'){show('pageDone');return}
    $('#nodeInfo').textContent='Dastur ishlamayapti (lokal xizmat javob bermadi)';return}
  S=s;
  $('#nodeInfo').textContent='Tugun: '+s.nodeId+' · Server: '+s.serverUrl+' · v'+s.version;
  if(s.state==='uninstalled'){show('pageDone');return}
  if(!s.setupDone||editing){show('pageSetup');fillSetup();return}
  if(!s.agreementAccepted){show('pageAgree');if(!agreeVersion)loadAgreement();if(s.lastError)msg($('#msgAgree'),s.lastError,false);return}
  show('pageRun');
  const p=$('#statusPill');
  const st=s.online?(s.userActive?['warn','Onlayn · siz faolsiz']:['ok','Onlayn']):(s.registered?['err','Aloqa yo\'q']:['warn','Ulanmoqda...']);
  p.innerHTML='';const d=document.createElement('span');d.className='dot '+st[0];p.appendChild(d);p.appendChild(document.createTextNode(' '+st[1]));
  $('#stFiles').textContent=s.storedFiles+' / '+s.localFiles;
  $('#stEarn').textContent=fmtUSD(s.earningsCents);
  $('#stUp').textContent=fmtDur(s.uptimeSec);
  $('#stTasks').textContent=s.tasksDue;
  $('#stSpace').textContent=fmtB(s.usedBytes)+' / '+fmtB(s.quotaBytes)+(s.platform==='windows'?' ('+s.driveLetter+':)':'');
  $('#spaceBar').style.width=Math.min(100,s.quotaBytes?s.usedBytes/s.quotaBytes*100:0)+'%';
  $('#stActivity').textContent=s.activity||'Kutish rejimi';
  $('#stDir').textContent=s.sdriveDir;
  $('#stAuto').textContent=s.autostart?'Yoqilgan — kompyuter yoqilganda ishga tushadi':'O\'rnatilmagan';
  $('#stLast').textContent=s.lastHeartbeat?Math.max(0,Math.round(s.serverTime-s.lastHeartbeat))+' s oldin':'–';
  const e=$('#stError');e.textContent=s.lastError?('⚠ '+s.lastError):'';e.classList.toggle('hidden',!s.lastError);
  $('#payoutInfo').textContent=s.payoutOpen?'Payout oynasi ochiq — balansni hozir chiqarib olishingiz mumkin.':
    ('Payout oy oxirida beriladi (6.3-band)'+(s.payoutOpensAt?': '+new Date(s.payoutOpensAt*1000).toLocaleDateString()+' dan boshlab.':'.'));
}
$('#btnSetup').addEventListener('click',async()=>{
  const b=$('#btnSetup');b.disabled=true;msg($('#msgSetup'),'Saqlanmoqda...');
  const r=await api('POST','/api/setup',{driveLetter:$('#driveLetter').value||'S',quotaGb:parseFloat($('#quotaGb').value),sdriveDir:$('#sdrivePath').value.trim(),serverUrl:$('#serverUrl').value.trim()});
  b.disabled=false;
  if(r.ok){editing=false;msg($('#msgSetup'),'');agreeVersion='';status()}else msg($('#msgSetup'),'Xato: '+r.error,false);
});
$('#agreeCheck').addEventListener('change',e=>{$('#btnAccept').disabled=!e.target.checked||!agreeVersion});
$('#btnAccept').addEventListener('click',async()=>{
  const r=await api('POST','/api/accept',{version:agreeVersion});
  if(r.ok)status();else msg($('#msgAgree'),'Xato: '+r.error,false);
});
$('#btnClaim').addEventListener('click',async()=>{
  const r=await api('POST','/api/claim');
  if(r.ok)msg($('#msgRun'),'✅ Payout rasmiylashtirildi: '+fmtUSD(r.payout&&r.payout.amountCents),true);
  else msg($('#msgRun'),'Xato: '+r.error,false);
  status();
});
$('#btnSettings').addEventListener('click',()=>{editing=true;$('#driveLetter').innerHTML='';status()});
$('#btnUninstall').addEventListener('click',async()=>{
  if(!confirm('Diqqat! Oylik pulni oy oxirida chiqarib olmasangiz, balans to\'lanmaydi (shartnoma 6.2-band). SDrive fayllari o\'chiriladi. Davom etasizmi?'))return;
  const r=await api('POST','/api/uninstall');
  $('#doneText').textContent='SDrive Cluster to\'xtatildi, avtostartdan olib tashlandi va klaster fayllari o\'chirildi.'+
    (r.forfeitedCents>0?' Yo\'qotilgan balans: '+fmtUSD(r.forfeitedCents)+'.':'')+(r.warning?' Ogohlantirish: '+r.warning:'')+' Endi dastur faylini o\'chirishingiz mumkin.';
  S.state='uninstalled';show('pageDone');
});
status();setInterval(status,3000);
</script>
</body>
</html>)HTML";
