/*
  ESP32 pH Monitor + Simple Calibration (Easy to explain)
  -------------------------------------------------------
  Two webpages:
    - "/" shows ONLY the current pH (SMOOTHED) and a Calibrate button
    - "/calibrate" opens in a new window and saves calibration (UNSMOOTHED)

  Calibration is LINEAR and user-friendly:
    1) Basic (2-point): use pH 4.01 and pH 7.00 -> ONE straight line for all pH
    2) Optional (3rd point): add pH 9.18 -> TWO straight lines:
         - one line for pH <= 7  (between 4.01 and 7.00)
         - one line for pH >= 7  (between 7.00 and 9.18)

  Measurement source is a compile-time switch:
    - INA226 bus voltage (stable)
    - ESP32 ADC on GPIO36 (ADC1)

  Notes:
    - Only the MAIN page applies IIR smoothing
*/

// ===================== USER OPTIONS =====================

// ---- Pick ONE measurement source ----
#define USE_INA226   1   // 1 = INA226, 0 = ESP32 ADC

// ---- WiFi ----
const char* ssid     = "innerstellar";
const char* password = "starbaby";
const int   port     = 1129;

// ---- Main page smoothing ----
#define MAIN_PAGE_SMOOTHING  1
const float MAIN_ALPHA = 0.15f;   // 0.05 smoother/slow, 0.15 good, 0.30 faster/less smooth

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
  static const int   PH_ADC_PIN  = 36;    // GPIO36 = ADC1_CH0
  static const float ADC_VREF    = 3.30f; // approximate; calibration handles most error
  static const float ADC_MAX     = 4095.0f;
#endif

// ===================== GLOBALS =====================
WebServer server(port);
Preferences prefs;

static const float PH7 = 7.00f;
static const float PH4 = 4.01f;
static const float PH9 = 9.18f;

// Calibration model:
// - Always have "low" line from (V4,4.01) to (V7,7.00)
// - Optional "high" line from (V7,7.00) to (V9,9.18)
bool  useTwoLines = false;   // false = one line for all; true = piecewise two-line
bool  typicalDirection = true; // true if V4 > V7 (common on many pH boards)

float v7_break = 2.100f;

float mLow = -2.49f;
float bLow = 10.4725f;

float mHigh = -2.49f;
float bHigh = 10.4725f;

// smoothing state for MAIN page only
#if MAIN_PAGE_SMOOTHING
float filtV_main = NAN;
#endif

// ===================== LOAD/SAVE CAL =====================
void loadCalibration() {
  prefs.begin("phcal", true);

  useTwoLines      = prefs.getBool("two", false);
  typicalDirection = prefs.getBool("typ", true);

  v7_break = prefs.getFloat("v7", v7_break);

  mLow  = prefs.getFloat("mL", mLow);
  bLow  = prefs.getFloat("bL", bLow);

  mHigh = prefs.getFloat("mH", mHigh);
  bHigh = prefs.getFloat("bH", bHigh);

  prefs.end();

  Serial.printf("Loaded cal: twoLines=%d typical=%d v7=%.4f\n",
                useTwoLines ? 1 : 0, typicalDirection ? 1 : 0, v7_break);
  Serial.printf("  low : pH = %.8f*V + %.8f\n", mLow, bLow);
  Serial.printf("  high: pH = %.8f*V + %.8f\n", mHigh, bHigh);
}

void saveOneLine(float V4, float V7) {
  float m = (PH4 - PH7) / (V4 - V7);
  float b = PH7 - m * V7;

  prefs.begin("phcal", false);
  prefs.putBool("two", false);
  prefs.putBool("typ", (V4 > V7));
  prefs.putFloat("v7", V7);
  prefs.putFloat("mL", m);
  prefs.putFloat("bL", b);
  prefs.putFloat("mH", m);
  prefs.putFloat("bH", b);
  prefs.end();

  useTwoLines      = false;
  typicalDirection = (V4 > V7);
  v7_break = V7;

  mLow = m;  bLow = b;
  mHigh = m; bHigh = b;

#if MAIN_PAGE_SMOOTHING
  filtV_main = NAN; // reset smoothing after calibration change
#endif
}

