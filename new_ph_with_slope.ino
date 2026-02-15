/*
  ESP32 pH Monitor + Calibration (2-point or 3-point)
  - Main page "/" shows ONLY current pH + Calibrate button (opens "/calibrate" in a new window)
  - Calibration page lets you save:
      * 2-point line using pH 7.00 and pH 4.01
      * 3-point quadratic using pH 7.00, pH 4.01, pH 9.18
  - Coefficients stored in NVS (Preferences) so they survive reboot
  - Compile-time switch to choose measurement source:
      * INA226 bus voltage (stable)
      * ESP32 ADC on GPIO36 (ADC1)

  NOTE: If using ADC, GPIO36 is good because ADC1 works while WiFi is on.
*/

// ===================== USER OPTIONS =====================

// ---- Pick ONE measurement source ----
#define USE_INA226   1   // set to 1 to use INA226 (I2C), set to 0 to use ESP32 ADC

// ---- WiFi ----
const char* ssid     = "innerstellar";
const char* password = "starbaby";
const int   port     = 1129;

// ---- Optional smoothing ----
#define ENABLE_IIR_SMOOTHING  1
const float IIR_ALPHA = 0.15f;  // 0.05 smoother/slow, 0.15 good default, 0.30 faster/less smooth

// ---- Scaling if you have a divider between sensor output and measurement point ----
// Example: divider halves voltage -> VOLTAGE_SCALE = 2.0
static const float VOLTAGE_SCALE = 1.0f;

// ===================== INCLUDES =====================
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#if USE_INA226
  #include <Wire.h>
  #include <INA226_WE.h>
  #define INA_ADDR 0x40
  INA226_WE ina226(INA_ADDR);
#else
  // ADC
  static const int   PH_ADC_PIN   = 36;    // GPIO36 = ADC1_CH0
  static const float ADC_VREF     = 3.30f; // approximate; calibration with buffers makes this OK
  static const float ADC_MAX      = 4095.0f;
#endif

// ===================== GLOBALS =====================
WebServer server(port);
Preferences prefs;

static const float PH7 = 7.00f;
static const float PH4 = 4.01f;
static const float PH9 = 9.18f;

// Calibration storage:
// Linear:   pH = m*V + b
// Quadratic pH = a*V^2 + b*V + c  (we store qa,qb,qc)
bool  useQuadratic = false;

float linM = -2.49f;
float linB = 10.4725f;

float qa = 0.0f;
float qb = -2.49f;
float qc = 10.4725f;

#if ENABLE_IIR_SMOOTHING
float filtV = NAN;
#endif

// ===================== MATH: 3-point quadratic from 3 points =====================
bool solveQuadraticFrom3Points(float x1,float y1, float x2,float y2, float x3,float y3,
                               float &a,float &b,float &c) {
  // Lagrange interpolation expanded to ax^2 + bx + c
  float d1 = (x1 - x2) * (x1 - x3);
  float d2 = (x2 - x1) * (x2 - x3);
  float d3 = (x3 - x1) * (x3 - x2);
  if (fabs(d1) < 1e-9f || fabs(d2) < 1e-9f || fabs(d3) < 1e-9f) return false;

  float A1 = y1 / d1;
  float A2 = y2 / d2;
  float A3 = y3 / d3;

  a = A1 + A2 + A3;
  b = -(A1*(x2+x3) + A2*(x1+x3) + A3*(x1+x2));
  c =  (A1*(x2*x3) + A2*(x1*x3) + A3*(x1*x2));
  return true;
}

// ===================== CALC pH =====================
float calculatePH(float v_sensor) {
  if (useQuadratic) {
    return qa*v_sensor*v_sensor + qb*v_sensor + qc;
  } else {
    return linM*v_sensor + linB;
  }
}

// ===================== LOAD/SAVE CAL =====================
void loadCalibration() {
  prefs.begin("phcal", true);

  useQuadratic = prefs.getBool("quad", false);

  linM = prefs.getFloat("m", linM);
  linB = prefs.getFloat("b", linB);

  qa = prefs.getFloat("qa", qa);
  qb = prefs.getFloat("qb", qb);
  qc = prefs.getFloat("qc", qc);

  prefs.end();

  Serial.printf("Loaded cal: quad=%d\n", useQuadratic ? 1 : 0);
  Serial.printf("  linear: m=%.8f b=%.8f\n", linM, linB);
  Serial.printf("  quad:   a=%.10f b=%.8f c=%.8f\n", qa, qb, qc);
}

