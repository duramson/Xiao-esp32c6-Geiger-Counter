/*
 * Cajoe Geiger Counter — Schrödinger's Lamp Firmware (Original)
 *
 * Hardware: ESP32 + Cajoe PCB + 22× WS2812B Strip
 * Quelle:   Eigener Prototyp (Cedrik), vor Rad2Light
 *
 * WICHTIG beim Portieren auf Rad2Light v1.0:
 *   - GEIGER_PIN:  4 → 19 (GPIO19/D8 = GM_CLICK)
 *   - LED_PIN:    10 → 18 (GPIO18/D10 = WS_DATA)
 *   - Interrupt:  FALLING → RISING (andere Verstärker-Topologie)
 *   - NUM_LEDS:   22 → je nach Strip
 *   - WiFi-Credentials entfernen / parametrisieren
 *
 * Features die portiert werden sollen:
 *   - Perlin-Noise Shimmer (inoise8)
 *   - Grün → Blau → Weiß Farbverlauf nach Zählrate
 *   - Tick-Boost mit exponentiellem Fade
 *   - Peak-Decay mit Soft-Limit
 *   - WiFi WebServer für Parameter-Tuning
 */

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>

// Hardware
#define GEIGER_PIN  4
#define LED_PIN     10
#define NUM_LEDS    22

// WiFi
const char* WIFI_SSID     = "REDACTED";
const char* WIFI_PASSWORD = "REDACTED";
IPAddress staticIP(10, 0, 90, 250);
IPAddress gateway(10, 0, 90, 1);
IPAddress subnet(255, 255, 255, 0);

// Tunable parameters (live via webserver)
uint8_t  brightness     = 200;   // Globale Helligkeit (0-255)
uint32_t fadeDurationMs = 1000;  // Dauer bis komplett ausgeblendet (ms)
uint8_t  minBrightness  = 60;     // Grundleuchten: Helligkeitsminimum (0 = aus)
uint8_t  softLimitPct   = 70;    // Ueberhitzungs-Schwelle in % der Helligkeit (0-100)
float    tickBoost      = 0.5f;  // Anstieg pro Tick (kann unter 1 sein)
uint32_t peakDecayMs    = 8000;  // Zeit bis Peak zurueck auf Schwelle faellt
uint32_t overloadMs     = 5000;  // Zeit bei Max bis Farbe weiss wird

// Berechneter Schwellenwert auf 0-255 Skala
inline uint8_t softLimitValue() { return (uint8_t)((uint32_t)brightness * softLimitPct / 100); }

CRGB leds[NUM_LEDS];
volatile bool tickDetected = false;
float   glowFloat = 0.0f;       // aktuelle Helligkeit (faded exponentiell)
float   peakFloat = 0.0f;       // Energieniveau (steigt mit Ticks, zerfaellt)
unsigned long peakMaxSince = 0;  // seit wann peak >= 255

WebServer server(80);

void IRAM_ATTR onGeigerTick() {
    tickDetected = true;
}

// ── Webserver ────────────────────────────────────────────────────────────────

