/* ============================================================================
 *  SmartPump — tank-fill controller with a REAL on-device neural network
 *  ESP32 DevKitV1 · single tethered float · relay · WiFi/NTP · web dashboard
 *
 *  - Pump control: float low -> pump ON, float full -> pump OFF, max-runtime
 *    fault. Works with no WiFi. Manual override (auto/on/off), safety-guarded.
 *  - Neural net (hand-written C: forward + backprop + SGD): learns your pump
 *    pattern LIVE on the chip from real events, weights persisted to flash.
 *  - Glassmorphism dashboard at http://192.168.4.1 (own hotspot) — live status,
 *    activity timeline, usage stats, learned heatmap, predictions, controls.
 * ========================================================================== */

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include "esp_random.h"
#include "esp_netif.h"

/* ---------- CONFIG -------------------------------------------------------- */
#define TZ_INFO   "IST-5:30"                 // change for your timezone
const char* AP_SSID = "SmartPump";
const char* AP_PASS = "pump12345";
const char* WIFI_SSID = "";                  // set live via /wifi
const char* WIFI_PASS = "";

#define PIN_FLOAT   25
#define PIN_RELAY   26
#define PIN_LED      2
// NPN transistor buffer inverts the drive: GPIO HIGH -> transistor on ->
// relay IN pulled low -> active-LOW relay ENERGIZES. So GPIO HIGH = pump ON.
#define RELAY_ON    HIGH
#define RELAY_OFF   LOW
#define FLOAT_CALL_LEVEL  LOW                 // raw read meaning "water low"

uint32_t MAX_RUN_MS   = 15UL*60*1000;         // adjustable via dashboard
#define FAULT_COOLDOWN (10UL*60*1000)
#define DEBOUNCE_MS     500
#define LEARN_SAMPLE_MS 60000UL               // train NN once/min
#define AUTO_READY_SAMPLES 10UL               // TEST value: Auto unlocks almost immediately for bench testing.
                                              // For production set to 30240UL (~3 weeks of one-sample-per-minute data).
#define CHART_MIN_SAMPLES  200UL              // separate from AUTO_READY_SAMPLES so the "typical day" chart
                                              // only appears once there's real data, regardless of the auto-gate test value
#define SAVE_MS         600000UL              // persist every 10 min

/* ---------- TYPES (must precede any function that uses them) ------------- */
struct Deb { uint8_t pin; bool logical,lastRaw; uint32_t t; };

/* ---------- NEURAL NET (4 -> 8 -> 8 -> 1) -------------------------------- */
#define NI 4
#define NH1 8
#define NH2 8
float W1[NI][NH1], bias1[NH1];
float W2[NH1][NH2], bias2[NH2];
float W3[NH2], bias3;
float Z1[NH1], A1[NH1], Z2[NH2], A2[NH2], OUTv;
#define NWEIGHTS (NI*NH1 + NH1 + NH1*NH2 + NH2 + NH2 + 1)

float rnd01() { return (float)esp_random() / 4294967295.0f; }
float relu(float x){ return x>0?x:0; }