void saveTwoLines(float V4, float V7, float V9) {
  float mL = (PH4 - PH7) / (V4 - V7);
  float bL = PH7 - mL * V7;

  float mH = (PH7 - PH9) / (V7 - V9);
  float bH = PH7 - mH * V7;

  prefs.begin("phcal", false);
  prefs.putBool("two", true);
  prefs.putBool("typ", (V4 > V7));
  prefs.putFloat("v7", V7);
  prefs.putFloat("mL", mL);
  prefs.putFloat("bL", bL);
  prefs.putFloat("mH", mH);
  prefs.putFloat("bH", bH);
  prefs.end();

  useTwoLines      = true;
  typicalDirection = (V4 > V7);
  v7_break = V7;

  mLow = mL;  bLow = bL;
  mHigh = mH; bHigh = bH;

#if MAIN_PAGE_SMOOTHING
  filtV_main = NAN;
#endif
}

// ===================== pH CALC =====================
float calculatePH_piecewise(float v) {
  if (!useTwoLines) return mLow * v + bLow;

  // Decide which line based on which side of V7 we are on.
  if (typicalDirection) {
    // Usually V4 > V7 > V9
    if (v >= v7_break) return mLow * v + bLow;   // pH <= 7 side
    else               return mHigh * v + bHigh; // pH >= 7 side
  } else {
    // Inverted mapping
    if (v <= v7_break) return mLow * v + bLow;
    else               return mHigh * v + bHigh;
  }
}

// ===================== VOLTAGE READ (raw vs smoothed) =====================
#if USE_INA226

float readVoltageRawTrimmed() {
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
  return sum / (float)(N - 2);
}

#else

float readVoltageRawTrimmed() {
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
  return (avgCounts / ADC_MAX) * ADC_VREF;
}

#endif

float readVoltageMainSmoothed() {
  float v = readVoltageRawTrimmed();

#if MAIN_PAGE_SMOOTHING
  if (isnan(filtV_main)) filtV_main = v;
  filtV_main = filtV_main + MAIN_ALPHA * (v - filtV_main);
  return filtV_main;
#else
  return v;
#endif
}

// ===================== API HANDLERS =====================
void handlePH() {
  // Main page uses SMOOTHED voltage -> smoother pH
  float v = readVoltageMainSmoothed();
  float p = calculatePH_piecewise(v);
  server.send(200, "text/plain", String(p, 2));
}

void handlePHV() {
  // Calibration page shows RAW (no smoothing)
  float v = readVoltageRawTrimmed();
  server.send(200, "text/plain", String(v, 4));
}

void handleCalJson() {
  String json = "{";
  json += "\"two\":" + String(useTwoLines ? "true" : "false") + ",";
  json += "\"v7\":" + String(v7_break, 4) + ",";
  json += "\"mL\":" + String(mLow, 8) + ",";
  json += "\"bL\":" + String(bLow, 8) + ",";
  json += "\"mH\":" + String(mHigh, 8) + ",";
  json += "\"bH\":" + String(bHigh, 8);
  json += "}";
  server.send(200, "application/json", json);
}

// 2-point: /setcal2?v4=...&v7=...
void handleSetCal2() {
  if (!server.hasArg("v4") || !server.hasArg("v7")) {
    server.send(400, "text/plain", "Missing v4 or v7");
    return;
  }
  float V4 = server.arg("v4").toFloat();
  float V7 = server.arg("v7").toFloat();

  if (V4 <= 0.0f || V7 <= 0.0f) {
    server.send(400, "text/plain", "Voltages must be > 0");
    return;
  }
  if (fabs(V4 - V7) < 0.01f) {
    server.send(400, "text/plain", "v4 and v7 too close (bad calibration)");
    return;
  }

  saveOneLine(V4, V7);
  server.send(200, "text/plain", "Saved ONE line using pH 4.01 & 7.00");
}

