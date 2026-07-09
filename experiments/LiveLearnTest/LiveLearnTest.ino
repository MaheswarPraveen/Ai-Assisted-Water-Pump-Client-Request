/* ============================================================================
 *  LiveLearnTest — Autotest-only: the board drives its own ON/OFF rhythm on a
 *  repeating 15-second cycle and trains continuously while it runs. Then
 *  Start Demo replays the learned pattern on the real relay.
 *
 *  Real accurate clock throughout (NTP) -- the "cycle" is real elapsed
 *  seconds, wrapped every CYCLE_S seconds, so a rhythm can repeat and be
 *  learned within a short session.
 *
 *  Relay on GPIO26 (same transistor-buffer wiring as SmartPump: HIGH=ON).
 *  Reuses SmartPump's saved WiFi creds for NTP -- connect SmartPump to your
 *  WiFi at least once first.
 * ========================================================================== */

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include "esp_random.h"
#include "esp_netif.h"

#define PIN_RELAY  26
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW
#define TZ_INFO "IST-5:30"

#define CYCLE_S 15            // repeating cycle length in seconds
#define SAMPLE_MS 200UL        // train 5x/second -- fine enough to hit every position in the cycle
#define LR 0.06f
#define READY_MS (1UL*CYCLE_S*1000UL)   // Start Demo unlocks after one full lap of autotest
#define DEMO_MS  (1UL*60UL*1000UL)      // demo auto-stops after 1 min
#define DEMO_STEP_MS 100UL

// Autotest pattern within each 15s cycle (elapsed seconds 0..14):
//   3-6   ON
//   7-10  off
//   11-14 ON
//   0-2   off
bool autoTestPattern(int sec){ return (sec>=3 && sec<=6) || (sec>=11 && sec<=14); }

const char* AP_SSID="SmartPumpTest";
const char* AP_PASS="test12345";

/* ---- same tiny network as SmartPump (4 -> 8 -> 8 -> 1) ---- */
#define NI 4
#define NH1 8
#define NH2 8
float W1[NI][NH1], bias1[NH1];
float W2[NH1][NH2], bias2[NH2];
float W3[NH2], bias3;
float Z1[NH1], A1[NH1], Z2[NH2], A2[NH2], OUTv;
float rnd01(){ return (float)esp_random()/4294967295.0f; }
float relu(float x){ return x>0?x:0; }
void netInit(){
  float r1=sqrtf(6.0f/(NI+NH1)), r2=sqrtf(6.0f/(NH1+NH2)), r3=sqrtf(6.0f/(NH2+1));
  for(int i=0;i<NI;i++)for(int j=0;j<NH1;j++)W1[i][j]=(rnd01()*2-1)*r1;
  for(int j=0;j<NH1;j++)for(int k=0;k<NH2;k++)W2[j][k]=(rnd01()*2-1)*r2;
  for(int k=0;k<NH2;k++)W3[k]=(rnd01()*2-1)*r3;
  for(int j=0;j<NH1;j++)bias1[j]=0;
  for(int k=0;k<NH2;k++)bias2[k]=0;
  bias3=0;
}
float netForward(const float x[NI]){
  for(int j=0;j<NH1;j++){ float s=bias1[j]; for(int i=0;i<NI;i++)s+=x[i]*W1[i][j]; Z1[j]=s; A1[j]=relu(s); }
  for(int k=0;k<NH2;k++){ float s=bias2[k]; for(int j=0;j<NH1;j++)s+=A1[j]*W2[j][k]; Z2[k]=s; A2[k]=relu(s); }
  float s=bias3; for(int k=0;k<NH2;k++)s+=A2[k]*W3[k];
  OUTv=1.0f/(1.0f+expf(-s)); return OUTv;
}
void netTrain(const float x[NI], float target, float lr){
  netForward(x);
  float dz3=OUTv-target;
  float dZ2[NH2]; for(int k=0;k<NH2;k++)dZ2[k]=(dz3*W3[k])*(Z2[k]>0?1:0);
  float dZ1[NH1];
  for(int j=0;j<NH1;j++){ float s=0; for(int k=0;k<NH2;k++)s+=dZ2[k]*W2[j][k]; dZ1[j]=s*(Z1[j]>0?1:0); }
  for(int k=0;k<NH2;k++)W3[k]-=lr*dz3*A2[k]; bias3-=lr*dz3;
  for(int j=0;j<NH1;j++)for(int k=0;k<NH2;k++)W2[j][k]-=lr*dZ2[k]*A1[j];
  for(int k=0;k<NH2;k++)bias2[k]-=lr*dZ2[k];
  for(int i=0;i<NI;i++)for(int j=0;j<NH1;j++)W1[i][j]-=lr*dZ1[j]*x[i];
  for(int j=0;j<NH1;j++)bias1[j]-=lr*dZ1[j];
}
// Position within the REPEATING cycle (real elapsed seconds, wrapped).
void feat(int cyclePos,float x[NI]){
  x[0]=sinf(2*PI*cyclePos/(float)CYCLE_S); x[1]=cosf(2*PI*cyclePos/(float)CYCLE_S);
  x[2]=0.5f; x[3]=0.5f;   // unused second dimension, kept for network shape
}
float predAt(int cyclePos){ float x[NI]; feat(cyclePos,x); return netForward(x); }
bool nowClock(){ struct tm t; return getLocalTime(&t,10); }   // just for the "real time" display
int cyclePosNow(uint32_t sinceMs){ return (int)((millis()-sinceMs)/1000UL)%CYCLE_S; }