void netInit() {
  float r1=sqrtf(6.0f/(NI+NH1)), r2=sqrtf(6.0f/(NH1+NH2)), r3=sqrtf(6.0f/(NH2+1));
  for(int i=0;i<NI;i++)for(int j=0;j<NH1;j++)W1[i][j]=(rnd01()*2-1)*r1;
  for(int j=0;j<NH1;j++)for(int k=0;k<NH2;k++)W2[j][k]=(rnd01()*2-1)*r2;
  for(int k=0;k<NH2;k++)W3[k]=(rnd01()*2-1)*r3;
  for(int j=0;j<NH1;j++)bias1[j]=0;
  for(int k=0;k<NH2;k++)bias2[k]=0;
  bias3=0;
}
float netForward(const float x[NI]) {
  for(int j=0;j<NH1;j++){ float s=bias1[j]; for(int i=0;i<NI;i++)s+=x[i]*W1[i][j]; Z1[j]=s; A1[j]=relu(s); }
  for(int k=0;k<NH2;k++){ float s=bias2[k]; for(int j=0;j<NH1;j++)s+=A1[j]*W2[j][k]; Z2[k]=s; A2[k]=relu(s); }
  float s=bias3; for(int k=0;k<NH2;k++)s+=A2[k]*W3[k];
  OUTv=1.0f/(1.0f+expf(-s)); return OUTv;
}
void netTrain(const float x[NI], float target, float lr) {
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
void feat(int h, int d, float x[NI]) {
  x[0]=sinf(2*PI*h/24); x[1]=cosf(2*PI*h/24);
  x[2]=sinf(2*PI*d/7);  x[3]=cosf(2*PI*d/7);
}
// pack/unpack all weights into a flat buffer for flash storage
void netPack(float* buf){ int n=0;
  for(int i=0;i<NI;i++)for(int j=0;j<NH1;j++)buf[n++]=W1[i][j];
  for(int j=0;j<NH1;j++)buf[n++]=bias1[j];
  for(int j=0;j<NH1;j++)for(int k=0;k<NH2;k++)buf[n++]=W2[j][k];
  for(int k=0;k<NH2;k++)buf[n++]=bias2[k];
  for(int k=0;k<NH2;k++)buf[n++]=W3[k]; buf[n++]=bias3;
}
void netUnpack(const float* buf){ int n=0;
  for(int i=0;i<NI;i++)for(int j=0;j<NH1;j++)W1[i][j]=buf[n++];
  for(int j=0;j<NH1;j++)bias1[j]=buf[n++];
  for(int j=0;j<NH1;j++)for(int k=0;k<NH2;k++)W2[j][k]=buf[n++];
  for(int k=0;k<NH2;k++)bias2[k]=buf[n++];
  for(int k=0;k<NH2;k++)W3[k]=buf[n++]; bias3=buf[n++];
}

/* ---------- STATE -------------------------------------------------------- */
Preferences prefs;
WebServer server(80);
String savedSSID, savedPass;

enum PumpState { IDLE, FILLING, FAULT };
PumpState state = IDLE;
enum Manual { M_AUTO, M_ON, M_OFF };
Manual manual = M_AUTO;

uint32_t fillStart=0, faultSince=0;
bool fillIsManual=false;   // was the current/last fill started by the ON button?
#define MIN_LEARN_MS 60000UL   // manual runs shorter than this are ignored as training data (accidental/test presses)
uint32_t sampleCount=0;
uint32_t fillsTotal=0, fillsToday=0, fillsWeek=0;
uint32_t totalRunSec=0, lastFillDurMs=0, lastFillMs=0;
uint32_t fillDurSumSec=0;                 // for average
int curDay=-1, curWeek=-1;
uint32_t faultCount=0; String lastFault="none"; uint32_t lastFaultMs=0;

Deb debFloat;

// event ring buffer (for timeline + CSV): type 0=off,1=on,2=fault
struct Ev { uint32_t ms; uint8_t type; };
#define NEV 240
Ev evbuf[NEV]; int evHead=0, evCount=0;
void logEvent(uint8_t type){ evbuf[evHead]={millis(),type}; evHead=(evHead+1)%NEV; if(evCount<NEV)evCount++; }

uint32_t tLearn=0,tSave=0,tTick=0,tLed=0,tNtp=0;

/* ---------- HELPERS ------------------------------------------------------ */
void relay(bool on){ digitalWrite(PIN_RELAY, on?RELAY_ON:RELAY_OFF); }
void debInit(Deb&d,uint8_t p){ d.pin=p; d.lastRaw=digitalRead(p); d.logical=d.lastRaw; d.t=millis(); }
void debUpd(Deb&d){ bool r=digitalRead(d.pin); if(r!=d.lastRaw){d.lastRaw=r;d.t=millis();} if(millis()-d.t>=DEBOUNCE_MS)d.logical=r; }
bool callForWater(){ return debFloat.logical==FLOAT_CALL_LEVEL; }
bool haveTime(){ struct tm t; return getLocalTime(&t,10); }
int currentBin(int&wday,int&hour){ struct tm t; if(!getLocalTime(&t,10))return -1; wday=t.tm_wday; hour=t.tm_hour; return wday*24+hour; }

void saveAll(){
  float buf[NWEIGHTS]; netPack(buf);
  prefs.putBytes("nn", buf, sizeof(buf));
  prefs.putUInt("samples",sampleCount);
  prefs.putUInt("ftot",fillsTotal);
  prefs.putUInt("fday",fillsToday); prefs.putUInt("fweek",fillsWeek);
  prefs.putInt("cday",curDay); prefs.putInt("cweek",curWeek);
  prefs.putUInt("runsec",totalRunSec); prefs.putUInt("dursum",fillDurSumSec);
}

/* ---------- STATS: day/week rollover ------------------------------------- */
void rolloverCheck(){
  struct tm t; if(!getLocalTime(&t,10)) return;
  int day=t.tm_yday, wk=t.tm_yday - t.tm_wday;
  if(curDay!=day){ curDay=day; fillsToday=0; }
  if(curWeek!=wk){ curWeek=wk; fillsWeek=0; }
}

/* ---------- PUMP STATE MACHINE ------------------------------------------- *
 *  Modes:  ON   = pump runs until you press OFF (or max-runtime safety cutoff)
 *          OFF  = pump stays off
 *          AUTO = float controls it (low -> on, full -> off)
 *  Max-runtime is a hard safety backstop in every running mode.
 * ------------------------------------------------------------------------- */
void setFault(const char* why){ state=FAULT; faultSince=millis(); faultCount++; lastFault=why; lastFaultMs=millis(); relay(false); logEvent(2); }
void endFill(){ state=IDLE; lastFillMs=millis(); lastFillDurMs=lastFillMs-fillStart;
  totalRunSec+=lastFillDurMs/1000; fillDurSumSec+=lastFillDurMs/1000; logEvent(0); }

void updatePump(){
  bool low = callForWater();
  switch(state){
    case IDLE:
      relay(false);
      if(manual==M_OFF) break;
      if(manual==M_ON || low){                 // ON = run now; AUTO = run when low
        state=FILLING; fillStart=millis(); fillIsManual=(manual==M_ON);
        fillsTotal++; fillsToday++; fillsWeek++; logEvent(1);
      }
      break;
    case FILLING:
      relay(true);
      if(manual==M_OFF){ endFill(); }                            // OFF pressed -> stop
      else if(manual==M_ON){                                     // ON -> stay on (like a switch)
        if(millis()-fillStart>MAX_RUN_MS) setFault("max runtime safety cutoff");
      } else {                                                   // AUTO -> float decides
        if(!low) endFill();                                      // tank full -> stop
        else if(millis()-fillStart>MAX_RUN_MS) setFault("max runtime (dry source / stuck float)");
      }
      break;
    case FAULT:
      relay(false);
      if(millis()-faultSince>FAULT_COOLDOWN){
        state=IDLE;
        manual=M_AUTO;   // a fault means something ran unattended too long — hand control back to the float instead of silently repeating the same manual mode
      }
      break;
  }
}

/* ---------- LEARNER: train NN live on real events ------------------------ *
 *  Every state sample trains the network — including MANUAL on/off. A
 *  deliberate manual switch is genuine demand (e.g. you turn the pump on at
 *  the same time each day), so it counts toward the learned pattern just like
 *  automatic fills do.
 * ------------------------------------------------------------------------- */
void updateLearner(){
  int wday,hour; int idx=currentBin(wday,hour); if(idx<0)return;
  float x[NI]; feat(hour,wday,x);
  netTrain(x, (state==FILLING)?1.0f:0.0f, 0.02f);
  sampleCount++;
}

/* ---------- TARIFF (time-of-use electricity pricing) ---------------------- *
 *  Normal 6am-6pm: 90%, Peak 6pm-10pm: 125%, Off-Peak 10pm-6am: 100% of the
 *  standard ruling tariff. The float switch only reports low/full (no partial
 *  level), so AUTO mode can never safely *defer* a fill once water is called
 *  for -- this only informs the dashboard display and biases which upcoming
 *  hour gets recommended when several hours look equally likely to need water.
 * ------------------------------------------------------------------------- */
float tariffMult(int h){ if(h>=18&&h<22)return 1.25f; if(h>=6&&h<18)return 0.90f; return 1.00f; }
const char* tariffLabel(int h){ if(h>=18&&h<22)return "Peak"; if(h>=6&&h<18)return "Normal"; return "Off-Peak"; }

/* ---------- PREDICTIONS --------------------------------------------------- */
float predAt(int h,int d){ float x[NI]; feat(h,d,x); return netForward(x); }
// next hour (from now) whose prediction crosses a sensible threshold, preferring
// cheaper tariff hours when several upcoming hours are similarly likely
int nextPumpHoursAhead(){
  int wday,hour; if(currentBin(wday,hour)<0) return -1;
  float mx=0; for(int d=0;d<7;d++)for(int h=0;h<24;h++)mx=fmaxf(mx,predAt(h,d));
  float thr=fmaxf(0.15f, mx*0.5f);
  int best=-1; float bestTariff=999;
  for(int a=1;a<=48;a++){
    int h=(hour+a)%24, d=(wday+((hour+a)/24))%7;
    if(predAt(h,d)>=thr){
      if(best<0) best=a;                          // first qualifying hour, as before
      // within the first 6 candidate hours, prefer whichever is cheapest
      if(a-best<=6 && tariffMult(h)<bestTariff){ bestTariff=tariffMult(h); best=a; }
      if(a-best>6) break;
    }
  }
  return best;
}

/* ---------- WEB: dashboard page (dark glassmorphism) --------------------- */
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartPump</title>
<style>
:root{--glass:rgba(255,255,255,.06);--stroke:rgba(255,255,255,.12);--txt:#e8eefc;--dim:#8b97b5;--accent:#38bdf8;--on:#22c55e;--fault:#ef4444}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,sans-serif;color:var(--txt);
 background:radial-gradient(1200px 800px at 15% -10%,#1b2a4a 0%,transparent 60%),
            radial-gradient(1000px 700px at 110% 10%,#3a1b4a 0%,transparent 55%),#0a0f1e;
 min-height:100vh;padding:18px;background-attachment:fixed}
h1{font-size:1.25rem;margin:0 0 2px;display:flex;align-items:center;gap:8px}
.sub{color:var(--dim);font-size:.8rem;margin-bottom:16px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}
.card{background:var(--glass);border:1px solid var(--stroke);border-radius:18px;padding:16px;
 backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);box-shadow:0 8px 32px rgba(0,0,0,.35)}
.card h2{font-size:.72rem;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:0 0 12px}
.big{display:flex;align-items:center;gap:14px}
.dot{width:16px;height:16px;border-radius:50%;box-shadow:0 0 16px currentColor}
.state{font-size:1.5rem;font-weight:700}
.muted{color:var(--dim);font-size:.8rem}
.row{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid rgba(255,255,255,.06);font-size:.9rem}
.row:last-child{border:none}.row .k{color:var(--dim)}.row .v{font-weight:600}
.stats{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;text-align:center}
.stat{background:rgba(255,255,255,.04);border-radius:12px;padding:10px}
.stat .n{font-size:1.5rem;font-weight:700}.stat .l{font-size:.68rem;color:var(--dim)}
.bar{height:9px;background:rgba(255,255,255,.08);border-radius:6px;overflow:hidden;margin-top:6px}
.bar>div{height:100%;background:linear-gradient(90deg,#38bdf8,#818cf8);border-radius:6px;transition:width .4s}
canvas{width:100%;display:block;border-radius:10px}
.btns{display:flex;gap:8px;flex-wrap:wrap}
button{flex:1;min-width:90px;padding:11px;border-radius:12px;border:1px solid var(--stroke);
 background:var(--glass);color:var(--txt);font-weight:600;cursor:pointer;font-size:.85rem;transition:.15s}
button:hover{background:rgba(255,255,255,.14)}
button.on{border-color:var(--on);color:#9ff5bd}button.off{border-color:var(--fault);color:#ffb4b4}
button.act{background:var(--accent);color:#08111f;border-color:var(--accent)}
a.link{color:var(--accent);text-decoration:none;font-size:.85rem}
.pill{font-size:.68rem;padding:3px 9px;border-radius:20px;background:rgba(255,255,255,.08);color:var(--dim)}
</style></head><body>
<h1>&#128167; SmartPump</h1>
<div class="sub">on-device neural network &middot; live tank control</div>
<div class="btns" style="max-width:600px;margin-bottom:6px">
  <button class="on"  onclick="cmd('on')" style="font-size:1rem;padding:14px">&#9654; Pump ON</button>
  <button class="off" onclick="cmd('off')" style="font-size:1rem;padding:14px">&#9632; Stop now</button>
  <button class="act" id="autoBtn" onclick="cmd('auto')" style="font-size:1rem;padding:14px">Auto &#128274;</button>
</div>
<div id="autoWarn" style="display:none;max-width:600px;background:rgba(239,68,68,.15);border:1px solid #ef4444;color:#ffb4b4;padding:9px 12px;border-radius:11px;font-size:.8rem;margin-bottom:16px"></div>
<div class="grid">

  <div class="card">
    <h2>Live status</h2>
    <div class="big"><span class="dot" id="dot"></span>
      <div><div class="state" id="state">--</div>
      <div class="muted" id="stateSub">&nbsp;</div></div></div>
    <div class="row"><span class="k">Float</span><span class="v" id="float">--</span></div>
    <div class="row"><span class="k">Mode</span><span class="v" id="mode">--</span></div>
    <div class="row"><span class="k">Time</span><span class="v" id="time">--</span></div>
    <div class="row"><span class="k">Uptime</span><span class="v" id="uptime">--</span></div>
    <div class="row"><span class="k">WiFi</span><span class="v" id="wifi">--</span></div>
    <div class="row"><span class="k">Free memory</span><span class="v" id="heap">--</span></div>
    <div class="row"><span class="k">Tariff now</span><span class="v" id="tariff">--</span></div>
  </div>

  <div class="card">
    <h2>Activity (last hour)</h2>
    <canvas id="timeline" height="60"></canvas>
    <div class="muted" style="margin-top:8px">green = pump running</div>
  </div>

  <div class="card">
    <h2>Usage</h2>
    <div class="stats">
      <div class="stat"><div class="n" id="fToday">-</div><div class="l">today</div></div>
      <div class="stat"><div class="n" id="fWeek">-</div><div class="l">this week</div></div>
      <div class="stat"><div class="n" id="fTotal">-</div><div class="l">total fills</div></div>
    </div>
    <div class="row" style="margin-top:12px"><span class="k">Last fill</span><span class="v" id="lastFill">--</span></div>
    <div class="row"><span class="k">Avg fill</span><span class="v" id="avgFill">--</span></div>
    <div class="row"><span class="k">Total runtime</span><span class="v" id="runtime">--</span></div>
    <div class="row"><span class="k">Since last fill</span><span class="v" id="sinceFill">--</span></div>
    <div class="row"><span class="k">Predicted next</span><span class="v" id="nextPred">--</span></div>
  </div>

  <div class="card">
    <h2>AI &mdash; learned water pattern</h2>
    <div class="muted" style="margin:-4px 0 12px">A typical day, based on real pump activity. Taller bar = pump runs more often around that hour.</div>
    <canvas id="daychart" height="90"></canvas>
    <div class="muted" id="peakSummary" style="margin-top:8px">Not enough data yet to spot a pattern.</div>
    <div class="row" style="margin-top:12px"><span class="k">Chance pump needed now</span><span class="v" id="pNow">--</span></div>
    <div class="bar"><div id="pNowBar" style="width:0%"></div></div>
    <div class="row" style="margin-top:8px"><span class="k">Chance next hour</span><span class="v" id="pNext">--</span></div>
    <div class="bar"><div id="pNextBar" style="width:0%"></div></div>
    <div class="row" style="margin-top:8px"><span class="k">Dataset progress</span><span class="v" id="learn">--</span></div>
    <div class="bar"><div id="learnBar" style="width:0%"></div></div>
    <div class="muted" id="readyMsg" style="margin-top:6px"></div>
  </div>

  <div class="card">
    <h2>Health</h2>
    <div class="row"><span class="k">Fault count</span><span class="v" id="faults">--</span></div>
    <div class="row"><span class="k">Last fault</span><span class="v" id="lastFault">--</span></div>
    <div class="row"><span class="k">When</span><span class="v" id="faultWhen">--</span></div>
  </div>

  <div class="card">
    <h2>Controls</h2>
    <div class="muted" style="margin:-4px 0 12px">Pump ON / OFF / Auto buttons are at the top of the page.</div>
    <div class="btns" style="margin-bottom:10px">
      <button onclick="resetFault()">Reset fault</button>
      <button onclick="location.href='/api/csv'">Download CSV</button>
    </div>
    <div class="btns" style="margin-bottom:10px">
      <button onclick="location.href='/wificfg'">&#128246; WiFi settings</button>
    </div>
    <div class="row"><span class="k">Max runtime (min)</span>
      <span class="v"><input id="maxrun" type="number" min="1" max="120" style="width:64px;padding:5px;border-radius:8px;border:1px solid var(--stroke);background:var(--glass);color:var(--txt)">
      <button style="min-width:auto;flex:none;padding:6px 10px" onclick="setMax()">set</button></span></div>
  </div>

</div>
<script>
function fmt(s){return s;}
async function cmd(m){
  const r=await fetch('/api/pump?m='+m,{method:'POST'});
  if(!r.ok){ const msg=await r.text(); document.getElementById('autoWarn').style.display='block';
    document.getElementById('autoWarn').textContent='⚠ '+msg; return; }
  document.getElementById('autoWarn').style.display='none';
  tick();
}
async function resetFault(){await fetch('/api/fault',{method:'POST'});tick();}
async function setMax(){let v=document.getElementById('maxrun').value;await fetch('/api/settings?maxrun='+v,{method:'POST'});}
let lastLearnPct=0, lastChartReady=false;
async function tick(){
 try{
  const d=await (await fetch('/api/status')).json();
  lastLearnPct=d.learnPct;
  lastChartReady=d.chartReady;
  const st=document.getElementById('state'); st.textContent=d.state;
  const c=d.state=='FILLING'?getComputedStyle(document.documentElement).getPropertyValue('--on')
        :d.state=='FAULT'?getComputedStyle(document.documentElement).getPropertyValue('--fault')
        :'#8b97b5';
  st.style.color=c; document.getElementById('dot').style.color=c;
  document.getElementById('stateSub').textContent=d.stateSub;
  document.getElementById('float').textContent=d.low?'LOW (calling)':'satisfied';
  document.getElementById('mode').textContent=d.mode;
  document.getElementById('time').textContent=d.time;
  document.getElementById('uptime').textContent=d.uptime;
  document.getElementById('wifi').textContent=d.wifi;
  document.getElementById('heap').textContent=d.heap;
  document.getElementById('tariff').textContent=d.tariffPct+'% ('+d.tariffLabel+')';
  document.getElementById('fToday').textContent=d.fToday;
  document.getElementById('fWeek').textContent=d.fWeek;
  document.getElementById('fTotal').textContent=d.fTotal;
  document.getElementById('lastFill').textContent=d.lastFill;
  document.getElementById('avgFill').textContent=d.avgFill;
  document.getElementById('runtime').textContent=d.runtime;
  document.getElementById('sinceFill').textContent=d.sinceFill;
  document.getElementById('nextPred').textContent=d.nextPred;
  document.getElementById('pNow').textContent=Math.round(d.pNow*100)+'%';
  document.getElementById('pNext').textContent=Math.round(d.pNext*100)+'%';
  document.getElementById('pNowBar').style.width=(d.pNow*100)+'%';
  document.getElementById('pNextBar').style.width=(d.pNext*100)+'%';
  document.getElementById('learn').textContent=d.learn;
  document.getElementById('learnBar').style.width=d.learnPct+'%';
  const autoBtn=document.getElementById('autoBtn'), readyMsg=document.getElementById('readyMsg');
  if(d.autoReady){ autoBtn.textContent='Auto'; autoBtn.style.opacity=1;
    readyMsg.textContent='✓ Ready for auto mode'; readyMsg.style.color='#9ff5bd'; }
  else { autoBtn.textContent='Auto 🔒'; autoBtn.style.opacity=.55;
    readyMsg.textContent='Collecting real usage data — '+d.learnPct+'% of the way to unlocking Auto mode'; readyMsg.style.color=''; }
  document.getElementById('faults').textContent=d.faults;
  document.getElementById('lastFault').textContent=d.lastFault;
  document.getElementById('faultWhen').textContent=d.faultWhen;
  if(document.activeElement.id!='maxrun')document.getElementById('maxrun').value=d.maxrun;
  drawTimeline(d.events);
 }catch(e){}
}
function hourLabel(h){ const ap=h<12?'am':'pm'; let h12=h%12; if(h12==0)h12=12; return h12+ap; }
async function drawHeat(){
 try{
  const cv=document.getElementById('daychart'),ctx=cv.getContext('2d');
  const W=cv.width=cv.clientWidth, H=cv.height;
  const summary=document.getElementById('peakSummary');

  // Gate on ACTUAL data collected (not on how spread-out the untrained
  // network's guesses look — a fresh network hovers near 50% everywhere,
  // which isn't "flat enough" to self-detect as untrained).
  if(!lastChartReady){
    ctx.clearRect(0,0,W,H);
    ctx.fillStyle='#64748b'; ctx.font='12px system-ui'; ctx.textAlign='center';
    ctx.fillText('Gathering data…', W/2, H/2);
    summary.textContent='Not enough real usage data yet to show a reliable pattern.';
    return;
  }

  const grid=await (await fetch('/api/heatmap')).json();     // 7 days x 24 hours
  // average across the week into one "typical day" of 24 values
  const day=new Array(24).fill(0);
  for(let hr=0;hr<24;hr++){ let s=0; for(let d=0;d<7;d++)s+=grid[d*24+hr]; day[hr]=s/7; }

  ctx.clearRect(0,0,W,H);
  const bw=W/24, maxV=Math.max(...day,0.05);
  for(let hr=0;hr<24;hr++){
    const v=day[hr], bh=Math.max((v/maxV)*(H-18),2);
    ctx.fillStyle='rgba(56,189,248,'+(0.35+0.5*(v/maxV))+')';
    ctx.fillRect(hr*bw+1, H-bh-14, bw-2, bh);
    if(hr%3==0){ ctx.fillStyle='#64748b'; ctx.font='9px system-ui'; ctx.textAlign='center';
      ctx.fillText(hourLabel(hr), hr*bw+bw/2, H-3); }
  }

  // plain-English summary: top 2 busiest hours, quietest stretch
  const idxs=[...Array(24).keys()].sort((a,b)=>day[b]-day[a]);
  const busiest=idxs.slice(0,2).map(hourLabel).join(' and ');
  const quiet=idxs.slice(-4).sort((a,b)=>a-b);
  summary.textContent='Busiest around '+busiest+'. Quietest around '+hourLabel(quiet[0])+'–'+hourLabel(quiet[quiet.length-1])+'.';
 }catch(e){}
}
function drawTimeline(events){
  const cv=document.getElementById('timeline'),ctx=cv.getContext('2d');
  const W=cv.width=cv.clientWidth,H=cv.height;
  ctx.fillStyle='rgba(255,255,255,.04)';ctx.fillRect(0,0,W,H);
  // events: [{ago:sec, on:bool}] within last 3600s, drawn as green blocks
  ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--on');
  for(let i=0;i<events.length;i++){
    let e=events[i];
    if(!e.on)continue;
    let x1=W*(1-Math.min(e.ago,3600)/3600);
    let x2=W*(1-Math.min(e.end,3600)/3600);
    ctx.fillRect(x2,10,Math.max(x1-x2,2),H-20);
  }
}
tick(); drawHeat();
setInterval(tick,2000); setInterval(drawHeat,15000);
</script></body></html>
)HTML";

const char WIFIPAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartPump &mdash; Connect</title><style>
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
 font-family:system-ui,-apple-system,sans-serif;color:#e8eefc;padding:20px;
 background:radial-gradient(1200px 800px at 15% -10%,#1b2a4a 0%,transparent 60%),
            radial-gradient(1000px 700px at 110% 10%,#3a1b4a 0%,transparent 55%),#0a0f1e}
.card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:20px;
 padding:26px;max-width:360px;width:100%;backdrop-filter:blur(16px);box-shadow:0 8px 32px rgba(0,0,0,.4)}
h1{font-size:1.3rem;margin:0 0 4px}.sub{color:#8b97b5;font-size:.85rem;margin-bottom:14px}
label{display:block;font-size:.78rem;color:#8b97b5;margin:12px 0 4px}
input{width:100%;padding:11px;border-radius:11px;border:1px solid rgba(255,255,255,.15);
 background:rgba(255,255,255,.05);color:#e8eefc;font-size:1rem}
button{width:100%;margin-top:20px;padding:13px;border:none;border-radius:12px;
 background:linear-gradient(90deg,#38bdf8,#818cf8);color:#08111f;font-weight:700;font-size:1rem;cursor:pointer}
.note{color:#64748b;font-size:.78rem;margin-top:16px;line-height:1.4}
.err{background:rgba(239,68,68,.15);border:1px solid #ef4444;color:#ffb4b4;padding:10px 12px;border-radius:11px;font-size:.85rem;margin-bottom:6px}
</style></head><body>
<div class="card">
<h1>&#128167; SmartPump</h1>
<div class="sub">Connect to your WiFi to begin</div>
<!--ERR-->
<form method="POST" action="/wifi">
<label>WiFi name</label><input name="ssid" required autofocus>
<label>Password</label><input name="pass" type="password">
<button>Connect</button></form>
<div class="note">Needed so the device knows the correct time, which the AI uses to learn your pattern. The SmartPump hotspot stays on the whole time.</div>
</div></body></html>
)HTML";

/* ---------- WEB HANDLERS ------------------------------------------------- */
const char* stateName(){ return state==IDLE?"IDLE":state==FILLING?"FILLING":"FAULT"; }
String hms(uint32_t s){ char b[24]; snprintf(b,sizeof(b),"%luh %lum",(unsigned long)(s/3600),(unsigned long)((s/60)%60)); return b; }

// Dashboard is gated behind WiFi: no home WiFi connected -> force the setup
// page. This guarantees NTP time (and therefore learning) is live whenever
// the dashboard is reachable.
void handleRoot(){
  if(WiFi.status()==WL_CONNECTED) server.send_P(200,"text/html",PAGE);
  else server.send_P(200,"text/html",WIFIPAGE);
}
// Always reachable at /wifi so you can (re)connect or switch networks anytime.
void handleWifiForm(){ server.send_P(200,"text/html",WIFIPAGE); }

void handleStatus(){
  int wday,hour; int idx=currentBin(wday,hour);
  const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  float pNow=0,pNext=0; char timeStr[24]="no NTP yet";
  float curTariff=1.0f; const char* curTariffLbl="--";
  if(idx>=0){ pNow=predAt(hour,wday); pNext=predAt((hour+1)%24,(wday+((hour+1)/24))%7);
    struct tm t; getLocalTime(&t,10); strftime(timeStr,sizeof(timeStr),"%a %H:%M",&t);
    curTariff=tariffMult(hour); curTariffLbl=tariffLabel(hour); }

  uint32_t upS=millis()/1000;
  float learnPct=sampleCount/(float)AUTO_READY_SAMPLES*100; if(learnPct>100)learnPct=100;
  float learnDays=sampleCount/1440.0f;
  bool autoReady = sampleCount>=AUTO_READY_SAMPLES;

  // running-for / since-last
  String stateSub;
  if(state==FILLING) stateSub="running "+String((millis()-fillStart)/1000)+"s";
  else if(state==FAULT) stateSub="cooldown";
  else stateSub="waiting";

  String lastFillS = lastFillMs? String(lastFillDurMs/1000)+"s" : "never";
  String avgFillS = fillsTotal? String(fillDurSumSec/fillsTotal)+"s" : "--";
  String sinceS = lastFillMs? hms((millis()-lastFillMs)/1000) : "--";
  int na=nextPumpHoursAhead();
  String nextS = (na<0)?"--":(na==1?"in ~1 hr":"in ~"+String(na)+" hrs");
  String faultWhen = lastFaultMs? hms((millis()-lastFaultMs)/1000)+" ago":"--";

  // build events array (last hour, on-segments) from ring buffer
  String ev="[";
  uint32_t now=millis();
  bool first=true; uint32_t segEnd=0; bool inOn=false;
  // walk oldest->newest
  for(int n=0;n<evCount;n++){
    int i=(evHead-evCount+n+NEV)%NEV;
    uint32_t ago=(now-evbuf[i].ms)/1000;
    if(ago>3600 && !(evbuf[i].type==1)) { /*skip old*/ }
    if(evbuf[i].type==1){ inOn=true; segEnd=ago; }
    else if(inOn){ if(!first)ev+=","; ev+="{\"ago\":"+String(segEnd)+",\"on\":true,\"end\":"+String(ago)+"}"; first=false; inOn=false; }
  }
  if(inOn){ if(!first)ev+=","; ev+="{\"ago\":"+String(segEnd)+",\"on\":true,\"end\":0}"; }
  ev+="]";

  String rssi = (WiFi.status()==WL_CONNECTED)? String(WiFi.RSSI())+" dBm" : "hotspot only";

  String j="{";
  j+="\"state\":\""+String(stateName())+"\",";
  j+="\"stateSub\":\""+stateSub+"\",";
  j+="\"low\":"+String(callForWater()?"true":"false")+",";
  j+="\"mode\":\""+String(manual==M_AUTO?"AUTO":manual==M_ON?"ON":"OFF")+"\",";
  j+="\"time\":\""+String(timeStr)+"\",";
  j+="\"uptime\":\""+hms(upS)+"\",";
  j+="\"wifi\":\""+rssi+"\",";
  j+="\"heap\":\""+String(ESP.getFreeHeap()/1024)+" KB\",";
  j+="\"fToday\":"+String(fillsToday)+",";
  j+="\"fWeek\":"+String(fillsWeek)+",";
  j+="\"fTotal\":"+String(fillsTotal)+",";
  j+="\"lastFill\":\""+lastFillS+"\",";
  j+="\"avgFill\":\""+avgFillS+"\",";
  j+="\"runtime\":\""+hms(totalRunSec)+"\",";
  j+="\"sinceFill\":\""+sinceS+"\",";
  j+="\"nextPred\":\""+nextS+"\",";
  j+="\"pNow\":"+String(pNow,3)+",";
  j+="\"pNext\":"+String(pNext,3)+",";
  j+="\"learn\":\""+String(learnDays,1)+" days ("+String(sampleCount)+")\",";
  j+="\"learnPct\":"+String((int)learnPct)+",";
  j+="\"autoReady\":"+String(autoReady?"true":"false")+",";
  j+="\"chartReady\":"+String(sampleCount>=CHART_MIN_SAMPLES?"true":"false")+",";
  j+="\"tariffPct\":"+String((int)(curTariff*100))+",";
  j+="\"tariffLabel\":\""+String(curTariffLbl)+"\",";
  j+="\"faults\":"+String(faultCount)+",";
  j+="\"lastFault\":\""+lastFault+"\",";
  j+="\"faultWhen\":\""+faultWhen+"\",";
  j+="\"maxrun\":"+String(MAX_RUN_MS/60000)+",";
  j+="\"events\":"+ev;
  j+="}";
  server.send(200,"application/json",j);
}

void handleHeatmap(){
  String j="[";
  for(int d=0;d<7;d++)for(int h=0;h<24;h++){ if(d||h)j+=","; j+=String(predAt(h,d),3); }
  j+="]";
  server.send(200,"application/json",j);
}

void handleCsv(){
  String csv="ago_sec,event\n";
  time_t nowEpoch=time(nullptr); uint32_t nowMs=millis();
  for(int n=0;n<evCount;n++){
    int i=(evHead-evCount+n+NEV)%NEV;
    const char* ty=evbuf[i].type==1?"ON":evbuf[i].type==0?"OFF":"FAULT";
    csv+=String((nowMs-evbuf[i].ms)/1000)+","+ty+"\n";
  }
  server.sendHeader("Content-Disposition","attachment; filename=smartpump.csv");
  server.send(200,"text/csv",csv);
}

void handlePump(){ String m=server.arg("m");
  if(m=="auto" && sampleCount<AUTO_READY_SAMPLES){
    server.send(409,"text/plain","not enough data yet — Auto mode unlocks once the dataset bar is full"); return;
  }
  if(m=="on"){
    manual=M_ON;
  } else if(m=="off"){
    // OFF is a one-shot stop, not a standing lockout: stop the pump right now,
    // then immediately hand control back to the float so the sensor keeps
    // working (tilt the float low again afterward and it fills as normal).
    if(state==FILLING) endFill();
    relay(false);
    manual=M_AUTO;
  } else {
    manual=M_AUTO;
  }
  server.send(200,"text/plain","ok"); }
void handleFaultReset(){ if(state==FAULT)state=IDLE; server.send(200,"text/plain","ok"); }
void handleSettings(){ if(server.hasArg("maxrun")){ int v=server.arg("maxrun").toInt(); if(v>=1&&v<=120)MAX_RUN_MS=v*60000UL; } server.send(200,"text/plain","ok"); }

void handleWifiSave(){
  String s=server.arg("ssid"), p=server.arg("pass");
  if(s.length()==0){ server.send(400,"text/plain","SSID required"); return; }
  prefs.putString("wifi_ssid",s); prefs.putString("wifi_pass",p);
  savedSSID=s; savedPass=p;
  // connect live (no reboot) — AP/hotspot stays up the whole time
  WiFi.disconnect(); delay(100);
  WiFi.begin(savedSSID.c_str(),savedPass.c_str());
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<12000) delay(200);
  if(WiFi.status()==WL_CONNECTED){
    setStaDefault(); syncNtp();                              // route + grab the time
    server.sendHeader("Location","/"); server.send(302,"text/plain","");  // -> dashboard
  } else {
    String page=FPSTR(WIFIPAGE);
    page.replace("<!--ERR-->","<div class='err'>Couldn't connect to \""+s+"\" &mdash; check the name &amp; password.</div>");
    server.send(200,"text/html",page);
  }
}

// Disconnect from the current network and forget it -> back to login page.
void handleWifiForget(){
  prefs.remove("wifi_ssid"); prefs.remove("wifi_pass");
  savedSSID=""; savedPass="";
  WiFi.disconnect();
  server.send(200,"text/plain","disconnected");
}

// Dedicated WiFi config page (opened by the dashboard button). Lets you
// disconnect the current network or go add a new one. Login page stays pure.
void handleWifiCfg(){
  bool conn = WiFi.status()==WL_CONNECTED;
  String ss = savedSSID.length()? savedSSID : "(none saved)";
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi settings</title><style>*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;font-family:system-ui,sans-serif;color:#e8eefc;padding:20px;background:radial-gradient(1200px 800px at 15% -10%,#1b2a4a 0,transparent 60%),radial-gradient(1000px 700px at 110% 10%,#3a1b4a 0,transparent 55%),#0a0f1e}.card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:20px;padding:24px;max-width:360px;width:100%;backdrop-filter:blur(16px)}h1{font-size:1.2rem;margin:0 0 14px}.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.06);font-size:.9rem}.k{color:#8b97b5}button{width:100%;margin-top:12px;padding:12px;border-radius:12px;font-weight:600;font-size:.95rem;cursor:pointer}.add{background:linear-gradient(90deg,#38bdf8,#818cf8);color:#08111f;border:none}.dis{border:1px solid #ef4444;color:#ffb4b4;background:rgba(239,68,68,.15)}a{color:#38bdf8;font-size:.85rem}</style></head><body><div class='card'><h1>&#128246; WiFi settings</h1>";
  h += "<div class='row'><span class='k'>Network</span><span>"+ss+"</span></div>";
  h += "<div class='row'><span class='k'>Status</span><span>"+String(conn?"Connected":"Not connected")+"</span></div>";
  if(conn) h += "<div class='row'><span class='k'>IP</span><span>"+WiFi.localIP().toString()+"</span></div>";
  h += "<button class='add' onclick=\"location.href='/wifi'\">Add / change network</button>";
  h += "<button class='dis' onclick='disc()'>Disconnect current network</button>";
  h += "<p style='margin-top:14px'><a href='/'>&larr; back to dashboard</a></p></div>";
  h += "<script>async function disc(){await fetch('/api/wifi/forget',{method:'POST'});setTimeout(function(){location.href='/wifi';},500);}</script></body></html>";
  server.send(200,"text/html",h);
}

void setupWeb(){
  server.on("/",handleRoot);
  server.on("/api/status",handleStatus);
  server.on("/api/heatmap",handleHeatmap);
  server.on("/api/csv",handleCsv);
  server.on("/api/pump",HTTP_POST,handlePump);
  server.on("/api/fault",HTTP_POST,handleFaultReset);
  server.on("/api/settings",HTTP_POST,handleSettings);
  server.on("/api/wifi/forget",HTTP_POST,handleWifiForget);
  server.on("/wificfg",HTTP_GET,handleWifiCfg);
  server.on("/wifi",HTTP_GET,handleWifiForm);
  server.on("/wifi",HTTP_POST,handleWifiSave);
  server.begin();
}

/* ---------- WIFI + NTP --------------------------------------------------- */
// In AP+STA mode outbound internet (NTP) can leave via the AP interface, which
// has no gateway -> time never syncs. Force the home-WiFi (STA) netif as the
// default route so NTP packets actually reach the internet.
void setStaDefault(){
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if(sta) esp_netif_set_default_netif(sta);
}
void syncNtp(){
  // IP servers first (bypass DNS, which also mis-routes in AP+STA); hostname fallback
  configTzTime(TZ_INFO, "216.239.35.0", "216.239.35.4", "pool.ntp.org");
}
void connectHome(){
  if(savedSSID.length()==0){ Serial.println("No home WiFi yet — set at http://192.168.4.1/wifi"); return; }
  WiFi.begin(savedSSID.c_str(),savedPass.c_str());
  Serial.printf("Joining \"%s\"",savedSSID.c_str());
  for(int i=0;i<20&&WiFi.status()!=WL_CONNECTED;i++){ delay(300); Serial.print("."); }
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf(" ok %s\n",WiFi.localIP().toString().c_str());
    setStaDefault();
    syncNtp();
    struct tm t; int i=0;
    while(!getLocalTime(&t,500) && i<24){ Serial.print("."); i++; }   // wait up to ~12s for NTP
    if(getLocalTime(&t,100)) Serial.printf("NTP ok: %04d-%02d-%02d %02d:%02d\n",
       t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min);
    else Serial.println("NTP FAILED (network may block time sync)");
  }
  else Serial.println(" no home WiFi (hotspot still works)");
}

/* ---------- SETUP / LOOP ------------------------------------------------- */
void __attribute__((constructor)) earlySafe(){ pinMode(PIN_RELAY,OUTPUT); digitalWrite(PIN_RELAY,RELAY_OFF); }

void setup(){
  pinMode(PIN_RELAY,OUTPUT); relay(false);
  pinMode(PIN_LED,OUTPUT); digitalWrite(PIN_LED,LOW);
  Serial.begin(115200); delay(300);
  Serial.println("\n=== SmartPump + on-device neural net ===");
  pinMode(PIN_FLOAT,INPUT_PULLUP); debInit(debFloat,PIN_FLOAT);

  prefs.begin("smartpump",false);
  if(prefs.getBytesLength("nn")==NWEIGHTS*sizeof(float)){
    float buf[NWEIGHTS]; prefs.getBytes("nn",buf,sizeof(buf)); netUnpack(buf);
    sampleCount=prefs.getUInt("samples",0);
    fillsTotal=prefs.getUInt("ftot",0); fillsToday=prefs.getUInt("fday",0); fillsWeek=prefs.getUInt("fweek",0);
    curDay=prefs.getInt("cday",-1); curWeek=prefs.getInt("cweek",-1);
    totalRunSec=prefs.getUInt("runsec",0); fillDurSumSec=prefs.getUInt("dursum",0);
    Serial.printf("loaded NN (%lu samples), %lu total fills\n",(unsigned long)sampleCount,(unsigned long)fillsTotal);
  } else { netInit(); Serial.println("fresh NN (random weights)"); }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.softAP(AP_SSID,AP_PASS);
  Serial.printf("Hotspot \"%s\" at %s\n",AP_SSID,WiFi.softAPIP().toString().c_str());
  savedSSID=prefs.getString("wifi_ssid",WIFI_SSID);
  savedPass=prefs.getString("wifi_pass",WIFI_PASS);
  connectHome();
  setupWeb();
  Serial.println("Dashboard: http://192.168.4.1");
}

void loop(){
  server.handleClient();
  debUpd(debFloat);
  updatePump();
  uint32_t now=millis();
  if(now-tTick>=30000){ tTick=now; rolloverCheck(); }
  if(WiFi.status()==WL_CONNECTED && !haveTime() && now-tNtp>=60000){ tNtp=now; setStaDefault(); syncNtp(); }
  if(now-tLearn>=LEARN_SAMPLE_MS){ tLearn=now; updateLearner(); }
  if(now-tSave>=SAVE_MS){ tSave=now; saveAll(); }
  if(now-tLed>=250){ tLed=now;
    if(state==FILLING)digitalWrite(PIN_LED,HIGH);
    else if(state==FAULT)digitalWrite(PIN_LED,!digitalRead(PIN_LED));
    else digitalWrite(PIN_LED,LOW); }
  delay(5);
}