void saveLinear(float m, float b) {
  prefs.begin("phcal", false);
  prefs.putBool("quad", false);
  prefs.putFloat("m", m);
  prefs.putFloat("b", b);
  prefs.end();

  useQuadratic = false;
  linM = m;
  linB = b;

#if ENABLE_IIR_SMOOTHING
  filtV = NAN; // re-init smoothing after cal change
#endif
}

void saveQuadratic(float a, float b, float c) {
  prefs.begin("phcal", false);
  prefs.putBool("quad", true);
  prefs.putFloat("qa", a);
  prefs.putFloat("qb", b);
  prefs.putFloat("qc", c);
  prefs.end();

  useQuadratic = true;
  qa = a; qb = b; qc = c;

#if ENABLE_IIR_SMOOTHING
  filtV = NAN;
#endif
}

// ===================== VOLTAGE READ =====================
#if USE_INA226

float readPHVoltage() {
  // INA226 bus voltage with trimmed mean (drop min & max) + optional IIR smoothing
  const int N = 20;
  float sum = 0.0f;
  float mn =  1e9f;
  float mx = -1e9f;

  for (int i = 0; i < N; i++) {
    float a = ina226.getBusVoltage_V();
    sum += a;
    if (a < mn) mn = a;
    if (a > mx) mx = a;
    delay(2);
  }

  sum -= mn;
  sum -= mx;
  float v = sum / (float)(N - 2);

  float v_sensor = v * VOLTAGE_SCALE;

#if ENABLE_IIR_SMOOTHING
  if (isnan(filtV)) filtV = v_sensor;
  filtV = filtV + IIR_ALPHA * (v_sensor - filtV);
  return filtV;
#else
  return v_sensor;
#endif
}

#else

float readPHVoltage() {
  // ESP32 ADC with trimmed mean (drop min & max) + optional IIR smoothing
  const int N = 25;
  uint32_t sum = 0;
  uint16_t mn = 4095;
  uint16_t mx = 0;

  for (int i = 0; i < N; i++) {
    uint16_t a = analogRead(PH_ADC_PIN);
    sum += a;
    if (a < mn) mn = a;
    if (a > mx) mx = a;
    delay(2);
  }

  sum -= mn;
  sum -= mx;
  float avgCounts = (float)sum / (float)(N - 2);

  float v_pin = (avgCounts / ADC_MAX) * ADC_VREF;
  float v_sensor = v_pin * VOLTAGE_SCALE;

#if ENABLE_IIR_SMOOTHING
  if (isnan(filtV)) filtV = v_sensor;
  filtV = filtV + IIR_ALPHA * (v_sensor - filtV);
  return filtV;
#else
  return v_sensor;
#endif
}

#endif

// ===================== API HANDLERS =====================
void handlePH() {
  float v = readPHVoltage();
  float p = calculatePH(v);
  server.send(200, "text/plain", String(p, 2));
}

void handlePHV() {
  float v = readPHVoltage();
  server.send(200, "text/plain", String(v, 4));
}

void handleCalJson() {
  // returns current mode and coefficients
  String json = "{";
  json += "\"quad\":" + String(useQuadratic ? "true" : "false") + ",";
  json += "\"m\":" + String(linM, 8) + ",";
  json += "\"b\":" + String(linB, 8) + ",";
  json += "\"qa\":" + String(qa, 10) + ",";
  json += "\"qb\":" + String(qb, 8) + ",";
  json += "\"qc\":" + String(qc, 8);
  json += "}";
  server.send(200, "application/json", json);
}