// 3-point (two lines): /setcal3?v4=...&v7=...&v9=...
void handleSetCal3() {
  if (!server.hasArg("v4") || !server.hasArg("v7") || !server.hasArg("v9")) {
    server.send(400, "text/plain", "Missing v4 or v7 or v9");
    return;
  }
  float V4 = server.arg("v4").toFloat();
  float V7 = server.arg("v7").toFloat();
  float V9 = server.arg("v9").toFloat();

  if (V4 <= 0.0f || V7 <= 0.0f || V9 <= 0.0f) {
    server.send(400, "text/plain", "Voltages must be > 0");
    return;
  }
  if (fabs(V4 - V7) < 0.01f || fabs(V9 - V7) < 0.01f || fabs(V9 - V4) < 0.01f) {
    server.send(400, "text/plain", "Two voltages are too close (bad 3-point)");
    return;
  }

  saveTwoLines(V4, V7, V9);
  server.send(200, "text/plain", "Saved TWO lines (4->7) and (7->9.18)");
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
    "<button onclick=\"window.open('/calibrate','phcal','width=560,height=740');\">Calibrate</button>"
    "</div>"
    "<small>Smooth display (updates every 2 seconds)</small>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleCalibrate() {
  // Calibration page: display entry order 4.01 first, then 7.00, then optional 9.18
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
    "    document.getElementById('mode').innerText = cal.two ? 'Two lines (4->7 and 7->9.18)' : 'One line (4->7 for all pH)';"
    "    document.getElementById('low').innerText  = `pH = ${cal.mL.toFixed(6)}*V + ${cal.bL.toFixed(6)}`;"
    "    document.getElementById('high').innerText = `pH = ${cal.mH.toFixed(6)}*V + ${cal.bH.toFixed(6)}`;"
    "  }catch(e){console.log(e);}"
    "}"
    "async function save2(){"
    "  const v4 = document.getElementById('v4').value;"
    "  const v7 = document.getElementById('v7').value;"
    "  const msg = document.getElementById('msg'); msg.innerText='';"
    "  try{"
    "    const t = await fetch(`/setcal2?v4=${encodeURIComponent(v4)}&v7=${encodeURIComponent(v7)}`,{cache:'no-store'}).then(r=>r.text());"
    "    msg.innerText=t; refresh();"
    "  }catch(e){msg.innerText='Save failed';}"
    "}"
    "async function save3(){"
    "  const v4 = document.getElementById('v4').value;"
    "  const v7 = document.getElementById('v7').value;"
    "  const v9 = document.getElementById('v9').value;"
    "  const msg = document.getElementById('msg'); msg.innerText='';"
    "  try{"
    "    const t = await fetch(`/setcal3?v4=${encodeURIComponent(v4)}&v7=${encodeURIComponent(v7)}&v9=${encodeURIComponent(v9)}`,{cache:'no-store'}).then(r=>r.text());"
    "    msg.innerText=t; refresh();"
    "  }catch(e){msg.innerText='Save failed';}"
    "}"
    "function closeMe(){ window.close(); }"
    "setInterval(refresh,2000);window.onload=refresh;"
    "</script></head><body>"
    "<div id='box'>"
    "<h2>pH Calibration</h2>"
    "<div class='row'>Live pHv (raw): <b><span id='phv'>--</span></b> V</div>"
    "<hr>"
    "<div class='row'>pH 4.01 voltage: <input id='v4' type='number' step='0.001' value='2.600'></div>"
    "<div class='row'>pH 7.00 voltage: <input id='v7' type='number' step='0.001' value='2.100'></div>"
    "<div>"
    "<button onclick='save2()'>Save ONE line (4 & 7)</button>"
    "</div>"
    "<hr>"
    "<div class='row'><b>Optional:</b> add a second line above pH 7 using pH 9.18</div>"
    "<div class='row'>pH 9.18 voltage: <input id='v9' type='number' step='0.001' value='1.800'></div>"
    "<div>"
    "<button onclick='save3()'>Save TWO lines (4,7,9.18)</button>"
    "<button class='gray' onclick='closeMe()'>Close</button>"
    "</div>"
    "<div id='msg' style='margin-top:10px;font-size:14px'></div>"
    "<p><small>Active mode: <b><span id='mode'>--</span></b></small></p>"
    "<p><small>Line for pH ≤ 7: <code><span id='low'>--</span></code></small></p>"
    "<p><small>Line for pH ≥ 7: <code><span id='high'>--</span></code></small></p>"
    "<p><small>Tip: Let readings stabilize in each buffer before saving.</small></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(200);

#if USE_INA226
  Wire.begin(); // SDA=21, SCL=22 on most ESP32 dev boards
  if (!ina226.init()) {
    Serial.println("INA226 init failed! Check wiring/address.");
  } else {
    ina226.setAverage(AVERAGE_16);             // try AVERAGE_64 for even smoother
    ina226.setConversionTime(CONV_TIME_1100);  // slower conversion = more stable
    Serial.println("INA226 OK");
  }
#else
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

  server.on("/",          HTTP_GET, handleRoot);
  server.on("/calibrate", HTTP_GET, handleCalibrate);

  server.on("/ph",     HTTP_GET, handlePH);
  server.on("/phv",    HTTP_GET, handlePHV);
  server.on("/cal",    HTTP_GET, handleCalJson);
  server.on("/setcal2", HTTP_GET, handleSetCal2);
  server.on("/setcal3", HTTP_GET, handleSetCal3);

  server.begin();
  Serial.printf("HTTP server started on port %d\n", port);
}

void loop() {
  server.handleClient();
}
