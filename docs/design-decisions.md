# Design Decisions

This document records *why* each major circuit decision was made. The firmware
notes describe *what* the design does; this file explains the reasoning behind it.

## 1. Gate driver UCC27517 instead of direct GPIO drive

Problem: the 1N60G MOSFET (Q1) has V_GS(th) = 2 to 4 V and is specified at
V_GS = 10 V. Driving the gate directly from a 3.3 V GPIO does not turn it on
fully, which leads to high R_DS(on), poor efficiency and thermal problems.

Solution: a UCC27517DBVR (U1) gate driver IC, supplied from +5 V (USB VBUS). It
delivers 4 A peak gate current and switches the gate cleanly to 5 V. R2 (100 R)
limits the gate current and damps ringing. R4 (100k) acts as a gate pulldown and
keeps Q1 safely off during boot.

Alternative rejected: a discrete totem-pole stage (BC847 + BC857) works, but the
UCC27517 is more compact, more reliable and has internal UVLO.

## 2. Drain protection: RCD clamp plus RC snubber

Problem: when the flyback FET Q1 turns off, the leakage inductance of L1 produces
V_DS spikes. The 1N60G is rated for 600 V V_DSS, and without protection that
rating is exceeded.

Two stages are used:

1. RCD clamp: D1 (US1M) and C3 (1nF/630V) in parallel with R3 (100k) to +5 V.
   This clamps the drain voltage to HV_MAIN + V_D1 (about 401 V). The energy is
   dissipated in R3.
2. RC snubber: R1 (100 R) and C1 (1nF/630V) to GND. This damps high-frequency
   ringing on the drain edge.

Why both: the clamp limits the amplitude, the snubber damps the oscillation. At
flyback voltages, either one alone is not reliable enough.

## 3. HV divider: 4 x 3.3M instead of 3 x 4.7M

Problem (voltage derating): 0805 resistors typically have a 150 V working voltage.
At HV = 400 V with only three resistors, about 133 V drops across each one, which
sits right at the spec limit.

Solution: 4 x 3.3M (R5 to R8, all 0805) in series gives 100 V per resistor,
comfortably inside spec. The lower resistor R9 is 33k.

Divider ratio: (4 x 3.3M + 33k) / 33k = 401:1. At 400 V this gives
V_SENSE = 1.0 V, so ADC = 1240 (out of 4095). Good operating point: enough
resolution, with headroom for overvoltage.

Old version (Rev 0.1): 3 x 4.7M + 10k = 1:1411. V_SENSE was only 0.28 V at 400 V,
which wasted about 90 percent of the ADC resolution.

## 4. Anode feed: 3 x 1.5M instead of 2 x 4.7M

Problem: same voltage-derating issue. In addition, 9.4M is at the upper edge for
the J305.

Solution: 3 x 1.5M (R14 to R16, all 0805) for 4.5M total.

- About 133 V per resistor (inside the 0805 spec).
- 4.5M is within the recommended range for the J305.
- Lower total resistance means shorter dead time, which is better at high count
  rates.
- Still enough current limiting to protect the tube.

## 5. WS2812B-2020 instead of a discrete LED

Old version: a KT-0603R (red 0603 LED) with a series resistor, which can only do
on and off.

New version: a WS2812B-2020 (LED1) as an onboard RGB LED.

Advantages:

- Full RGB color for status display (green = ok, red = fault, blue = idle,
  white flash = click).
- Chainable: DOUT to a 3-pin header (H1) to an external LED strip.
- Only one GPIO pin for any number of LEDs.
- Forms the basis for the Schroedinger's Lamp mode.

R17 (330 R) is a series resistor placed directly at the MCU pin (GPIO18), not at
the LED end. It protects the GPIO with long cables and shapes the signal edge.

## 6. Buzzer: passive piezo plus freewheel plus damping

Buzzer: Murata PKLCS1212E4001-R1, a passive piezo (no internal oscillator). It is
driven by individual GPIO pulses, not a continuous tone.

Click generation: GPIO high for about 300 us charges the piezo, then GPIO low
discharges it through R18 and produces a "tock".

