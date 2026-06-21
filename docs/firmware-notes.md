# Firmware Notes

- MCU: Seeed Studio XIAO ESP32-C6 (Arduino / PlatformIO)
- This describes the bring-up firmware in `firmware/src/main.cpp`.

## Pin mapping (matches schematic and netlist)

| XIAO pin | GPIO | Net | Function | Type |
|---|---|---|---|---|
| 3 | GPIO2 / A2 / D2 | `HV_SENSE` | ADC input, HV feedback | Analog in |
| 8 | GPIO17 / D7 | `HV_EN` | PWM output to UCC27517 to Q1 gate | PWM out |
| 9 | GPIO19 / D8 | `GM_CLICK` | Pulse interrupt from Q3 collector | Digital in (ISR) |
| 10 | GPIO20 / D9 | `UI_BUZZER` | Buzzer driver to R11 to Q2 base | Digital out |
| 11 | GPIO18 / D10 | `WS_DATA` | WS2812B data to R17 (330R) to LED1 | Digital out |
| 12 | - | `VCC` | 3.3 V (onboard LDO) | Power |
| 13 | - | `GND` | Ground | Power |
| 14 | - | `+5V` | USB VBUS | Power |
| 1, 2, 4 to 7 | D0, D1, D3 to D6 | - | Free (via an optional header) | - |

## 1. HV generator (HV_GEN)

- Pin: GPIO17/D7 to `HV_EN` to UCC27517 (U1) to R2 (100R) to Q1 (1N60G) gate.
- Peripheral: LEDC (LED PWM controller).
- Switching frequency: 20 to 30 kHz fixed, default 25 kHz.
- Regulation: hysteresis controller (bang-bang) with a +/-2 percent deadband
  around the setpoint.
  - Control variable: duty cycle (0 to 50 percent, DCM operation).
  - Process variable: ADC at `HV_SENSE` (GPIO2/A2/D2).
  - Setpoint: configurable in software, default 400 V for the J305.
  - ADC conversion: V_HV = V_ADC x 401 (divider ratio 13.2M : 33k = 1:401).
- Soft start: ramp the duty slowly (0 to setpoint over 500 ms) so the HV output
  capacitor (C2, 100nF/630V) is not charged instantly.
- Overvoltage protection: if V_ADC exceeds the threshold (about 450 V equivalent),
  duty goes to 0, HV_EN goes low, and a warning is shown over LED and serial.
- Control cycle: read the ADC and adjust the duty every 1 to 5 ms. No interrupt is
  needed, a simple timer task is enough.

```
+5V -> L1 (1mH) -> HV_SW -> D2 (US1M) -> HV_MAIN (about 400V)
                     ^                          |
                  Q1 Drain               C2 (100nF/630V) -> GND
                     |
                  Q1 Source -> GND

Drain protection (HV_SW):
  RCD clamp:  D1 (US1M) + C3 (1nF/630V) parallel R3 (100k) -> +5V
  RC snubber: R1 (100R) + C1 (1nF/630V) -> GND

Gate drive:
  HV_EN -> U1 (UCC27517, VDD=+5V) -> R2 (100R) -> Q1 gate
  R4 (100k) gate pulldown -> GND
  C4 (100nF) bypass right at U1
```

## 2. Pulse detection (GM_AMP to GM_CLICK)

- Pin: GPIO19/D8 to `GM_CLICK`.
- Rest state: low (about 0.2 V), Q3 (MMBT3904) saturated via R13 (1M) base bias
  from VCC (3.3 V).
- Tube pulse: rising edge on GM_CLICK (Q3 turns off briefly).
- ISR: `attachInterrupt(GM_CLICK_PIN, isr_gm_click, RISING)`.
  - In the ISR: only `click_count++` (volatile uint32_t) and store a timestamp.
  - No Serial.print, no delay, nothing blocking in the ISR.
- Debounce: in software, minimum 100 us dead time between two valid edges. The
  J305 has about 90 us dead time, so 100 us is a good value.
- Count-rate calculation: in the main loop or a timer task, read and reset
  `click_count` every second and compute CPM and CPS.

```
HV_MAIN -> R16 (1.5M) -> R14 (1.5M) -> R15 (1.5M) -> TUBE_PLUS -> tube anode
                                                          |
                                                    C6 (1nF/1kV) AC coupling
                                                          |
                                               Q3 base   <- R13 (1M -> VCC)
                                               Q3 emitter -> GND
                                               Q3 collector -> R12 (10k -> VCC) -> GM_CLICK

Tube cathode -> GND (via U4 KF301 terminal)
```

## 3. Buzzer / click sound (UI)

- Pin: GPIO20/D9 to `UI_BUZZER` to R11 (1k) to Q2 (MMBT3904) base.
- Click generation: one single pulse per GM_CLICK event.
  - GPIO high for about 300 us, Q2 conducts, the piezo
    (BUZZER1, Murata PKLCS1212E4001-R1) charges.
  - GPIO low, the piezo discharges through R18 (10k damping) and produces the
    "tock".
- Protection: D4 (1N4148WS) freewheel diode in parallel with the buzzer (cathode
  to +5 V).
- Tuning the click character:
  - Vary the pulse width (100 to 500 us) to change volume and pitch.
  - Optional: a double pulse (2 x 200 us with a 100 us gap) for a harder click.
- At high count rates (above 1000 CPM): cap the click rate at 10 per second, or use
  a rate tone (frequency proportional to count rate).

