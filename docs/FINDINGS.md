# Punto 188 1.2 8V B-CAN findings

Source: ESP32-S3 listen-only sniffer, 50 kbps 11-bit, `PUNTO-CAN` logger.
Captures 2026-11-21 → 11-25: idle, revs, city, all doors+boot, lights (all modes),
blinkers L+R+hazard, brakes, defog, reverse, fan, cold start, 5 drives, fueling,
key-off → key-on → engine-start, OBD ground truth, plus 2026-11-25 app test
captures incl. the odometer 223,445 → 223,450 km session.

The 1.4 16V `CAN.md` ID set (`281/2A0/380/180`) does NOT exist on this car.

**Engine on is `RPM > 0`.** `0DD.b0 == 0 && b1 == 0` is NOT an engine-on test:
it also occurs transiently at key-on with the engine stopped, and `0x80 0x10`
means engine OFF, not on. Equivalent positive evidence: `0F0.b2 == 0x40`.

## Bus inventory (native IDs)

| ID | DLC | Rate | Role |
|---|---|---|---|
| `0DD` | 7 | 20 Hz | engine: RPM (b6×32) + coolant (b2−40) + injection pulse (b4:b5 BE) + battery/oil telltales (b0/b1) |
| `0F0` | 8 | 4 Hz | body: doors, handbrake (b0 bit5, confirmed), ignition/engine state (b2), fuel sender (b5) |
| `0E5` | 4 | 10 Hz | speed (b0:b1 BE ÷10) + awake flag (b2) + rolling distance counter (b3, +1/≈6.2 m) |
| `0CF` | 6 | 4 Hz | brakes, lights, indicators, defog |
| `0F3` | 3 | 2 Hz | handbrake echo (b0 bit 5, confirmed) |
| `0F2` | 1 | 2 Hz | CITY button |
| `700-703` | 4/2/2/2 | 1 Hz | B-CAN network management (NM) wake/sleep heartbeats |
| `740` | 0 | one-shot | key-on ping |
| `743` | 6 | one-shot | key-on PROXI handshake (static payload, NOT odometer) |
| `0C6/0C7` | 7 | ~25 s period while key ON | Fiat CODE immobilizer challenge-response keepalive — excluded from decode |

## Confirmed signals (OBD-anchored where applicable)

- **RPM** — `0DD.b6 × 32`. This equals the known 1.4 16V design (`byte 6 = rpm/32`)
  and fits OBD `010C` on par or better than a 16-bit `b6:b5` read (combined
  n=9082: 8-bit RMSE 89.0 vs 16-bit 89.6; residual is OBD pairing lag).
  Note `(b6<<8|b5)/8` is algebraically `b6×32 + b5/8` — the difference is `b5/8`,
  ≤32 rpm. `b5` is the low byte of `FuelInjectionPulse`, not the RPM low byte.
- **Battery telltale** — `0DD.b0` bit 7 (`0x80`): alternator/battery lamp. Set at
  key-on/engine-off; clears FIRST (~+0.64 s after first RPM>0), matching the
  physical cluster (battery lamp out before oil lamp).
- **Oil telltale** — `0DD.b1` bit 4 (`0x10`): oil-pressure lamp. Set at
  key-on/engine-off; clears SECOND (~+1.64 s after first RPM>0), matching the
  physical cluster.
- **Injection pulse** — `0DD.b4:b5`, 16-bit BE (`FuelInjectionPulse`). `0` with
  engine off; confirmed via DFCO coast-in-gear: drops to `0x0000` across 731
  frames at 1,600–1,760+ RPM in `drive_2_with_test3`. Exact L/h scale pending
  full-tank fill-up.
- **Distance counter** — `0E5.b3`, strictly `+1 mod 256`. Increments even during
  2–3 km/h creep while `0E5` speed reads 0 km/h; ~6.15–6.25 m/tick across 168
  moving ticks (odometer 223,445 → 223,450 km session). 8-bit: wraps every
  ~1.6 km — consumers must track wraps. NOT a checksum (verified
  non-deterministic from `b0..b2` alone while moving, constant while stopped).
- **Coolant** — `0DD.b2 − 40`. Exact-law RMSE 0.2–0.5 °C over 37–87 °C.
- **Speed** — `0E5` bytes 0:1 BE ÷10 (r=0.99 vs OBD `010D` 0–41 km/h).
- **Fuel** — `0F0.b5`, sender-side, ~percent. Fill test: monotonic rise in-file
  (`0x18→0x1F`) then step to mid-40s at half tank; leads the damped needle by
  minutes. Scale ±5 pending brim fill.
- **Ignition / engine state** — `0F0.b2`, clean ladder verified to the millisecond:
  `0x10` asleep → `0x48` key-on/engine-off → `0xC8` cranking → `0x40` running.
  Bits: `0x40` ignition on, `0x80` cranking, `0x08` engine stopped, `0x10` asleep.
- **Bus awake** — `0E5.b2` bit `0x40`: `0x00` asleep, `0x40` awake.
- **CITY** — `0F2.b0` bit `0x20`: `0x28` on / `0x08` off.
- **Doors** — `0F0.b1`: `0x04` driver, `0x08` passenger, `0x10` left-rear,
  `0x20` right-rear, `0x40` boot (`0x80` = all shut).