// 2-point: /setcal2?v7=2.100&v4=2.600
void handleSetCal2() {
  if (!server.hasArg("v7") || !server.hasArg("v4")) {
    server.send(400, "text/plain", "Missing v7 or v4");
    return;
  }
  float v7 = server.arg("v7").toFloat();
  float v4 = server.arg("v4").toFloat();

  if (v7 <= 0.0f || v4 <= 0.0f) {
    server.send(400, "text/plain", "Voltages must be > 0");
    return;
  }
  if (fabs(v4 - v7) < 0.01f) {
    server.send(400, "text/plain", "v4 and v7 too close (bad calibration)");
    return;
  }

  float m = (PH4 - PH7) / (v4 - v7);
  float b = PH7 - m * v7;

  saveLinear(m, b);

  server.send(200, "text/plain",
              "Saved 2-point: pH = " + String(m, 8) + "*V + " + String(b, 8));
}

// 3-point: /setcal3?v7=...&v4=...&v9=...
void handleSetCal3() {
  if (!server.hasArg("v7") || !server.hasArg("v4") || !server.hasArg("v9")) {
    server.send(400, "text/plain", "Missing v7 or v4 or v9");
    return;
  }
  float v7 = server.arg("v7").toFloat();
  float v4 = server.arg("v4").toFloat();
  float v9 = server.arg("v9").toFloat();

  if (v7 <= 0.0f || v4 <= 0.0f || v9 <= 0.0f) {
    server.send(400, "text/plain", "Voltages must be > 0");
    return;
  }
  // quick sanity: avoid duplicates
  if (fabs(v4 - v7) < 0.01f || fabs(v9 - v7) < 0.01f || fabs(v9 - v4) < 0.01f) {
    server.send(400, "text/plain", "Two voltages are too close (bad 3-point)");
    return;
  }

  float a,b,c;
  if (!solveQuadraticFrom3Points(v4, PH4, v7, PH7, v9, PH9, a, b, c)) {
    server.send(400, "text/plain", "Could not solve quadratic (check voltages)");
    return;
  }

  saveQuadratic(a, b, c);

  server.send(200, "text/plain",
              "Saved 3-point: pH = " + String(a, 10) + "*V^2 + " + String(b, 8) + "*V + " + String(c, 8));
}

