/*
 * Rad2Light - Bring-up and test firmware
 * ESP32-C6 Geiger counter (XIAO ESP32-C6)
 *
 * Serial commands (115200 baud):
 *   STATUS          Show all current values
 *   HV ON           Start HV regulation (soft start)
 *   HV OFF          Stop HV immediately
 *   DUTY <0-50>     Fixed duty cycle without regulation (calibration)
 *   CLICK           Test buzzer click
 *   LED <r> <g> <b> Onboard WS2812B color test
 *   STRIP <n>       Set strip length (default 0 = onboard only)
 *   CAL             Continuous ADC raw value plus computed HV
 *   STOP            Exit CAL / DUTY mode
 *   HELP            List commands
 *
 * Safety:
 *   - HV does not start automatically
 *   - Overvoltage (>460V) triggers an immediate shutdown
 *   - Watchdog: loop stall >500ms kills HV
 *
 * Bring-up stage:
 *   This build is gated by BRINGUP_STAGE (set in platformio.ini, default 1).
 *   Lower stages physically block the HV path so a half-populated board cannot
 *   energize 400V by accident. See docs/bring-up.md for the full ladder.
 */

#include <Arduino.h>
#include <FastLED.h>

// --- Bring-up stage gate ---
// Set with -DBRINGUP_STAGE=<n> in platformio.ini. Each stage unlocks the next
// block of hardware once the previous one has passed. See docs/bring-up.md.
//   1 = rails + UI (LED, buzzer). HV path forced off.
//   2 = HV open-loop. Manual DUTY only, hard low duty cap, no regulation.
//   3 = HV closed-loop. Bang-bang regulation, soft start, overvoltage trip.
//   4 = GM detection. Pulse ISR and per-tick buzzer enabled.
//   5 = full application (strip, lamp mode, connectivity).
#define STAGE_RAILS_UI  1
#define STAGE_HV_OPEN   2
#define STAGE_HV_CLOSED 3
#define STAGE_GM        4
#define STAGE_FULL      5

#ifndef BRINGUP_STAGE
#define BRINGUP_STAGE STAGE_RAILS_UI
#endif

#if BRINGUP_STAGE < STAGE_RAILS_UI || BRINGUP_STAGE > STAGE_FULL
#error "BRINGUP_STAGE must be between 1 (rails+UI) and 5 (full application)"
#endif

// --- Pin definitions (matches netlist) ---
#define PIN_HV_SENSE  2  // GPIO2  / A2 / D2  - ADC input
#define PIN_HV_EN     17 // GPIO17 / D7       - PWM to UCC27517
#define PIN_GM_CLICK  19 // GPIO19 / D8       - ISR input (rising edge)
#define PIN_UI_BUZZER 20 // GPIO20 / D9       - buzzer driver Q2
#define PIN_WS_DATA   18 // GPIO18 / D10      - WS2812B data

// --- Constants ---
#define HV_PWM_FREQ    25000 // 25 kHz switching frequency
#define HV_PWM_CHANNEL 0
#define HV_PWM_BITS    8 // 0-255 duty range

#define HV_DIVIDER_RATIO 401.0f // (4 x 3.3M + 33k) / 33k
#define ADC_VREF         3.3f
#define ADC_MAX          4095.0f

#define HV_TARGET_DEFAULT 400.0f // volt, J305 setpoint
#define HV_HYSTERESIS     8.0f   // +/-2% of 400V
#define HV_OVERVOLTAGE    460.0f // emergency shutdown

// HV duty ceiling, gated by the bring-up stage (hard safety cap).
#if BRINGUP_STAGE >= STAGE_HV_CLOSED
#define HV_MAX_DUTY 128 // 50% of 255, full closed-loop range
#elif BRINGUP_STAGE >= STAGE_HV_OPEN
#define HV_MAX_DUTY 40 // open-loop bench testing only, conservative cap
#else
#define HV_MAX_DUTY 0 // HV path forced off below stage 2
#endif

#define SOFT_START_MS   500 // soft start duration
#define SOFT_START_STEP 5   // duty increment per step

#define DEBOUNCE_US 100 // GM pulse debounce

#define MAX_LEDS        50  // maximum strip length
#define BUZZER_PULSE_US 300 // click pulse width

// --- Globals ---

// HV control
volatile bool hvEnabled = false;
bool hvRegulating = false;
float hvTarget = HV_TARGET_DEFAULT;
int hvDutyManual = -1; // -1 = auto regulation
int hvDutyCurrent = 0;
unsigned long hvSoftStartedAt = 0;

