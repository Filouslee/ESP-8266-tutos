#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Servo.h>

// -------- Wi-Fi ----------
const char* ssid = "YOUR_WIFI_SSID";         
const char* password = "YOUR_WIFI_PASSWORD";
const char* AP_SSID   = "RADAR-ESP8266";
const char* AP_PASS   = "12345678";

ESP8266WebServer server(80);

// -------- IO ------------
const int PIN_SERVO = D4;  // GPIO2
const int PIN_TRIG  = D5;  // GPIO14
const int PIN_ECHO  = D6;  // GPIO12 (via diviseur 5V->3V3)

Servo servo;

// -------- Etat mesure ---
volatile int   lastAngle = 0;
volatile float lastDist  = NAN;
volatile unsigned long lastMs = 0;
volatile bool  lastOk   = false;

// -------- HTML ----------
// Page web: radar canvas + polling /data
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 Radar</title>
<style>
  body{margin:0;background:#0b1321;color:#d8f3ff;font-family:system-ui,Segoe UI,Roboto,Arial}
  header{padding:10px 16px;background:#0f1b34;border-bottom:1px solid #1e2a4a}
  .wrap{display:flex;gap:16px;flex-wrap:wrap;padding:12px 16px}
  #cv{background:#081020;border:1px solid #1e2a4a;border-radius:8px}
  .panel{min-width:260px}
  .row{margin:8px 0}
  input[type=range]{width:100%}
  code{background:#0f1b34;padding:2px 6px;border-radius:4px}
  .pill{display:inline-block;padding:2px 8px;border:1px solid #1e2a4a;border-radius:999px;margin-left:6px}
</style>
</head>
<body>
<header>
  <strong>ESP8266 Radar</strong>
  <span id="status" class="pill">connecting…</span>
</header>
<div class="wrap">
  <canvas id="cv" width="600" height="600"></canvas>
  <div class="panel">
    <div class="row">Max range (cm): <span id="rval">200</span>
      <input id="rmax" type="range" min="50" max="500" value="200">
    </div>
    <div class="row">Polling (ms): <span id="ival">50</span>
      <input id="ivalin" type="range" min="20" max="200" value="50">
    </div>
    <div class="row">Points kept: <span id="pval">1200</span>
      <input id="pmax" type="range" min="200" max="2400" value="1200">
    </div>
    <div class="row">Last measurement: <code id="last">—</code></div>
    <div class="row">Tips:
      <ul>
        <li>0° at the top, clockwise sweep.</li>
        <li>Color fades with age.</li>
      </ul>
    </div>
  </div>
</div>
<script>
// JavaScript stays the same; only status text messages in fetchData() are translated
const cv = document.getElementById('cv');
const ctx = cv.getContext('2d');
let RMAX_CM = 200;
let POLL_MS = 50;
let MAX_POINTS = 1200;

const rmaxEl = document.getElementById('rmax');
const rvalEl = document.getElementById('rval');
const ivalEl = document.getElementById('ival');
const ivalIn = document.getElementById('ivalin');
const pmaxEl = document.getElementById('pmax');
const pvalEl = document.getElementById('pval');
const lastEl = document.getElementById('last');
const statusEl = document.getElementById('status');

rmaxEl.oninput = e => { RMAX_CM = +e.target.value; rvalEl.textContent = RMAX_CM; };
ivalIn.oninput = e => { POLL_MS = +e.target.value; ivalEl.textContent = POLL_MS; restartPoll(); };
pmaxEl.oninput = e => { MAX_POINTS = +e.target.value; pvalEl.textContent = MAX_POINTS; };

const W = cv.width, H = cv.height;
const cx = W/2, cy = H/2;
const radius = Math.min(W,H)/2 - 20;

let points = []; // {a,d,t}

function grid(){
  ctx.clearRect(0,0,W,H);
  ctx.save();
  ctx.translate(cx,cy);
  // background
  ctx.fillStyle = '#09142a';
  ctx.beginPath(); ctx.arc(0,0,radius+12,0,Math.PI*2); ctx.fill();
  // rings
  ctx.strokeStyle = '#173054'; ctx.lineWidth = 1;
  for(let i=1;i<=4;i++){ ctx.beginPath(); ctx.arc(0,0,radius*i/4,0,Math.PI*2); ctx.stroke(); }
  // spokes every 30°
  for(let a=0;a<360;a+=30){
    const th = (a-90)*Math.PI/180;
    ctx.beginPath(); ctx.moveTo(0,0);
    ctx.lineTo(radius*Math.cos(th), radius*Math.sin(th));
    ctx.stroke();
  }
  ctx.restore();
}

function draw(thetaDeg){
  grid();
  ctx.save();
  ctx.translate(cx,cy);
  // sweep line
  const th = (thetaDeg-90)*Math.PI/180;
  ctx.strokeStyle = '#8efcff'; ctx.globalAlpha = 0.8; ctx.lineWidth = 1.2;
  ctx.beginPath(); ctx.moveTo(0,0);
  ctx.lineTo(radius*Math.cos(th), radius*Math.sin(th)); ctx.stroke();

  // points (fade with age)
  const now = performance.now();
  const n = points.length;
  for(let i=0;i<n;i++){
    const p = points[i];
    const age = now - p.t;
    const alpha = Math.max(0, 1.0 - age / 3000); // fade over ~3s
    if(alpha <= 0) continue;
    const rr = Math.min(radius, (p.d / RMAX_CM) * radius);
    const t  = (p.a-90)*Math.PI/180;
    ctx.fillStyle = `rgba(80,255,120,${alpha})`;
    ctx.beginPath();
    ctx.arc(rr*Math.cos(t), rr*Math.sin(t), 2.2, 0, Math.PI*2);
    ctx.fill();
  }
  ctx.restore();
}

let pollTimer = null;
function restartPoll(){
  if(pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(fetchData, POLL_MS);
}
async function fetchData(){
  try{
    const r = await fetch('/data?ts='+Date.now(), {cache:'no-store'});
    if(!r.ok) throw new Error(r.statusText);
    const j = await r.json();
    statusEl.textContent = 'ok';
    if(j && j.ok){
      lastEl.textContent = `a=${j.a.toFixed(0)}°, d=${isFinite(j.d)?j.d.toFixed(1):'NaN'} cm`;
      if(isFinite(j.d)){
        points.push({a:j.a, d:j.d, t:performance.now()});
        if(points.length > MAX_POINTS) points.splice(0, points.length - MAX_POINTS);
      }
      draw(j.a);
    }
  }catch(e){
    statusEl.textContent = 'disconnected';
  }
}

grid(); draw(0); restartPoll();
</script>
</body>
</html>
)HTML";

// ---------- Mesure distance ----------
float measureDistanceCm() {
  // Impulsion de déclenchement
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Echo avec timeout (~25 ms => ~4 m)
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (duration == 0) return NAN;

  // d = (t * 0,0343 cm/µs) / 2
  float dist = (duration * 0.0343f) / 2.0f;
  return dist;
}

float median3(float a, float b, float c) {
  if (isnan(a)) a = 1e9;
  if (isnan(b)) b = 1e9;
  if (isnan(c)) c = 1e9;
  if ((a >= b && a <= c) || (a <= b && a >= c)) return a;
  if ((b >= a && b <= c) || (b <= a && b >= c)) return b;
  return c;
}

// ---------- Wi-Fi ----------
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("WiFi STA connexion à "); Serial.println(ssid);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(300); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connecté. IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin("esp8266")) {
      Serial.println("mDNS actif: http://esp8266.local");
    }
  } else {
    Serial.println("Echec STA. Démarrage AP…");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress ip = WiFi.softAPIP();
    Serial.print("AP actif: "); Serial.println(AP_SSID);
    Serial.print("IP AP: "); Serial.println(ip);
  }
}

// ---------- HTTP ----------
void setupHTTP() {
  server.on("/", HTTP_GET, [](){
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/data", HTTP_GET, [](){
    // instantané JSON
    char buf[128];
    noInterrupts();
    int a = lastAngle; float d = lastDist; unsigned long t = lastMs; bool ok = lastOk;
    interrupts();
    snprintf(buf, sizeof(buf),
      "{\"ok\":%d,\"a\":%d,\"d\":%s,\"t\":%lu}",
      ok?1:0, a, (isnan(d)?"NaN":String(d,1).c_str()), t);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "application/json", buf);
  });

  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("Serveur HTTP prêt.");
}