void handleRoot() {
    String html = R"HTML(<!DOCTYPE html>
<html><head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Radiation Lamp</title>
  <style>
    body { font-family: sans-serif; background: #111; color: #eee; padding: 24px; max-width: 420px; margin: auto; }
    h1 { color: #4f4; margin-bottom: 24px; }
    label { display: block; margin-top: 20px; font-size: 13px; color: #888; text-transform: uppercase; letter-spacing: 1px; }
    .row { display: flex; align-items: center; gap: 12px; margin-top: 6px; }
    input[type=range] { flex: 1; accent-color: #4f4; height: 4px; }
    span { min-width: 44px; text-align: right; font-size: 15px; color: #4f4; }
    button { margin-top: 32px; width: 100%; padding: 12px; background: #4f4; color: #111; border: none; border-radius: 8px; font-size: 16px; font-weight: bold; cursor: pointer; }
    button:active { background: #3c3; }
    #status { margin-top: 12px; text-align: center; font-size: 13px; color: #666; min-height: 18px; }
  </style>
</head><body>
  <h1>Radiation Lamp</h1>

  <label>Globale Helligkeit</label>
  <div class="row">
    <input type="range" id="brightness" min="0" max="250" value=")HTML" + String(brightness) + R"HTML(" oninput="document.getElementById('b_val').textContent=this.value">
    <span id="b_val">)HTML" + String(brightness) + R"HTML(</span>
  </div>

  <label>Fade-Dauer (ms)</label>
  <div class="row">
    <input type="range" id="fadeDuration" min="200" max="15000" step="100" value=")HTML" + String(fadeDurationMs) + R"HTML(" oninput="document.getElementById('f_val').textContent=this.value">
    <span id="f_val">)HTML" + String(fadeDurationMs) + R"HTML(</span>
  </div>

  <label>Minimale Helligkeit (Grundleuchten)</label>
  <div class="row">
    <input type="range" id="minBrightness" min="0" max="200" value=")HTML" + String(minBrightness) + R"HTML(" oninput="document.getElementById('m_val').textContent=this.value">
    <span id="m_val">)HTML" + String(minBrightness) + R"HTML(</span>
  </div>

  <label>Ueberhitzungs-Schwelle (% der Helligkeit, ab hier wird Peak blau)</label>
  <div class="row">
    <input type="range" id="softLimit" min="0" max="100" value=")HTML" + String(softLimitPct) + R"HTML(" oninput="document.getElementById('sl_val').textContent=this.value+'%'">
    <span id="sl_val">)HTML" + String(softLimitPct) + R"HTML(%</span>
  </div>

  <label>Tick Boost (Anstieg pro Tick, z.B. 0.3)</label>
  <div class="row">
    <input type="range" id="tickBoost" min="1" max="100" value=")HTML" + String((int)(tickBoost * 10)) + R"HTML(" oninput="document.getElementById('tb_val').textContent=(this.value/10).toFixed(1)">
    <span id="tb_val">)HTML" + String(tickBoost, 1) + R"HTML(</span>
  </div>

  <label>Peak Decay (ms bis Peak auf Soft Limit faellt)</label>
  <div class="row">
    <input type="range" id="peakDecay" min="500" max="30000" step="500" value=")HTML" + String(peakDecayMs) + R"HTML(" oninput="document.getElementById('pd_val').textContent=this.value">
    <span id="pd_val">)HTML" + String(peakDecayMs) + R"HTML(</span>
  </div>

  <label>Overload Zeit (ms bis Weiss bei Max)</label>
  <div class="row">
    <input type="range" id="overloadMs" min="500" max="20000" step="500" value=")HTML" + String(overloadMs) + R"HTML(" oninput="document.getElementById('ol_val').textContent=this.value">
    <span id="ol_val">)HTML" + String(overloadMs) + R"HTML(</span>
  </div>

  <button onclick="save()">Speichern</button>
  <div id="status"></div>

  <script>
    function save() {
      const p = new URLSearchParams({
        brightness:    document.getElementById('brightness').value,
        fadeDuration:  document.getElementById('fadeDuration').value,
        minBrightness: document.getElementById('minBrightness').value,
        softLimit:     document.getElementById('softLimit').value,
        tickBoost:     document.getElementById('tickBoost').value,
        peakDecay:     document.getElementById('peakDecay').value,
        overloadMs:    document.getElementById('overloadMs').value,
      });
      document.getElementById('status').textContent = '...';
      fetch('/set?' + p)
        .then(r => r.text())
        .then(() => document.getElementById('status').textContent = 'Gespeichert!')
        .catch(() => document.getElementById('status').textContent = 'Fehler!');
    }
  </script>
</body></html>)HTML";
    server.send(200, "text/html", html);
}

void handleSet() {
    if (server.hasArg("brightness"))
        brightness = (uint8_t)constrain(server.arg("brightness").toInt(), 0, 255);
    if (server.hasArg("fadeDuration"))
        fadeDurationMs = (uint32_t)constrain(server.arg("fadeDuration").toInt(), 200, 15000);
    if (server.hasArg("minBrightness"))
        minBrightness = (uint8_t)constrain(server.arg("minBrightness").toInt(), 0, 200);
    if (server.hasArg("softLimit"))
        softLimitPct = (uint8_t)constrain(server.arg("softLimit").toInt(), 0, 100);
    if (server.hasArg("tickBoost"))
        tickBoost = constrain(server.arg("tickBoost").toFloat() / 10.0f, 0.1f, 10.0f);
    if (server.hasArg("peakDecay"))
        peakDecayMs = (uint32_t)constrain(server.arg("peakDecay").toInt(), 500, 30000);
    if (server.hasArg("overloadMs"))
        overloadMs = (uint32_t)constrain(server.arg("overloadMs").toInt(), 500, 20000);

    FastLED.setBrightness(brightness);
    server.send(200, "text/plain", "OK");
}

// ── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1500); // warten bis Monitor verbunden ist

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(brightness);
    FastLED.clear(true);

    pinMode(GEIGER_PIN, INPUT);
    // Cajoe idles HIGH, pulst auf LOW bei jedem Zerfall -> FALLING
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), onGeigerTick, FALLING);

    WiFi.config(staticIP, gateway, subnet);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Verbinde mit WiFi");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
        server.on("/", handleRoot);
        server.on("/set", handleSet);
        server.begin();
        Serial.println("Webserver bereit");
    } else {
        Serial.println("\nWiFi FEHLGESCHLAGEN - Geigerzaehler laeuft ohne Webserver");
    }
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    server.handleClient();
    unsigned long now = millis();
    bool ticked = false;
    uint8_t sl = softLimitValue();

    // Tick: Peak hochdruecken, Glow mitziehen
    if (tickDetected) {
        tickDetected = false;
        if (peakFloat < sl) peakFloat = sl;
        peakFloat = fminf(255.0f, peakFloat + tickBoost);
        if (peakFloat > glowFloat) glowFloat = peakFloat;
        ticked = true;
    }

    // Peak-Zerfall: Intervall proportional zu 1/excess (schnell oben, sehr langsam bei sl)
    static unsigned long lastPeakDecay = 0;
    float excess = peakFloat - sl;
    if (excess >= 0.5f) {
        uint32_t interval = (uint32_t)((float)peakDecayMs / excess);
        if (now - lastPeakDecay >= interval) {
            lastPeakDecay = now;
            peakFloat = fmaxf((float)sl, peakFloat - 1.0f);
        }
    }

    // Glow-Fade (exponentiell)
    if (glowFloat > 0.0f) {
        glowFloat *= expf(-logf(255.0f) / (float)fadeDurationMs);
        if (glowFloat < 1.0f) glowFloat = 0.0f;
    }

    // Overload-Timer (Hysterese: erst bei <250 zuruecksetzen)
    uint8_t peak = (uint8_t)peakFloat;
    if (peak >= 255) {
        if (peakMaxSince == 0) peakMaxSince = now;
    } else if (peak < 250) {
        peakMaxSince = 0;
    }

    uint8_t display = max((uint8_t)glowFloat, minBrightness);

    // Farbe: Gruen(sl) -> Blau(255) -> Weiss(255+Zeit), quadratische Kurve
    uint8_t hue = 85, sat = 255;
    if (peak > sl && sl < 255) {
        float t = (float)(peak - sl) / (float)(255 - sl);
        hue = (uint8_t)(85.0f + t * t * 75.0f);
    }
    if (peakMaxSince > 0) {
        unsigned long dt = now - peakMaxSince;
        sat = dt >= overloadMs ? 0 : (uint8_t)map(dt, 0, overloadMs, 255, 0);
    }

    // Per-LED Shimmer (Perlin-Noise) fuer organischen Look
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t noise = inoise8(i * 40, now / 5);
        int16_t offset = ((int16_t)noise - 128) * (int16_t)display / 512;
        uint8_t v = (uint8_t)constrain((int16_t)display + offset, 0, 255);
        leds[i] = CHSV(hue, sat, v);
    }

    FastLED.show();

    if (ticked) {
        String log = "TICK  " + String(display) + "/" + String(brightness);
        if (peak > sl && sl < 255 && peakMaxSince == 0) {
            float blauPct = (float)(peak - sl) / (float)(255 - sl);
            blauPct = blauPct * blauPct * 100.0f;
            log += " Blau:" + String((int)blauPct) + "%";
        }
        if (peakMaxSince > 0) {
            int weissPct = constrain((int)(100 - sat * 100 / 255), 0, 100);
            log += " Weiss:" + String(weissPct) + "%";
        }
        Serial.println(log);
    }

    delay(1);
}
