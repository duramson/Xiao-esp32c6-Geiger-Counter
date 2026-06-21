# PCB Layout Notes

- EDA tool: EasyEDA Pro (English UI)
- Fabrication: JLCPCB (bare boards, no assembly)
- Components: LCSC (all LCSC part numbers are in `hardware/bom.csv`)

## Board basics

- 2-layer PCB, blue soldermask, ENIG (gold-plated pads)
- M3 mounting holes in all four corners
- Top silkscreen: project name and radiation symbol
- Bottom silkscreen: repo URL, date, gear icon
- Tented vias (no epoxy fill, a JLCPCB option that saves significant cost)

## Trace widths

| Type | Width | Used for |
|---|---|---|
| HV traces | 12 mil | HV_MAIN, HV_SW, drain nets |
| Power traces | 20 mil | +5V, VCC (3.3V), GND connections |
| Signal traces | 8 mil | HV_SENSE, GM_CLICK, HV_EN, UI_BUZZER, WS_DATA |

## HV clearance

- 2 mm minimum spacing between the HV zone (HV_MAIN, HV_SW, drain nets) and the
  logic zone.
- Set through Net Rules in the copper pour dialog, not through the global safe
  spacing.
- In EasyEDA Pro: Copper Pour Properties, Net Rule tab, set the HV nets to 2 mm.
- Default safe spacing for all other nets: 0.3 mm.

## GND copper pour

- Both layers filled with a GND pour.
- Create the pour only after routing is finished.
- Thermals on GND pads for better solderability.
- In tight areas (for example around the HV components), reduce network spacing to
  0.3 to 0.5 mm so the pour can fill at all.

## Guard ring (HV divider)

A 0.3 mm GND track around the HV divider chain (R5 to R8, R9, C5) on the top layer.

Purpose: it prevents surface leakage currents between the HV traces and the
high-impedance measurement input. At 400 V with humidity, microamps can flow
across the PCB surface. The guard ring catches these and routes them to GND.

Implementation:

- Run a GND track around the divider area.
- Leave gaps where signals have to enter and exit (HV_MAIN in, HV_SENSE out).
- Not needed around the anode feed resistors (R14 to R16), since a defined current
  flows there anyway.

## Antenna keepout

- A 7 x 15 mm copper-free zone under the XIAO ceramic antenna (at the edge of the
  module).
- Both layers, no copper (neither traces nor pour).
- Created as a "Copper Prohibited Area" in EasyEDA Pro.
- The ESP32-C6 ceramic antenna needs this clearance for WiFi and BLE performance.
- It follows automatically from placing the XIAO at the board edge.

## Component placement: critical points

| Component | Placement rule | Reason |
|---|---|---|
| C4 (100nF) | Right at U1 (UCC27517), VDD to GND | Bypass, as close as possible |
| D3 (BAV99) | Right at the MCU pin (GPIO2) | ADC clamp, short protection path |
| R5 to R8 | In a straight line, not around a corner | Minimizes leakage paths, guard-ring friendly |
| C2 (film cap) | Near Q1/D2, short HV loop | Minimizes the loop area of the HV current path |
| L1 (1mH) | Spaced from sensitive signals | Magnetic stray field |
| WS2812B (LED1) | VCC/GND near the bypass (C9) | Sensitive to voltage dips |

## Test pads

| Test pad | Signal | Voltage | Caution |
|---|---|---|---|
| HV_MAIN | HV output | about 400 V | HIGH VOLTAGE |
| V_SENSE | ADC input | about 1 V | Safe |
| GM_CLICK | Pulse signal | 0 to 3.3 V | Safe |
| +5V | USB voltage | 5 V | Safe |
| VCC | LDO output | 3.3 V | Safe |
| GND | Ground | 0 V | Safe |

## JLCPCB order parameters

- Via covering: tented (NOT epoxy filled, which adds a large surcharge)
- Layers: 2
- Soldermask: blue
- Surface finish: ENIG (or HASL if cheaper)
- Assembly: no, hand soldered
- Quantity: 5 (minimum)

## Routing order (for future revisions)

1. HV traces first (12 mil), which defines the HV zone and clearances.
2. Power traces (20 mil), the +5V and VCC distribution.
3. Signal traces (8 mil), keeping HV_SENSE as short as possible.
4. GND pour last, on both layers.