// GM pulse counter
volatile uint32_t clickCount = 0;
volatile uint32_t clickCountSnap = 0;
volatile unsigned long lastClickUs = 0;
uint32_t cps = 0;
uint32_t cpm = 0;
uint32_t cpmBuffer[60] = {0}; // ring buffer for 60s
uint8_t cpmIndex = 0;
bool cpmBufferFull = false;

// Modes
bool calMode = false;
unsigned long lastStatusMs = 0;
unsigned long lastSecondMs = 0;
unsigned long lastLoopMs = 0;

// LEDs
CRGB leds[MAX_LEDS];
uint8_t numLeds = 1; // 1 = onboard only

// --- ISR: Geiger pulse ---
void IRAM_ATTR onGeigerClick() {
    unsigned long now = micros();
    if (now - lastClickUs >= DEBOUNCE_US) {
        lastClickUs = now;
        clickCount++;
    }
}

// --- HV functions ---

float readHV() {
    int raw = analogRead(PIN_HV_SENSE);
    float vAdc = (raw / ADC_MAX) * ADC_VREF;
    return vAdc * HV_DIVIDER_RATIO;
}

int readADCRaw() {
    return analogRead(PIN_HV_SENSE);
}

void setDuty(int duty) {
    duty = constrain(duty, 0, HV_MAX_DUTY);
    hvDutyCurrent = duty;
    ledcWrite(HV_PWM_CHANNEL, duty);
}

void hvOff() {
    setDuty(0);
    hvEnabled = false;
    hvRegulating = false;
    hvDutyManual = -1;
    Serial.println("[HV] OFF - Duty=0");
}

void hvOn() {
    if (hvEnabled) {
        Serial.println("[HV] Already running");
        return;
    }
    Serial.println("[HV] Soft-Start...");
    hvEnabled = true;
    hvRegulating = true;
    hvDutyManual = -1;
    hvSoftStartedAt = millis();
    setDuty(0);
}

void hvRegulate() {
    if (!hvEnabled || !hvRegulating)
        return;

    float hv = readHV();

    // Overvoltage protection
    if (hv > HV_OVERVOLTAGE) {
        hvOff();
        Serial.printf("[HV] !!! OVERVOLTAGE %.0fV - EMERGENCY STOP !!!\n", hv);
        // Blink red
        leds[0] = CRGB::Red;
        FastLED.show();
        return;
    }

    // Manual duty mode (for calibration)
    if (hvDutyManual >= 0) {
        setDuty(hvDutyManual);
        return;
    }

    // Soft start: ramp duty slowly
    unsigned long elapsed = millis() - hvSoftStartedAt;
    int maxDutyNow = (elapsed >= SOFT_START_MS)
                         ? HV_MAX_DUTY
                         : map(elapsed, 0, SOFT_START_MS, SOFT_START_STEP, HV_MAX_DUTY);

    // Bang-bang regulation
    if (hv < hvTarget - HV_HYSTERESIS) {
        int newDuty = min(hvDutyCurrent + 1, maxDutyNow);
        setDuty(newDuty);
    } else if (hv > hvTarget + HV_HYSTERESIS) {
        setDuty(max(hvDutyCurrent - 2, 0));
    }
    // else: in band, hold
}

// --- Buzzer ---
void buzzerClick() {
    digitalWrite(PIN_UI_BUZZER, HIGH);
    delayMicroseconds(BUZZER_PULSE_US);
    digitalWrite(PIN_UI_BUZZER, LOW);
}

// --- CPM calculation ---
void updateCounts() {
    if (millis() - lastSecondMs < 1000)
        return;
    lastSecondMs = millis();

    // Snapshot click count (atomic read)
    noInterrupts();
    cps = clickCount;
    clickCount = 0;
    interrupts();

    // Ring buffer for CPM
    cpmBuffer[cpmIndex] = cps;
    cpmIndex = (cpmIndex + 1) % 60;
    if (cpmIndex == 0)
        cpmBufferFull = true;

    // Calculate CPM
    uint8_t seconds = cpmBufferFull ? 60 : cpmIndex;
    uint32_t total = 0;
    for (uint8_t i = 0; i < seconds; i++) {
        total += cpmBuffer[i];
    }
    cpm = (seconds > 0) ? (total * 60) / seconds : 0;
}