D4 (1N4148WS) is a freewheel diode in parallel with the buzzer (cathode to +5 V).
The piezo is capacitive and generates voltage spikes on discharge, so D4 protects
Q2 (MMBT3904).

R18 (10k) is a damping resistor in parallel with the buzzer. It controls the
discharge and shapes the click character. Without R18 the click sounds too shrill.

R11 (1k) is the base series resistor for Q2, giving I_B about 2.8 mA at a 3.3 V
GPIO, enough for safe saturation.

Rate limiting: above 1000 CPM the click rate is capped at 10 per second,
otherwise it turns into a continuous tone.

## 7. HV regulation: bang-bang instead of PID

Chosen: a hysteresis (bang-bang) controller with a +/-8 V deadband around the
400 V setpoint (about +/-2 percent).

Why not PID:

- The HV does not need to be accurate to 0.1 V. The J305 behaves the same from
  380 to 420 V.
- Bang-bang is robust against ADC noise and parameter changes.
- No tuning required (no Kp, Ki, Kd).
- Easier to debug.
- The flyback topology in discontinuous conduction mode (DCM) does not respond
  linearly to duty cycle anyway.

Control cycle: read the ADC every 1 to 5 ms. HV too low means increase duty, HV
too high means decrease duty. Overvoltage (above 460 V) means duty 0 and HV_EN
low immediately.

## 8. GM_CLICK: rising edge instead of falling edge

Cajoe design (old): pulse detection uses a falling edge. The collector transistor
is off at rest, and on a tube pulse it turns on and pulls GM_CLICK low.

This design (new): rising edge. Q3 (MMBT3904) is saturated at rest through the R13
(1M) base bias from VCC (3.3 V). GM_CLICK rests at low (about 0.2 V). On a tube
pulse, the negative AC edge through C6 overrides the bias, Q3 turns off briefly,
and GM_CLICK rises to VCC.

Firmware consequence: use
`attachInterrupt(GM_CLICK_PIN, isr, RISING)`. When porting Cajoe code, FALLING has
to be changed to RISING.

Debounce: 100 us dead time between two valid edges. The J305 has about 90 us dead
time.

## 9. C2: 100nF/630V film capacitor

Type: KYET KP104J2J1006, a through-hole film capacitor, 100nF, 630V, P = 10 mm.

Why film instead of ceramic:

- At 630 V and 100nF there are hardly any MLCC options in a reasonable package.
- Film caps have lower ESR and no piezo effects.
- No capacitance loss under DC bias (an X7R ceramic can lose up to 80 percent of
  its rated capacitance at 400 V DC).

Why 100nF (and not more):

- The Cajoe board has no large HV cap. There the HV generator runs at a high
  switching frequency, and the parasitic plus tube capacitance is enough.
- 100nF is a compromise: enough energy buffer for stable HV between switching
  cycles, but not so much that the stored energy is dangerous on a short circuit.
- The 500 ms soft start charges the cap in a controlled way.

## 10. Protection measures

ADC clamp (D3, BAV99): a BAV99 dual diode at the HV_SENSE pin clamps the signal to
GND and VCC. It is drawn in the HV_SNS block of the schematic but placed physically
right at the MCU pin on the PCB (shortest path).

Input protection: the 5 V supply comes directly from the XIAO USB-C. The XIAO has
onboard ESD and reverse-polarity protection. Additional protection (TVS, P-FET)
was not implemented for this revision and is optional for a later one.

Watchdog: a software watchdog in the firmware. If the main loop blocks for more
than 500 ms, HV_EN goes low immediately. No hardware WDT is needed since the
ESP32-C6 has an internal one.

Boot behavior: HV never starts automatically. It only starts after an explicit
`HV ON` serial command.

## 11. Tube choice: J305

The J305 is a Chinese GM tube (similar to the SBM-20), widely used in DIY Geiger
counters.

- Operating voltage: about 380 to 420 V (plateau).
- Recommended anode resistor: 4.7 to 10M, this design uses 4.5M.
- Dead time: about 90 us.
- Background: about 15 to 25 CPM.
- Sensitivity: about 25 CPM per uR/h (rough figure, tube specific).
- The CPM to uSv/h conversion has to be calibrated per individual tube.
