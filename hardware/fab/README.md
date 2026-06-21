# Fabrication Files

This folder holds the manufacturing output for ordering bare boards.

> [!WARNING]
> Preliminary and untested. No board from these files has been fabricated or
> validated yet. The schematic and layout still need a cleanup pass. Do not assume
> the output here is final.

## What goes here

| File | Description |
|---|---|
| `rad2light-gerbers.zip` | Gerber set plus drill files, exported from EasyEDA Pro |
| `rad2light-step.step` | 3D model export (optional, for enclosure work) |

Export the Gerbers from EasyEDA Pro (Fabrication, PCB Fabrication File (Gerber))
and drop the zip in this folder using the name above. Keep the BOM in
[`../bom.csv`](../bom.csv) as the single source of truth for part numbers.

## JLCPCB order parameters

These match [`../../docs/pcb-notes.md`](../../docs/pcb-notes.md):

- Layers: 2
- Via covering: tented (NOT epoxy filled, which adds a large surcharge)
- Soldermask: blue
- Surface finish: ENIG (or HASL if cheaper)
- Assembly: no, hand soldered
- Quantity: 5 (minimum)

## Notes

- Components are ordered separately from LCSC, see [`../bom.csv`](../bom.csv).
- The GM tube (J305) and the XIAO ESP32-C6 module are sourced separately and are
  not part of the fab order.