// ===================== PAGES =====================
void handleRoot() {
  // Main page: ONLY pH + Calibrate button
  String html =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:Arial;margin:18px}"
    "#box{max-width:420px;margin:auto;border:2px solid #1e90ff;border-radius:14px;padding:16px;text-align:center}"
    "#ph{font-size:64px;font-weight:bold}"
    "button{font-size:18px;padding:10px 14px;border:0;border-radius:10px;background:#4CAF50;color:white}"
    "small{color:#666}"
    "</style>"
    "<script>"
    "async function refresh(){"
    "  try{"
    "    const ph = await fetch('/ph',{cache:'no-store'}).then(r=>r.text());"
    "    document.getElementById('ph').innerText = ph;"
    "  }catch(e){console.log(e);}"
    "}"
    "setInterval(refresh,2000);window.onload=refresh;"
    "</script></head><body>"
    "<div id='box'>"
    "<h2>pH Monitor</h2>"
    "<div id='ph'>--</div>"
    "<div style='margin:16px 0'>"
    "<button onclick=\"window.open('/calibrate','phcal','width=560,height=720');\">Calibrate</button>"
    "</div>"
    "<small>Updates every 2 seconds</small>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleCalibrate() {
  // Calibration page: enter voltages for pH7, pH4, pH9; save 2-point or 3-point
  String html =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:Arial;margin:18px}"
    "#box{max-width:560px;margin:auto;border:2px solid #ff7a00;border-radius:14px;padding:16px}"
    "input{font-size:18px;width:150px;padding:6px;margin:6px}"
    "button{font-size:18px;padding:10px 14px;border:0;border-radius:10px;background:#4CAF50;color:white;margin-right:10px;margin-top:8px}"
    ".gray{background:#666}"
    ".row{font-size:18px;margin:10px 0}"
    "small{color:#666}"
    "code{background:#f2f2f2;padding:2px 6px;border-radius:6px}"
    "</style>"
    "<script>"
    "async function refresh(){"
    "  try{"
    "    const phv = await fetch('/phv',{cache:'no-store'}).then(r=>r.text());"
    "    const cal = await fetch('/cal',{cache:'no-store'}).then(r=>r.json());"
    "    document.getElementById('phv').innerText = phv;"
    "    document.getElementById('mode').innerText = cal.quad ? '3-point (quadratic)' : '2-point (linear)';"
    "    document.getElementById('lin').innerText  = `pH = ${cal.m.toFixed(6)}*V + ${cal.b.toFixed(6)}`;"
    "    document.getElementById('quad').innerText = `pH = ${cal.qa.toFixed(10)}*V^2 + ${cal.qb.toFixed(6)}*V + ${cal.qc.toFixed(6)}`;"
    "  }catch(e){console.log(e);}"
    "}"
    "async function save2(){"
    "  const v7 = document.getElementById('v7').value;"
    "  const v4 = document.getElementById('v4').value;"
    "  const msg = document.getElementById('msg'); msg.innerText='';"
    "  try{"
    "    const t = await fetch(`/setcal2?v7=${encodeURIComponent(v7)}&v4=${encodeURIComponent(v4)}`,{cache:'no-store'}).then(r=>r.text());"
    "    msg.innerText=t; refresh();"
    "  }catch(e){msg.innerText='Save failed';}"
    "}"
    "async function save3(){"
    "  const v7 = document.getElementById('v7').value;"
    "  const v4 = document.getElementById('v4').value;"
    "  const v9 = document.getElementById('v9').value;"
    "  const msg = document.getElementById('msg'); msg.innerText='';"
    "  try{"
    "    const t = await fetch(`/setcal3?v7=${encodeURIComponent(v7)}&v4=${encodeURIComponent(v4)}&v9=${encodeURIComponent(v9)}`,{cache:'no-store'}).then(r=>r.text());"
    "    msg.innerText=t; refresh();"
    "  }catch(e){msg.innerText='Save failed';}"
    "}"
    "function closeMe(){ window.close(); }"
    "setInterval(refresh,2000);window.onload=refresh;"
    "</script></head><body>"
    "<div id='box'>"
    "<h2>pH Calibration</h2>"
    "<div class='row'>Live pHv: <b><span id='phv'>--</span></b> V</div>"
    "<div class='row'>pH 7.00 voltage: <input id='v7' type='number' step='0.001' value='2.100'></div>"
    "<div class='row'>pH 4.01 voltage: <input id='v4' type='number' step='0.001' value='2.600'></div>"
    "<div class='row'>pH 9.18 voltage: <input id='v9' type='number' step='0.001' value='1.800'></div>"
    "<div>"
    "<button onclick='save2()'>Save 2-point</button>"
    "<button onclick='save3()'>Save 3-point</button>"
    "<button class='gray' onclick='closeMe()'>Close</button>"
    "</div>"
    "<div id='msg' style='margin-top:10px;font-size:14px'></div>"
    "<p><small>Active mode: <b><span id='mode'>--</span></b></small></p>"
    "<p><small>Linear: <code><span id='lin'>--</span></code></small></p>"
    "<p><small>Quadratic: <code><span id='quad'>--</span></code></small></p>"
    "<p><small>Tip: Let readings stabilize in each buffer before saving.</small></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

// ===================== SETUP/LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(200);

#if USE_INA226
  Wire.begin(); // SDA=21, SCL=22 on most ESP32 dev boards
  if (!ina226.init()) {
    Serial.println("INA226 init failed! Check wiring/address.");
  } else {
    // Smoother readings
    ina226.setAverage(AVERAGE_16);            // try AVERAGE_64 for even smoother
    ina226.setConversionTime(CONV_TIME_1100); // slower conversion = more stable
    Serial.println("INA226 OK");
  }
#else
  // ADC setup
  analogReadResolution(12);
  analogSetPinAttenuation(PH_ADC_PIN, ADC_11db);
  Serial.println("Using ESP32 ADC on GPIO36");
#endif

  loadCalibration();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected.");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // Routes
  server.on("/",          HTTP_GET, handleRoot);
  server.on("/calibrate", HTTP_GET, handleCalibrate);

  server.on("/ph",    HTTP_GET, handlePH);
  server.on("/phv",   HTTP_GET, handlePHV);
  server.on("/cal",   HTTP_GET, handleCalJson);
  server.on("/setcal2", HTTP_GET, handleSetCal2);
  server.on("/setcal3", HTTP_GET, handleSetCal3);

  server.begin();
  Serial.printf("HTTP server started on port %d\n", port);
}

void loop() {
  server.handleClient();
}