- **Handbrake** — `0F0.b0` bit `0x20` + `0F3.b0` bit `0x20`. Confirmed: both clear
  cleanly on handbrake release while `0F0.b0` bit 3 (`0x08`, ABS fault) and
  `0F3.b0` bit 7 (`0x80`, passenger-airbag fault) stay latched.
- **Foot brake** — `0CF.b0` bit `0x80`.
- **Lights** — `0CF.b1`: `00` off, `60` position, `68` low, `78` high, `6A` fog.
  `700.b1` bit `0x10` mirrors stalk lights (`0x1E` off / `0x0E` on).
- **Blinkers** — `0CF.b2`: `0x40` left, `0x20` right (file-proven), `0x60` hazard;
  rear defog bit `0x10`.
- **Network management** — `700-703` NM heartbeats: `700.b3` `0x01→0x09→0x0F`;
  `700.b1` `0x1A` sleep → `0x5A`/`0x9A` wake bursts → `0x1E` awake;
  `701.b1` `0x04→0x06` and `703.b1` `0x1A→0x1E` on wake; `701`/`702` active
  only with key ON; `703` also broadcast while asleep.
- **Key-on handshake** — `740` (DLC 0) then 1 ms later `743` =
  `46 76 89 95 AA A0`, once per key cycle, **~1.0 s after MAR** (the 17.26 s
  figure is elapsed recording time; key-on occurred at 16.25–16.33 s in that
  capture). Present in `captures/keyon/*`. Payload byte-identical at 223,387,
  223,445 and 223,450 km: static PROXI / node-configuration handshake, NOT
  odometer.
- **Immobilizer keepalive** — `0C6`/`0C7`, Fiat CODE challenge-response. Fires
  at key ON and every ~25.04 s while key ON / engine OFF (triplet `0C7` +
  `0C6` + `0C7`; the middle `0C7` is `06 00…`); silent while the engine runs.
  Excluded from decode.

## Disproved hypotheses

- **`743` is NOT odometer.** `46 76 89 95 AA A0` byte-for-byte identical across
  223,387 → 223,450 km. Static PROXI / node-configuration handshake.
- **`702.b1` is NOT a cumulative counter.** Reverted to `0x04`/`0x06`; bit 4
  (`0x10`) is a transient NM status flag.
- **`0F0.b6` is NOT battery voltage.** Fixed at `0x0D` whenever awake
  (`0x00` asleep) across all captures while OBD `ATRV` ranged 11.9–14.9 V.
  It is a state/config flag byte, not a measurement.
- **`0DD.b0/b1 == 0` is NOT "engine on"** (see top of file) — also true at key-on
  with the engine stopped.
- **Not on B-CAN (2001 Punto Mk2 188):** reverse lamp, central locking, cabin/boot
  lamps (byte-identical captures across engage/disengage and lock/unlock), plus
  headlight beam-level dial, car-radio power, and the cluster trip-reset button
  (zero B-CAN bit changes across dedicated action captures, 2026-11-25).

## Watchlist — needs verification (NOT in the DBC)

| # | Candidate | Evidence so far | Verification test |
|---|---|---|---|
| 1 | `0F0.b0` bit3 `0x08` = ABS warning; `0F3.b0` bit7 `0x80` = passenger-airbag/child-seat warning | Both latch a few seconds after key-on (~4 s observed) and stay latched while running; handbrake `0x20` bits clear independently on release. | Release handbrake (confirm `0x20` clears independently) and toggle the passenger airbag switch. |
| 2 | Fuel scale = percent | 24–31 ≈ quarter tank, 43–48 ≈ half tank, monotonic during fill. | Brim fill: expect ~95–100. |
| 3 | Throttle vs load identity of injection pulse low byte | `0DD.b4` alone correlates with throttle (`0111`, r≈0.85), load (`0104`, r≈0.83) and MAP (`010B`, r≈0.84) equally on steady-state drives — collinear, so correlation cannot separate them. | Same pedal position uphill vs flat: load swings, pedal doesn't. |
| 4 | `FuelInjectionPulse` L/h scale | DFCO zero + idle/load span confirmed; absolute scale unverified. | Full-tank fill-up: integrate pulse over a tank vs litres added. |

## Capture discipline

- ELM physically unplugged for pure-CAN captures (its probing pollutes the bus).
- Tag filename with engine on/off — the bus changes format between states.
- One action per file, ~10 s steady each side; `Start slow` for multi-minute runs.
- Key-cycle captures must start **before** the key is turned; power the logger
  from a bank (cigarette socket drops during cranking).
- Decimation: 1 snapshot/ID/s (fast IDs 500 ms), any payload change logged
  (200 ms floor). 4000-frame ring ⇒ ~7 min; `slow` mode ⇒ ~15+ min.

## OBD cross-reference (tiered poller since v0.8)

Fast tier `010C/010D/0104/010B/0111`; slow `0105/010F/ATRV`; background `0101`.
Supported-PID mask from `0100`(+`0120`); `1`-suffix single-frame hints with
plain-PID fallback. `CALIBRATION_MODE=false` drops `010C/0105/010D` once B-CAN
is trusted. The IAW 59F answers **no** `012F` (fuel) — CAN `0F0.b5` is the only
fuel source. K-Line init per `car-obd` (`ATZ/ATE0/ATL0/ATS0/ATH1/ATSP5/0100`).
