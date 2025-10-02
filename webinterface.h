#ifndef WEBINTERFACE_H
#define WEBINTERFACE_H

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0'>
<title>ESP32 Control</title>
<style>
body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0}
.container{max-width:800px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}
h1{color:#333;text-align:center}
.section{margin:20px 0;padding:15px;border:1px solid #ddd;border-radius:5px;background:#f9f9f9}
.button-config{display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:center;margin:10px 0}
input[type='text']{padding:8px;border:1px solid #ccc;border-radius:4px}
button{padding:8px 15px;background:#007bff;color:#fff;border:none;border-radius:4px;cursor:pointer}
button:hover{background:#0056b3}
.relay-controls{text-align:center}
.status-indicator{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:5px}
.status-on{background:#28a745}.status-off{background:#dc3545}
.button-status{display:inline-block;width:20px;height:20px;border-radius:3px;margin-right:5px}
.button-pressed{background:#ffc107}.button-released{background:#6c757d}
.auto-control{margin:10px 0;padding:10px;border:1px solid #007bff;border-radius:4px;background:#e7f3ff}
</style>
</head>
<body>
<div class='container'>
<h1>ESP32 Control Panel</h1>

<div class='section'>
<h2>Controle Automatique</h2>
<div class='auto-control'>
<label><input type='checkbox' id='autoRelay' onchange='toggleAutoRelay(this.checked)'> Controle automatique du relais principal</label>
<p>Session courante: <strong id='currentSession'>-</strong></p>
<p>Statut: <span class='status-indicator' id='autoStatus'></span><span id='autoStatusText'>-</span></p>
</div>
</div>

<div class='section'>
<h2>Configuration des Boutons</h2>
<div id='buttonConfig'></div>
</div>

<div class='section'>
<h2>Etat des Boutons</h2>
<div id='buttonStatus'></div>
</div>

<div class='section'>
<h2>Controle Manuel du Relais Principal</h2>
<div class='relay-controls'>
<button onclick='setRelay(true)'>Activer Relais</button>
<button onclick='setRelay(false)'>Desactiver Relais</button>
<p>Etat: <span class='status-indicator' id='relayStatus'></span><span id='relayText'>-</span></p>
</div>
</div>

</div>

<script>
let buttonSessions=["","","","",""];

async function fetchData(){
 try{
  const r=await fetch('/api/status');
  const data=await r.json();
  document.getElementById('autoRelay').checked=data.autoRelay;
  document.getElementById('currentSession').textContent=data.currentSession||'-';
  document.getElementById('autoStatus').className='status-indicator '+(data.currentSession?'status-on':'status-off');
  document.getElementById('autoStatusText').textContent=data.currentSession?'Session active':'Aucune session';
  for(let i=0;i<5;i++){
    const pressed=(data.buttons[i]===0);
    const statusEl=document.getElementById('btn'+i+'Status');
    if(statusEl){
      statusEl.className='button-status '+(pressed?'button-pressed':'button-released');
      statusEl.nextSibling.textContent=pressed?'Appuye':'Relache';
    }
  }
  document.getElementById('relayStatus').className='status-indicator '+(data.relay?'status-on':'status-off');
  document.getElementById('relayText').textContent=data.relay?'Active':'Desactive';
 }catch(e){ console.error('Error fetching data:',e); }
}

async function toggleAutoRelay(enabled){
 try{
  const r=await fetch('/api/auto-relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled})});
  if(!r.ok) alert('Erreur lors de la configuration du controle automatique');
 }catch(e){ console.error('Error toggling auto relay:',e); }
}

async function loadButtonConfig(){
 try{
  const r=await fetch('/api/button-config');
  const data=await r.json();
  buttonSessions=data.sessions;
  generateButtonConfig();
 }catch(e){ console.error('Error loading button config:',e); }
}

function generateButtonConfig(){
 const container=document.getElementById('buttonConfig');
 container.innerHTML='';
 for(let i=0;i<5;i++){
   const div=document.createElement('div');
   div.className='button-config';
   const label=document.createElement('label');
   label.textContent='Bouton '+(i+1)+':';
   const input=document.createElement('input');
   input.type='text'; input.id='session'+i; input.value=buttonSessions[i];
   input.placeholder='Ex: DayOBananaBoat';
   const button=document.createElement('button');
   button.textContent='Sauver';
   button.onclick=function(){ saveButtonConfig(i); };
   div.appendChild(label); div.appendChild(input); div.appendChild(button);
   container.appendChild(div);
 }
 const statusContainer=document.getElementById('buttonStatus');
 statusContainer.innerHTML='';
 for(let i=0;i<5;i++){
   const div=document.createElement('div'); div.style.margin='5px 0';
   const statusSpan=document.createElement('span'); statusSpan.className='button-status'; statusSpan.id='btn'+i+'Status';
   const textSpan=document.createElement('span'); textSpan.id='btn'+i+'Text'; textSpan.textContent='-';
   div.appendChild(statusSpan);
   div.appendChild(document.createTextNode('Bouton '+(i+1)+': '));
   div.appendChild(textSpan);
   div.appendChild(document.createTextNode(' -> "'+(buttonSessions[i]||'Non configure')+'"'));
   statusContainer.appendChild(div);
 }
}

async function saveButtonConfig(i){
 const sessionValue=document.getElementById('session'+i).value;
 try{
  const r=await fetch('/api/button-config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({button:i,session:sessionValue})});
  if(r.ok){ buttonSessions[i]=sessionValue; generateButtonConfig(); alert('Configuration sauvegardee!'); }
  else{ alert('Erreur lors de la sauvegarde'); }
 }catch(e){ console.error('Error saving config:',e); alert('Erreur lors de la sauvegarde'); }
}

async function setRelay(state){
 try{
  const r=await fetch('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:state})});
  if(!r.ok) alert('Erreur lors du controle du relais');
 }catch(e){ console.error('Error controlling relay:',e); }
}

loadButtonConfig();
fetchData();
setInterval(fetchData,1000);
</script>
</body>
</html>
)rawliteral";

#endif