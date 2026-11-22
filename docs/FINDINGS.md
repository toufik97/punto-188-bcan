# Punto 188 1.2 8V B-CAN findings

Source: ESP32-S3 listen-only sniffer, 50 kbps 11-bit, `PUNTO-CAN` logger.
Captures 2026-11-21/22: idle, revs, city, all doors+boot, lights (all modes),
blinkers L+R+hazard, brakes (pedal + handbrake), defog, reverse, fan,
cold start, 3× driving, fueling + before/after gauge photos, OBD idle+drive.

The 1.4 16V `CAN.md` ID set (`281/2A0/380/180`) does NOT exist on this car.
All `*_guess` columns from that file read -1. This document replaces it.

## Bus inventory (10 native IDs + 2 visitors)

| ID | DLC | Rate | Role |
|---|---|---|---|
| `0DD` | 7 | 20 Hz | engine: RPM ÷8 + coolant raw−40 + load/state b4 (live only engine-on; frozen `80 10 xx…` engine-off) |
| `0F0` | 8 | 4 Hz | body: handbrake + doors + fuel b5 + state byte |
| `0E5` | 4 | 10 Hz | speed BE16 b0:b1 ÷10 + unknown fast byte b3 (b2 const 0x40) |
| `0CF` | 6 | 4 Hz | pedals + lights + indicators + defog |
| `0F3` | 3 | 2 Hz | handbrake echo byte + 2 constant |
| `0F2` | 1 | 2 Hz | buttons: CITY |
| `700/701/702/703` | 4/2/2/2 | 1 Hz | static config (`700/703` b1 bit `0x10` clears engine-off) |
| `0C6/0C7` | 7 | once each, 4 files | UDS-style single frames — suspected diagnostic-tool traffic, NOT native. Exclude from decode. |

## Confirmed signals (laws locked 2026-11-22 by revved+driving OBD runs)

- **RPM** — `0DD` LE16 `(b6<<8|b5)` **÷8 exact** (free fit 0.122 ≈ ⅛, exact RMSE ≈ fit RMSE).
- **Coolant** — `0DD.b2`: **C = raw−40** (exact-fit RMSE 0.2–0.5°C over 37–87°C, matches free fit).
- **Speed** — `0E5` bytes 0:1 BE **÷10** (r=0.99 vs OBD `010D` 0–41 km/h).
- **Fuel** — `0F0.b5` sender-side, ~percent (fill test: monotonic rise in-file + step to
  mid-40s at half tank; leads the damped needle by minutes). Scale ±5 pending brim-fill.
- **CITY** — `0F2.b0` bit `0x20`: `0x28` on / `0x08` off. Proven both directions.
- **Doors** — `0F0.b1`: `0x04` driver, `0x08` passenger, `0x10` left-rear,
  `0x20` right-rear, `0x40` boot (`0x80` = all shut). All proven open+shut.
- **Handbrake** — `0F0.b0` bit `0x20` (`28` on / `08` off) + `0F3.b0` bit `0x20`
  (`A0` on / `80` off). Proven both directions. (This + foot-brake explain the
  entire day-1 "warmup step" — it was feet, not temperature.)
- **Foot brake** — `0CF.b0` bit `0x80` (`80` pressed). Proven both directions.
- **Lights** — `0CF.b1`: `00` off, `60` position, `68` low (+`08`),
  `78` high (+`10`), `6A` fog (+`02`). Proven stepped through all modes.
  (`700.b1` bit `0x10` mirrors lights state: `0x1E` off / `0x0E` on.)
- **Blinkers** — `0CF.b2`: `0x40` left, **`0x20` right (file-proven)**, `0x60` hazard.
  Rear defog = bit `0x10`. All proven on→off.
- **Load proxy** — `0DD.b4` (0–10) tracks OBD load best (r=0.84). Display with ~ until verified.

## Provisional / single-source (needs one more capture)

- **Engine on/off** — `0DD` live (`b0=b1=0`) vs frozen (`80 10 xx…`), backed by
  `0F0.b2` engine-off bit. Observed, not toggled live yet.
- **`0F0.b6=0x0D`** — constant everywhere; prime fuel cross-check (if it never moves
  while `b5` does, it's not fuel).

## Pending tests (remind if missing)

1. **Throttle vs load** — one file: neutral revs + hill pull in gear (names `0DD.b4`).
2. Central lock/unlock: one file unlock→10s→lock (short captures so far, maybe already-in-state).
3. Blower/radiator fan: certain fan state + OBD coolant anchor.
4. Brim fill: parks `0F0.b5` scale near 95–100 if percent.
5. `700.b1` lights bit + `0E5.b3` (fast unknown) — watch in future logs.

## Watch list (unknown, tracked live)

- `0E5.b3` — fast-moving, uncorrelated with RPM/throttle/load/MAP (checksum candidate?).
- `0F0.b6=0x0D` — constant everywhere; fuel cross-check pending.
- `0CF.b1` base `0x60` = two bits for position lights (why two?) — noted, harmless.
- Central lock/unlock: no B-CAN trace (short captures / already-in-state — retest: one file unlock→10s→lock).
- Blower/radiator fan: no dedicated bit found; retest with certain fan state + OBD coolant anchor.
- Reverse lamp: byte-identical engaged vs disengaged — hard-wired, not on CAN.
- `0C6`/`0C7`: diagnostic-visitor frames, excluded from decode (likely ELM probing).

## OBD cross-reference (tiered poller since v0.8)

ESP32 polls ELM327 (BLE) with `millis()` timestamps into the merged log.
Fast tier every cycle: `010C`/`010D`/`0104`/`010B`/`0111` (`1` appended =
expect-1-frame hint, plain-PID fallback on `?`/ERROR, unsupported PIDs
skipped via the `0100`+`0120` bitmask). Slow tier every 5 s: `0105`/`010F`/`ATRV`.
Background every 30 s: `0101` (MIL + DTC count). `CALIBRATION_MODE=true`
polls everything; set `false` to drop `010C`/`0105`/`010D` (B-CAN covers them).
K-Line init per `car-obd` (`ATZ/ATE0/ATL0/ATS0/ATH1/ATSP5/0100`, slow timeouts,
`>` prompt router). `decode.py ref log.csv` auto-fits divisor (test: exact
`raw/8`), coolant curve (test: exact `raw-40`), hunts speed bytes, correlates
`0DD.b4` vs load/MAP/throttle. Legacy split files still read.

## Capture discipline (learned the hard way)

- ELM physically unplugged for pure-CAN captures (its probing pollutes the bus).
- Tag every filename with engine on/off — the bus changes format between them.
- One action per file, 10s steady each side; decimated logging (1Hz/ID + change
  trigger) holds ~7 min per capture, files stay <100KB.