// --- LED feedback ---
void updateStatusLED() {
    if (!hvEnabled) {
        // Idle: dim blue pulse
        uint8_t b = (millis() / 16) % 256;
        b = (b < 128) ? b : 255 - b;
        leds[0] = CRGB(0, 0, b / 4);
    } else {
        float hv = readHV();
        if (hv < hvTarget - 20) {
            // Ramping: yellow blink
            leds[0] = (millis() % 500 < 250) ? CRGB(40, 30, 0) : CRGB::Black;
        } else if (hv >= hvTarget - HV_HYSTERESIS && hv <= hvTarget + HV_HYSTERESIS) {
            // Stable: green
            leds[0] = CRGB(0, 30, 0);
        } else {
            // Out of range: red
            leds[0] = CRGB(30, 0, 0);
        }
    }

    // Flash white on click
    static unsigned long lastFlash = 0;
    noInterrupts();
    uint32_t snap = clickCountSnap;
    interrupts();

    if (cps > 0 && millis() - lastFlash > 50) {
        leds[0] = CRGB(80, 80, 80);
        lastFlash = millis();
    }

    FastLED.show();
}

// --- Serial command parser ---
void processCommand(String cmd) {
    cmd.trim();
    cmd.toUpperCase();

    if (cmd == "HELP") {
        Serial.println("=== Rad2Light Bring-Up ===");
        Serial.printf("Build stage: %d / %d\n", BRINGUP_STAGE, STAGE_FULL);
        Serial.println("STATUS          Show all values");
        Serial.println("HV ON           Start HV closed-loop (stage >= 3)");
        Serial.println("HV OFF          Stop HV immediately");
        Serial.println("DUTY <0-50>     Fixed duty %, open-loop (stage >= 2)");
        Serial.println("CLICK           Test buzzer click");
        Serial.println("LED <r> <g> <b> Set onboard LED color");
        Serial.println("STRIP <n>       Set strip length (0=off)");
        Serial.println("CAL             Continuous ADC readout");
        Serial.println("STOP            Exit CAL/DUTY mode");
        Serial.println("==========================");
    } else if (cmd == "STATUS") {
        float hv = readHV();
        int raw = readADCRaw();
        Serial.println("--- STATUS ---");
        Serial.printf("Stage:  %d / %d (compile-time)\n", BRINGUP_STAGE, STAGE_FULL);
        Serial.printf("HV:     %.1f V (ADC raw: %d)\n", hv, raw);
        Serial.printf("Duty:   %d / %d (%.1f%%)\n", hvDutyCurrent, HV_MAX_DUTY,
                      hvDutyCurrent * 100.0f / 255.0f);
        Serial.printf("HV On:  %s  |  Regulating: %s\n", hvEnabled ? "YES" : "NO",
                      hvRegulating ? "YES" : "NO");
        Serial.printf("CPS:    %lu  |  CPM: %lu\n", (unsigned long)cps, (unsigned long)cpm);
        Serial.printf("Target: %.0f V  |  Hyst: +/-%.0f V\n", hvTarget, HV_HYSTERESIS);
        Serial.printf("LEDs:   %d (Onboard + Strip)\n", numLeds);
        Serial.println("--------------");
    } else if (cmd == "HV ON") {
#if BRINGUP_STAGE >= STAGE_HV_CLOSED
        hvOn();
#else
        Serial.println("[STAGE] HV ON (closed-loop) needs BRINGUP_STAGE >= 3");
#endif
    } else if (cmd == "HV OFF") {
        hvOff();
    } else if (cmd.startsWith("DUTY ")) {
#if BRINGUP_STAGE >= STAGE_HV_OPEN
        int pct = cmd.substring(5).toInt();
        pct = constrain(pct, 0, 50);
        int duty = map(pct, 0, 100, 0, 255);
        hvDutyManual = duty;
        hvEnabled = true;
        hvRegulating = true;
        setDuty(duty);
        Serial.printf("[HV] Manual duty: %d%% (capped at raw %d)\n", pct, hvDutyCurrent);
#else
        Serial.println("[STAGE] DUTY (HV open-loop) needs BRINGUP_STAGE >= 2");
#endif
    } else if (cmd == "CLICK") {
        buzzerClick();
        Serial.println("[BUZZ] Click!");
    } else if (cmd.startsWith("LED ")) {
        int r = 0, g = 0, b = 0;
        sscanf(cmd.c_str(), "LED %d %d %d", &r, &g, &b);
        leds[0] = CRGB(r, g, b);
        FastLED.show();
        Serial.printf("[LED] Set to R=%d G=%d B=%d\n", r, g, b);
    } else if (cmd.startsWith("STRIP ")) {
        int n = cmd.substring(6).toInt();
        n = constrain(n, 0, MAX_LEDS - 1);
        numLeds = 1 + n; // 1 onboard + n strip
        FastLED.clearData();
        Serial.printf("[LED] Strip length: %d (total %d LEDs)\n", n, numLeds);
    } else if (cmd == "CAL") {
        calMode = true;
        Serial.println("[CAL] Continuous ADC readout - send STOP to exit");
    } else if (cmd == "STOP") {
        calMode = false;
        if (hvDutyManual >= 0) {
            hvDutyManual = -1;
            Serial.println("[HV] Back to auto regulation");
        }
        Serial.println("[MODE] Stopped");
    } else {
        Serial.printf("[?] Unknown command: %s\n", cmd.c_str());
    }
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    delay(2000); // USB-CDC needs time to enumerate

    Serial.println();
    Serial.println("==================================");
    Serial.println("   Rad2Light - Bring-Up Firmware");
    Serial.printf("   Build stage: %d / %d\n", BRINGUP_STAGE, STAGE_FULL);
    Serial.println("   Type HELP for commands");
    Serial.println("==================================");
    Serial.println();

    // Pin setup
    pinMode(PIN_HV_SENSE, INPUT);
    pinMode(PIN_GM_CLICK, INPUT);
    pinMode(PIN_UI_BUZZER, OUTPUT);
    digitalWrite(PIN_UI_BUZZER, LOW);

    // HV PWM setup (off by default)
    ledcSetup(HV_PWM_CHANNEL, HV_PWM_FREQ, HV_PWM_BITS);
    ledcAttachPin(PIN_HV_EN, HV_PWM_CHANNEL);
    ledcWrite(HV_PWM_CHANNEL, 0);

    // Geiger ISR (only armed once the GM detection stage is reached)
#if BRINGUP_STAGE >= STAGE_GM
    attachInterrupt(digitalPinToInterrupt(PIN_GM_CLICK), onGeigerClick, RISING);
#endif

    // WS2812B
    FastLED.addLeds<WS2812B, PIN_WS_DATA, GRB>(leds, MAX_LEDS);
    FastLED.setBrightness(60);
    FastLED.clear(true);

    // Startup blink
    leds[0] = CRGB::Green;
    FastLED.show();
    delay(300);
    leds[0] = CRGB::Black;
    FastLED.show();

    lastSecondMs = millis();
    lastLoopMs = millis();

    Serial.println("[BOOT] Ready. HV is OFF. Send 'HV ON' to start.");
    Serial.printf("[BOOT] ADC idle reading: %d (should be ~0)\n", readADCRaw());
}