## 4. WS2812B status LED and strip

- Pin: GPIO18/D10 (XIAO pin 11) to `WS_DATA` to R17 (330R) to LED1 (WS2812B-2020),
  then DOUT to H1 (3-pin header) to an external strip.
- Library: FastLED (Arduino), or the ESP-IDF RMT peripheral.
- RMT preferred: the ESP32-C6 has RMT hardware, so no bit-banging is needed.
- Data chain: LED1 (onboard, index 0) then strip (index 1 to N).
- C10 (100nF) bypass ceramic at the strip header between +5 V and GND.

Onboard LED (LED1, WS2812B-2020) modes:

- Off: no HV, system idle.
- Green pulsing (1 Hz): HV ramping up.
- Solid green: HV stable, system ready.
- Short white flash on each GM_CLICK.
- Red blinking: fault (overvoltage, sensor timeout).
- Slow blue pulse: 0 CPM for over 60 s ("Schroedinger sleeps").

Strip (Schroedinger's Lamp mode):

- Perlin-noise shimmer on all LEDs.
- Base color green to blue (at high count rate) to white (overload).
- Tick boost: each GM_CLICK raises brightness, with an exponential fade out.
- Parameters configurable over a web interface (brightness, fade duration, tick
  boost, and so on).
- The existing code from the old Cajoe project (`firmware/reference/`) can be
  ported by changing FALLING to RISING and adjusting the pin numbers.

Current budget:

- Onboard LED: max 12 mA (one WS2812B-2020).
- Strip: up to 60 mA per LED at white / full brightness.
- USB supplies max 500 mA, so limit strip brightness in firmware.
- Above 8 LEDs: feed external 5 V through U5 (JST XH).

## 5. HV sense (HV_SNS)

- Pin: GPIO2/A2/D2 to `HV_SENSE`.
- ADC: ESP32-C6 ADC1_CH2, 12-bit (0 to 4095), reference about 3.3 V.

```
HV_MAIN -> R5 (3.3M) -> R6 (3.3M) -> R7 (3.3M) -> R8 (3.3M) -> HV_SENSE
                                                                  |
                                                      R9 (33k) -> GND
                                                      C5 (10nF) -> GND
                                                      D3 (BAV99) -> clamp to VCC/GND
```

- Divider ratio: (4 x 3.3M + 33k) / 33k = 401:1.
- Conversion: `V_HV = (adc_raw / 4095.0) * 3.3 * 401`.
- Expected values: 400 V gives V_SENSE about 1.0 V, ADC about 1240.
- Guard ring: a GND track around R5 to R8, R9 and C5 on the PCB protects against
  surface leakage.

## 6. Bring-up and debug

Order:

1. Populate only the XIAO plus C7/C8/C9. Check that 3.3 V and 5 V are clean.
2. Populate the HV section (U1, Q1, L1, D1, D2, R1 to R4, C1 to C4). Use a bench
   supply with a current limit (200 mA). Ramp HV_EN up slowly from firmware and
   check V_SENSE on a scope or multimeter.
3. Populate the HV divider (R5 to R9, C5, D3). Log the ADC value over serial and
   verify against a multimeter at test pad HV_MAIN.
4. Populate GM_AMP (R14 to R16, C6, R12, R13, Q3). Connect the tube and check
   GM_CLICK on a scope for clean rising edges on each decay.
5. Populate the UI (Q2, R11, R18, D4, BUZZER1, LED1, R17). Test buzzer and LED over
   serial commands.
6. Connect the strip and test the lamp mode.

Serial commands (115200 baud):

- `HV ON` / `HV OFF`: HV regulation on/off.
- `DUTY 20`: fixed duty without regulation (for calibration).
- `CLICK`: manual buzzer test.
- `LED R G B`: onboard LED color test.
- `STATUS`: print all current values (V_HV, duty, CPM, CPS).
- `CAL`: continuous ADC raw value plus computed voltage for calibration.
- `STRIP <n>`: set strip length.
- `STOP`: exit CAL / DUTY mode.
- `HELP`: list commands.

Safety:

- HV must not start automatically at boot. It only starts after an explicit
  command.
- Overvoltage above 460 V triggers an immediate shutdown.
- Watchdog: if the loop stalls for more than 500 ms, HV_EN goes low.

## 7. Connectors

| Designator | Type | Function | Pins |
|---|---|---|---|
| U4 | KF301-5.0-2P | Tube connection (screw terminal) | 1=TUBE_PLUS, 2=GND |
| U5 | JST XH 2-pin | External 5 V feed | 1=GND, 2=+5V |
| H1 | Pin header 1x3, 2.54 mm | WS2812B strip | 1=+5V, 2=DOUT (data), 3=GND |

## 8. Planned extensions

- WiFi/BLE: stream readings over MQTT or a BLE characteristic, web interface for
  parameters.
- Display: SPI/I2C over the free pins (D0 to D6) for CPM and dose (uSv/h).
- Data logging: SD card over SPI.
- Deep sleep: wake on GM_CLICK (for battery operation).
- OTA firmware update over WiFi.
- Calibration: CPM to uSv/h conversion (J305 about 25 CPM per uR/h, tube specific).
- Schroedinger's Lamp features:
  - Quantum breathing: baseline pulse frequency proportional to CPM.
  - Decay history: decays shown as moving dots of light on the strip.
  - Easter egg: 0 CPM for over 60 s gives a blue pulse, the first tick gives a
    white flash.