// ---------- Setup / Loop ----------
int angle = 0;
int dir   = 1; // 1: monte, -1: descend

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  servo.attach(PIN_SERVO, 500, 2500);
  servo.write(angle);
  delay(400);

  startWiFi();
  setupHTTP();

  Serial.println("# Radar Wi-Fi prêt. Ouvre l’IP affichée ou http://esp8266.local");
}

void loop() {
  // Balayage
  servo.write(angle);
  // petite pause servo + service HTTP
  for (int i=0;i<3;i++){ delay(6); server.handleClient(); yield(); }

  // 3 mesures -> médiane
  float d1 = measureDistanceCm(); server.handleClient(); yield();
  float d2 = measureDistanceCm(); server.handleClient(); yield();
  float d3 = measureDistanceCm(); server.handleClient(); yield();
  float d  = median3(d1, d2, d3);

  // Publier l'instantané
  lastAngle = angle;
  lastDist  = d;
  lastMs    = millis();
  lastOk    = !isnan(d);

  // Avance de l'angle
  angle += dir * 2; // pas 2°
  if (angle >= 180) { angle = 180; dir = -1; }
  if (angle <= 0)   { angle = 0;   dir =  1; }

  // cadence globale + service HTTP
  for (int i=0;i<2;i++){ delay(10); server.handleClient(); yield(); }
}
