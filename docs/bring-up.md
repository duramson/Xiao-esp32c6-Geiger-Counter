# Bring-up Stages

Bringing a fresh board to life happens in stages. Each stage adds one block of
hardware, and you only move up once the previous stage has passed. The firmware
enforces this with a compile-time gate so a half-populated board cannot energize
the high-voltage section by accident.

> [!CAUTION]
> From stage 2 on, the board generates up to ~400 V DC. Use a current-limited
> bench supply, keep one hand behind your back, and discharge the HV capacitor (C2)
> before touching the board.

## How the gate works

The active stage is set at build time through `BRINGUP_STAGE` in
[`firmware/platformio.ini`](../firmware/platformio.ini):

```ini
build_flags =
    -DBRINGUP_STAGE=1
```

The firmware maps the value to a hard limit on what the board can do. The HV duty
ceiling is selected at compile time, so below stage 2 the PWM duty is clamped to 0
and the HV path is physically off no matter what you type on the serial console.

| Stage value | HV duty cap | `DUTY` (open-loop) | `HV ON` (closed-loop) | GM ISR + tick buzzer |
|---|---|---|---|---|
| 1 | 0 (off) | blocked | blocked | off |
| 2 | 40 / 255 (~16%) | allowed | blocked | off |
| 3 | 128 / 255 (50%) | allowed | allowed | off |
| 4 | 128 / 255 (50%) | allowed | allowed | on |
| 5 | 128 / 255 (50%) | allowed | allowed | on |

Blocked commands print a hint, for example
`[STAGE] HV ON (closed-loop) needs BRINGUP_STAGE >= 3`. The active stage is shown
in the boot banner, in `HELP`, and in `STATUS`.

To move up a stage, change the value in `platformio.ini`, rebuild, and flash:

```bash
cd firmware
pio run -t upload
```

## The stages

### Stage 1: Rails and UI

- Populate: XIAO module and the bypass caps (C7, C8, C9) only. No HV parts, no
  tube.
- Build: `-DBRINGUP_STAGE=1`.
- Allowed: `LED`, `CLICK`, `STATUS`, `CAL`. HV is forced off.
- Check:
  - 3.3 V and 5 V rails are clean on a multimeter.
  - `LED 30 0 0` / `LED 0 30 0` / `LED 0 0 30` drive the onboard LED.
  - `CLICK` produces an audible tick from the buzzer.
  - `CAL` shows an ADC idle reading near 0.
- Pass when: rails are stable and both LED and buzzer respond.

### Stage 2: HV open-loop

- Populate: the HV section (U1, Q1, L1, D1, D2, R1 to R4, C1 to C4). Still no
  divider feedback in the loop, no tube.
- Build: `-DBRINGUP_STAGE=2`.
- Power from a bench supply with the current limit set low (about 200 mA).
- Allowed: `DUTY <0-50>` (capped at duty 40), plus everything from stage 1.
  `HV ON` is still blocked.
- Check:
  - Raise duty in small steps with `DUTY` and watch the HV output on a meter or
    scope at the HV_MAIN test pad.
  - Confirm the drain snubber and clamp keep the switching node under control (no
    runaway, no excessive ringing).
  - Watch the supply current stays within the limit.
- Pass when: HV rises in a controlled way with duty and stays well below the 460 V
  trip.

### Stage 3: HV closed-loop

- Populate: the HV divider and ADC clamp (R5 to R9, C5, D3).
- Build: `-DBRINGUP_STAGE=3`.
- Allowed: `HV ON` (bang-bang regulation with soft start and the overvoltage trip),
  plus all earlier commands.
- Check:
  - With `CAL`, verify the ADC reading against a multimeter at HV_MAIN and confirm
    the 1:401 divider ratio.
  - `HV ON` should ramp to the 400 V setpoint and hold inside the +/-8 V band.
  - Force an overvoltage (briefly raise the setpoint or the input) and confirm the
    >460 V trip cuts HV.
- Pass when: regulation holds the setpoint and the overvoltage trip fires.

### Stage 4: GM detection

- Populate: GM_AMP (R14 to R16, C6, R12, R13, Q3) and connect the tube.
- Build: `-DBRINGUP_STAGE=4`.
- Allowed: the pulse ISR is armed and the per-tick buzzer is active, on top of full
  HV control.
- Check:
  - Scope GM_CLICK for clean rising edges on each decay.
  - `STATUS` shows a plausible background CPM (about 15 to 25 for a J305).
  - Each tick produces a buzzer click, rate-limited at high count rates.
- Pass when: counts track background and a check source, and clicks are audible.

### Stage 5: Full application

- Populate: the WS2812B strip header (H1) and any external 5 V feed (U5) if the
  strip is long.
- Build: `-DBRINGUP_STAGE=5`.
- Scope: the addressable strip, the Schroedinger's Lamp mode, and connectivity
  (WiFi/BLE) come together here. This is the target for the production firmware
  rather than the bring-up firmware in this repo.

## Notes

- Always start a new board at stage 1 and climb one step at a time. Skipping
  straight to stage 5 defeats the purpose of the gate.
- The gate is a development aid, not a substitute for safe HV practice. Treat the
  board as live whenever it is powered from stage 2 on.
