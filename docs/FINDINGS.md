# Punto 188 1.2 8V B-CAN findings

Source: ESP32-S3 listen-only sniffer, 50 kbps 11-bit, `PUNTO-CAN` logger.
Captures 2026-11-21 → 11-25: idle, revs, city, all doors+boot, lights (all modes),
blinkers L+R+hazard, brakes, defog, reverse, fan, cold start, 5 drives, fueling,
key-off → key-on → engine-start, OBD ground truth.

The 1.4 16V `CAN.md` ID set (`281/2A0/380/180`) does NOT exist on this car.

**Engine on is `RPM > 0`.** `0DD.b0 == 0 && b1 == 0` is NOT an engine-on test:
it also occurs transiently at key-on with the engine stopped, and `0x80 0x10`
means engine OFF, not on. Equivalent positive evidence: `0F0.b2 == 0x40`.

## Bus inventory (native IDs)

| ID | DLC | Rate | Role |
|---|---|---|---|
| `0DD` | 7 | 20 Hz | engine: RPM (b6×32) + coolant (b2−40) + raw b4/b5 + warning flags |
| `0F0` | 8 | 4 Hz | body: doors, handbrake, ignition/engine state (b2), fuel sender (b5) |
| `0E5` | 4 | 10 Hz | speed (b0:b1 BE ÷10) + awake flag (b2) + unknown b3 |
| `0CF` | 6 | 4 Hz | brakes, lights, indicators, defog |
| `0F3` | 3 | 2 Hz | handbrake echo (b0 bit 5) |
| `0F2` | 1 | 2 Hz | CITY button |
| `700-703` | 4/2/2/2 | 1 Hz | B-CAN network management (NM) wake/sleep heartbeats |
| `740` | 0 | one-shot | key-on ping |
| `743` | 6 | one-shot | key-on handshake (content unverified) |
| `0C6/0C7` | 7 | rare | diagnostic-visitor frames — exclude from decode |

## Confirmed signals (OBD-anchored where applicable)

- **RPM** — `0DD.b6 × 32`. This equals the known 1.4 16V design (`byte 6 = rpm/32`)
  and fits OBD `010C` on par or better than a 16-bit `b6:b5` read (combined
  n=9082: 8-bit RMSE 89.0 vs 16-bit 89.6; residual is OBD pairing lag).
  Note `(b6<<8|b5)/8` is algebraically `b6×32 + b5/8` — the difference is `b5/8`,
  ≤32 rpm. `b5` is therefore treated as a separate byte, not the RPM low byte.
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
- **Handbrake** — `0F0.b0` bit `0x20` + `0F3.b0` bit `0x20`.
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
  capture). Present in `captures/keyon/*`.

## Disproved hypotheses

- **`0F0.b6` is NOT battery voltage.** Fixed at `0x0D` whenever awake
  (`0x00` asleep) across all captures while OBD `ATRV` ranged 11.9–14.9 V.
  It is a state/config flag byte, not a measurement.
- **`0E5.b3` is NOT a simple checksum of `b0..b2`.** 468 distinct `(b0,b1,b2)`
  payloads yield 428 ambiguous `b3` values while moving, yet `b3` is constant
  `0x23` across 2000+ stationary frames. It depends on at least one hidden input.
- **`0DD.b0/b1 == 0` is NOT "engine on"** (see top of file) — also true at key-on
  with the engine stopped.
- **Reverse lamp and central locking are not on B-CAN** (byte-identical captures
  across engage/disengage and lock/unlock; cabin/boot lamps too).

## Watchlist — needs verification (NOT in the DBC)

| # | Candidate | Evidence so far | Verification test |
|---|---|---|---|
| 1 | **`743` = odometer sync** | Bits 37..20 (`(raw48>>20)&0x3FFFF`) = `0x36899` = **223,385 km**, ~2 km from the car's ~223,387 km. Could equally be immobilizer/PROXI. | Key OFF → ON capture after driving 5+ km; compare bytes. If the field tracks distance, confirmed. |
| 2 | `0DD.b0` bit7 `0x80` = oil-pressure lamp; `0DD.b1` bit4 `0x10` = alternator/battery lamp | Both set at key-on/engine-off. `0x80` cleared 1.26 s after first RPM>0; `0x10` cleared 2.0 s after, as ATRV passed ~13.0 V. | Match cluster telltale shutoff order at start; briefly ground the oil-pressure switch. |
| 3 | `0DD.b4:b5` (16-bit BE) = injection / instant consumption | `0` engine off; ranges ~210–617 at idle/cold idle, higher on cold-start peaks. Alone, `b4` tracks OBD load (r≈0.84). | Throttle blips + coasting fuel-cut capture; compare with `0104`/`0111`. |
| 4 | `0F0.b0` bit3 `0x08` = ABS warning; `0F3.b0` bit7 `0x80` = passenger-airbag/child-seat warning | Both latch a few seconds after key-on (~4 s observed) and stay latched while running. | Release handbrake (confirm `0x20` clears independently) and toggle the passenger airbag switch. |
| 5 | `0E5.b3` = rolling distance / wheel-pulse counter | Constant while stopped, varies while moving, not a checksum. | 15 m slow roll capture; check monotonic progression. |
| 6 | `702.b1` = NM status bit vs counter | `0x06` (Nov 22) → `0x16` (Nov 24): single bit-4 flip between sessions. | 10 key cycles stationary vs a 10 km drive. |
| 7 | Fuel scale = percent | 24–31 ≈ quarter tank, 43–48 ≈ half tank, monotonic during fill. | Brim fill: expect ~95–100. |
| 8 | Throttle vs load identity of `0DD.b4` | Correlates with throttle (`0111`, r≈0.85), load (`0104`, r≈0.83) and MAP (`010B`, r≈0.84) equally on steady-state drives — collinear, so correlation cannot separate them. | Same pedal position uphill vs flat: load swings, pedal doesn't. |

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