Preferences prefs;
WebServer server(80);
void relay(bool on){ digitalWrite(PIN_RELAY, on?RELAY_ON:RELAY_OFF); }
void setStaDefault(){ esp_netif_t* sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); if(sta)esp_netif_set_default_netif(sta); }

enum Mode { M_IDLE, M_DEMO };   // no separate "test" mode -- autotest IS the training
Mode mode=M_IDLE;
uint32_t demoStartMs=0, sampleCount=0;
int demoPos=0;                 // cycle position (0..CYCLE_S-1) the demo is currently showing
bool readyForDemo=false;
bool manualDemand=false;

bool autoTestOn=false;
uint32_t autoTestStartMs=0;
uint16_t trainCount[CYCLE_S]={0};

/* ---- dashboard ---- */
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LiveLearnTest</title><style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;color:#e8eefc;padding:20px;min-height:100vh;
background:radial-gradient(1200px 800px at 15% -10%,#1b2a4a 0,transparent 60%),radial-gradient(1000px 700px at 110% 10%,#3a1b4a 0,transparent 55%),#0a0f1e}
h1{font-size:1.25rem;margin:0 0 14px}
.card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:18px;padding:18px;margin-bottom:14px;max-width:480px;backdrop-filter:blur(14px)}
.row{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid rgba(255,255,255,.06);font-size:.9rem}
.row:last-child{border:none}.k{color:#8b97b5}.v{font-weight:600}
.bar{height:10px;background:rgba(255,255,255,.08);border-radius:6px;overflow:hidden;margin-top:6px}
.bar>div{height:100%;background:linear-gradient(90deg,#38bdf8,#818cf8);transition:width .2s linear}
canvas{width:100%;display:block;border-radius:10px;margin-top:10px}
.big{font-size:1.6rem;font-weight:700}
.countdown{font-size:2.2rem;font-weight:700;font-variant-numeric:tabular-nums;text-align:center;margin:8px 0}
.on{color:#22c55e}.off{color:#8b97b5}.ready{color:#9ff5bd}
button{padding:14px;border-radius:12px;border:none;font-weight:700;font-size:1rem;cursor:pointer}
.btns{display:flex;gap:8px}
.btns button{flex:1}
.onbtn{background:rgba(34,197,94,.18);border:1px solid #22c55e!important;color:#9ff5bd}
.offbtn{background:rgba(239,68,68,.15);border:1px solid #ef4444!important;color:#ffb4b4}
.startbtn{width:100%;margin-top:8px;background:linear-gradient(90deg,#38bdf8,#818cf8);color:#08111f}
.demobtn{width:100%;margin-top:8px;background:linear-gradient(90deg,#22c55e,#16a34a);color:#08111f}
button:disabled{opacity:.4;cursor:not-allowed}
</style></head><body>
<h1>&#9889; Live Learning Test</h1>

<div class="card">
  <div class="row"><span class="k">Real time</span><span class="v" id="time">--</span></div>
  <div class="row"><span class="k">Mode</span><span class="v" id="mode">--</span></div>
  <div class="row"><span class="k">Demand</span><span class="v" id="demand">--</span></div>
  <div class="row"><span class="k">Relay</span><span class="v" id="relay">--</span></div>
  <div class="row"><span class="k">Samples trained</span><span class="v" id="samples">--</span></div>
</div>

<div class="card">
  <div class="row"><span class="k">Autotest</span><span class="v" id="autoStatus">off</span></div>
  <div class="countdown" id="lapPos">0 / 15s</div>
  <div class="bar"><div id="bar" style="width:0%"></div></div>
  <div class="btns" style="margin-top:10px">
    <button class="startbtn" style="margin-top:0" onclick="startAutotest()">Start Autotest</button>
    <button class="offbtn" onclick="stopAutotest()">Stop Autotest</button>
  </div>
</div>

<div class="card">
  <div class="row"><span class="k">Live prediction</span><span class="v big" id="pred">--</span></div>
  <div class="row" id="demoPosRow" style="display:none"><span class="k">Demo at second</span><span class="v" id="demoPosV">--</span></div>
  <button class="demobtn" id="demoBtn" onclick="startDemo()" disabled>Start Demo (AI drives relay)</button>
  <button class="offbtn" id="stopDemoBtn" onclick="stopDemo()" style="width:100%;margin-top:8px;display:none">Stop Demo</button>
</div>

<div class="card">
  <h2 style="font-size:.9rem;color:#8b97b5;margin:0 0 8px">Learned pattern (this 15s cycle)</h2>
  <canvas id="chart" height="120"></canvas>
  <div class="k" id="summary" style="margin-top:8px;font-size:.85rem">--</div>
</div>

<script>
async function startDemo(){ await fetch('/api/start_demo',{method:'POST'}); tick(); }
async function stopDemo(){ await fetch('/api/stop_demo',{method:'POST'}); tick(); }
async function startAutotest(){ await fetch('/api/autotest_start',{method:'POST'}); tick(); }
async function stopAutotest(){ await fetch('/api/autotest_stop',{method:'POST'}); tick(); }
async function tick(){
 try{
  const d=await (await fetch('/api/status')).json();
  document.getElementById('time').textContent=d.time;
  document.getElementById('mode').textContent=d.mode;
  const dem=document.getElementById('demand'); dem.textContent=d.demand?'ON':'off'; dem.className='v '+(d.demand?'on':'off');
  const rel=document.getElementById('relay'); rel.textContent=d.relay?'ON':'off'; rel.className='v '+(d.relay?'on':'off');
  document.getElementById('samples').textContent=d.samples;
  document.getElementById('pred').textContent=(Math.round(d.pred*1000)/10)+'%';
  document.getElementById('demoPosRow').style.display=(d.mode=='DEMO')?'flex':'none';
  document.getElementById('demoPosV').textContent=d.demoPos+'s';
  document.getElementById('autoStatus').textContent=d.autotest?'running':'off';
  document.getElementById('lapPos').textContent=(d.autotest?d.lapSec:0)+' / '+d.cycleLen+'s';
  document.getElementById('bar').style.width=(d.autotest?(d.lapSec*100/d.cycleLen):0)+'%';
  document.getElementById('demoBtn').disabled=(!d.ready||d.mode=='DEMO');
  document.getElementById('demoBtn').textContent=(d.mode=='DEMO')?'Demo running…':'Start Demo (AI drives relay)';
  document.getElementById('stopDemoBtn').style.display=(d.mode=='DEMO')?'block':'none';
 }catch(e){}
}
async function drawChart(){
 try{
  const d=await (await fetch('/api/minutes')).json();
  const arr=d.pred, cnt=d.count, N=arr.length;
  const cv=document.getElementById('chart'),ctx=cv.getContext('2d');
  const W=cv.width=cv.clientWidth,H=cv.height;
  ctx.clearRect(0,0,W,H);
  const bw=W/N, maxV=Math.max(...arr,0.05);
  for(let s=0;s<N;s++){
    const v=arr[s], bh=Math.max((v/maxV)*(H-26),1);
    ctx.fillStyle='rgba(56,189,248,'+(0.35+0.5*(v/maxV))+')';
    ctx.fillRect(s*bw+0.5,H-bh-22,Math.max(bw-1,1),bh);
    ctx.fillStyle='#cbd5e1'; ctx.font='8px system-ui'; ctx.textAlign='center';
    ctx.fillText((Math.round(v*1000)/10), s*bw+bw/2, H-bh-24<8?10:H-bh-25);
    ctx.fillStyle=cnt[s]>0?'#64748b':'#3a4256'; ctx.font='9px system-ui';
    ctx.fillText('n='+cnt[s], s*bw+bw/2, H-2);
  }
  const idxs=[...Array(N).keys()].sort((a,b)=>arr[b]-arr[a]);
  const on=idxs.filter(s=>arr[s]>0.5).sort((a,b)=>a-b);
  const summary=document.getElementById('summary');
  summary.textContent = on.length ? ('Predicted ON at second(s): '+on.join(', ')+' of the '+N+'s cycle. Others: off.')
                                   : 'No position has crossed 50% yet — start autotest and wait a few laps.';
 }catch(e){}
}
tick(); drawChart();
setInterval(tick,300); setInterval(drawChart,1000);
</script></body></html>
)HTML";

void handleRoot(){ server.send_P(200,"text/html",PAGE); }

void handleStatus(){
  char timeStr[24]="no NTP yet";
  if(nowClock()){ struct tm t; getLocalTime(&t,10); strftime(timeStr,sizeof(timeStr),"%H:%M:%S",&t); }
  int lapSec = autoTestOn ? cyclePosNow(autoTestStartMs) : 0;
  float pred = (mode==M_DEMO) ? predAt(demoPos) : predAt(lapSec);
  bool relayOn=(digitalRead(PIN_RELAY)==RELAY_ON);
  String modeStr = mode==M_IDLE?"IDLE":"DEMO";

  String j="{";
  j+="\"time\":\""+String(timeStr)+"\",";
  j+="\"mode\":\""+modeStr+"\",";
  j+="\"demand\":"+String(manualDemand?"true":"false")+",";
  j+="\"relay\":"+String(relayOn?"true":"false")+",";
  j+="\"samples\":"+String(sampleCount)+",";
  j+="\"pred\":"+String(pred,3)+",";
  j+="\"demoPos\":"+String(demoPos)+",";
  j+="\"autotest\":"+String(autoTestOn?"true":"false")+",";
  j+="\"lapSec\":"+String(lapSec)+",";
  j+="\"cycleLen\":"+String(CYCLE_S)+",";
  j+="\"ready\":"+String(readyForDemo?"true":"false");
  j+="}";
  server.send(200,"application/json",j);
}
void handleMinutes(){
  String jp="[",jc="[";
  for(int mi=0;mi<CYCLE_S;mi++){
    if(mi){ jp+=","; jc+=","; }
    jp+=String(predAt(mi),4);
    jc+=String(trainCount[mi]);
  }
  jp+="]"; jc+="]";
  server.send(200,"application/json","{\"pred\":"+jp+",\"count\":"+jc+"}");
}
void handleStartDemo(){
  if(!readyForDemo){ server.send(409,"text/plain","run autotest for at least one lap first"); return; }
  mode=M_DEMO; demoStartMs=millis(); demoPos=0;
  server.send(200,"text/plain","ok");
}
void handleStopDemo(){
  mode=M_IDLE; relay(false);
  server.send(200,"text/plain","ok");
}
void handleAutotestStart(){
  autoTestOn=true; autoTestStartMs=millis();
  server.send(200,"text/plain","ok");
}
void handleAutotestStop(){
  autoTestOn=false;
  if(mode!=M_DEMO){ manualDemand=false; relay(false); }
  server.send(200,"text/plain","ok");
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n=== LiveLearnTest (autotest-only) ===");
  pinMode(PIN_RELAY, OUTPUT); relay(false);
  netInit();

  prefs.begin("smartpump", true);
  String ssid=prefs.getString("wifi_ssid","");
  String pass=prefs.getString("wifi_pass","");
  prefs.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID,AP_PASS);
  Serial.printf("Hotspot \"%s\" at %s\n",AP_SSID,WiFi.softAPIP().toString().c_str());

  if(ssid.length()==0){
    Serial.println("No saved WiFi creds — connect SmartPump to WiFi at least once first.");
  } else {
    WiFi.begin(ssid.c_str(),pass.c_str());
    Serial.printf("Joining \"%s\"",ssid.c_str());
    for(int i=0;i<20 && WiFi.status()!=WL_CONNECTED;i++){ delay(300); Serial.print("."); }
    if(WiFi.status()==WL_CONNECTED){
      Serial.printf(" ok %s\n",WiFi.localIP().toString().c_str());
      setStaDefault();
      configTzTime(TZ_INFO,"216.239.35.0","216.239.35.4","pool.ntp.org");
      struct tm t; int i=0; while(!getLocalTime(&t,500)&&i<24){Serial.print(".");i++;}
      if(getLocalTime(&t,100)) Serial.printf("NTP ok: %02d:%02d:%02d\n",t.tm_hour,t.tm_min,t.tm_sec);
      else Serial.println("NTP failed");
    } else Serial.println(" failed to connect");
  }

  server.on("/",handleRoot);
  server.on("/api/status",handleStatus);
  server.on("/api/minutes",handleMinutes);
  server.on("/api/start_demo",HTTP_POST,handleStartDemo);
  server.on("/api/stop_demo",HTTP_POST,handleStopDemo);
  server.on("/api/autotest_start",HTTP_POST,handleAutotestStart);
  server.on("/api/autotest_stop",HTTP_POST,handleAutotestStop);
  server.begin();
  Serial.println("Dashboard: http://192.168.4.1\n");
}

uint32_t tSample=0, tDemo=0, tAuto=0;

void loop(){
  server.handleClient();
  uint32_t now=millis();

  if(autoTestOn){
    // Drive relay/demand from the pattern (fine-grained, so ON/OFF edges land close to the real second)
    if(now-tAuto>=50){
      tAuto=now;
      bool want=autoTestPattern(cyclePosNow(autoTestStartMs));
      if(want!=manualDemand){ manualDemand=want; if(mode!=M_DEMO) relay(manualDemand); }
    }
    // Train continuously, fast enough to hit every position in the cycle every lap
    if(now-tSample>=SAMPLE_MS){
      tSample=now;
      int cp=cyclePosNow(autoTestStartMs);
      float x[NI]; feat(cp,x);
      netTrain(x, manualDemand?1.0f:0.0f, LR);
      sampleCount++;
      if(trainCount[cp]<65535) trainCount[cp]++;
      if(!readyForDemo && now-autoTestStartMs>=READY_MS) readyForDemo=true;
    }
  }

  if(mode==M_DEMO){
    if(now-demoStartMs>=DEMO_MS){
      mode=M_IDLE; relay(false);
      Serial.println(">>> Demo window ended. <<<");
    } else if(now-tDemo>=DEMO_STEP_MS){
      tDemo=now;
      demoPos = cyclePosNow(demoStartMs);
      float p=predAt(demoPos);
      relay(p>=0.5f);
    }
  }
}