// --- Loop ---
void loop() {
    unsigned long now = millis();

    // Watchdog: if loop took >500ms, kill HV
    if (hvEnabled && (now - lastLoopMs > 500)) {
        hvOff();
        Serial.println("[WDT] Loop stalled - HV killed for safety!");
    }
    lastLoopMs = now;

    // Serial command input
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        processCommand(cmd);
    }

    // HV regulation (every 2ms)
    static unsigned long lastHvMs = 0;
    if (hvEnabled && now - lastHvMs >= 2) {
        lastHvMs = now;
        hvRegulate();
    }

    // CPM calculation (every second)
    updateCounts();

    // Buzzer click on each detected pulse
    static uint32_t lastClickProcessed = 0;
    noInterrupts();
    uint32_t currentClicks = clickCount + cps; // approximate
    interrupts();
    // Simple approach: buzz on each CPS tick
    if (cps > 0 && cps != lastClickProcessed) {
        // Handled below per-tick
    }
    lastClickProcessed = cps;

    // Per-tick buzzer (check flag from ISR), only from the GM stage on
#if BRINGUP_STAGE >= STAGE_GM
    static unsigned long lastBuzzMs = 0;
    noInterrupts();
    bool hasNewClick = (clickCount > 0);
    interrupts();
    // Buzz max 10 times/sec to avoid a continuous tone
    if (hasNewClick && hvEnabled && (now - lastBuzzMs > 100)) {
        buzzerClick();
        lastBuzzMs = now;
    }
#endif

    // CAL mode: continuous readout
    if (calMode && now - lastStatusMs >= 200) {
        lastStatusMs = now;
        int raw = readADCRaw();
        float hv = readHV();
        Serial.printf("[CAL] ADC=%4d  V_sense=%.3fV  HV=%.1fV  Duty=%d\n", raw,
                      (raw / ADC_MAX) * ADC_VREF, hv, hvDutyCurrent);
    }

    // Status LED update (30 fps)
    static unsigned long lastLedMs = 0;
    if (now - lastLedMs >= 33) {
        lastLedMs = now;
        updateStatusLED();
    }

    // Periodic status print (every 5s if HV is on)
    static unsigned long lastPrintMs = 0;
    if (hvEnabled && now - lastPrintMs >= 5000) {
        lastPrintMs = now;
        float hv = readHV();
        Serial.printf("[AUTO] HV=%.1fV  Duty=%d (%.1f%%)  CPS=%lu  CPM=%lu\n", hv, hvDutyCurrent,
                      hvDutyCurrent * 100.0f / 255.0f, (unsigned long)cps, (unsigned long)cpm);
    }

    delay(1);
}
