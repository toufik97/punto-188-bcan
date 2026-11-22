# Hardware & methodology

Car: **Fiat Punto Mk2 (Type 188), 1.2 8V Fire**, European spec.
Bus tapped: **B-CAN, 50 kbps, 11-bit identifiers**, listen-only.

## Tap point

Instrument-cluster connector (positions labeled 1–9 on the housing):
**pins 3 and 4 = the CAN pair**, a black/pink + a white/pink twisted pair.
(CANH vs CANL order was not confirmed from documentation at time of writing.)

**Identify them safely on your car before connecting anything:**

1. Ignition MAR, engine running, back-probe both wires with a multimeter (DC).
   Both idle around ~2.5 V. CAN-H averages slightly *higher* under traffic
   (pulses toward 3.5 V), CAN-L slightly *lower* (dips toward 1.5 V).
   A scope shows it unambiguously: mirror-image pulses around 2.5 V.
2. If in doubt, just try one way: **on a listen-only tap with no termination,
   swapping CANH/CANL cannot damage anything** — you'll simply see no frames,
   then you swap and they appear. This is how the original tap was found.

## Sniffer hardware

- **ESP32-S3 DevKitC-1** (generic). Any S3 devkit works; USB CDC is the console.
- **SN65HVD230 (or TJA1050) transceiver module**, 3V3-powered:
  `VCC→3V3, GND→GND, TXD→GPIO4, RXD→GPIO5, RS→GND`.
- **120 Ω termination jumper: OFF.** The car already terminates both ends;
  a third terminator distorts the bus and can set dash faults.
- Short twisted stub from the tap to the module (< 1 m).

> GPIO note: on ESP32 core 4.x the TWAI driver rejects GPIO21/22
> (`install:258`); GPIO4/5 work. Beware S3 strapping/USB pins 19/20.

## Power

- **Testing setup (as used):** 65 W PD phone-charger module in the cigarette
  lighter → USB-C → S3. Fine for bench/drive logging, with caveats:
  the socket usually dies during cranking, so the logger reboots on every
  start (ring buffer is RAM — captures must start *after* the engine runs).
- **Recommended permanent install:** OBD pin 16 → fuse → buck converter
  (e.g. XL7015, 5 V) → S3 VIN, GND to OBD pins 4/5; add a chassis-ground wire
  from ESP32 GND to the car if the supply floats (isolated power bank),
  otherwise CAN common-mode drift can corrupt frames. Never skip the fuse.
- Current draw is small (S3 + transceiver + AP); do not leave it powered
  with the car parked for days.

## Firmware

`firmware/esp32-can-logger/esp32-can-logger.ino` (Arduino-ESP32 core 4.x,
no extra libraries — BLE stack ships with the core):

- `TWAI_MODE_LISTEN_ONLY` at 50 kbps, accept-all filter. **The firmware can
  never transmit** — safety by construction, verify with `grep -n
  "sendMsgBuf\|twai_transmit" firmware/esp32-can-logger/*.ino` (no hits).
- WiFi AP `PUNTO-CAN` / `punto1234`, live page at `http://192.168.4.1`
  (20 Hz CAN view, 150 → now 90 ms browser poll), `/csv` merged log,
  `/candump` for `can-utils`.
- Decimated logging: 1 snapshot/ID/s (static IDs slower), any payload change
  logged immediately (min 200 ms floor). Same 4000-ring, ~7 min coverage;
  `/start?slow=1` stretches to ~15+ min for warmups.
- BLE central to the ELM327: init `ATZ/ATE0/ATL0/ATS0/ATH1/ATSP5/0100`
  (+`0120` mask), tiered polling (`010C/010D/0104/010B/0111` fast;
  `0105/010F/ATRV` slow; `0101` background). See `docs/FINDINGS.md`.

Flash command is in the root README. After flashing, if the board sits in
download mode, opening the serial monitor once resets it into the app.

## OBD-II adapter used here

A cheap BLE ELM327 clone (fine for logging ground truth on the bench).
**Recommended for real use: vLinker MC+** (genuine firmware, BLE + WiFi,
proper sleep current, complete AT set) — clones are known for half-broken
AT commands, aggressive auto-protocol probing that spams the bus, and
parasitic drain. The poller skips unsupported PIDs via the `0100`/`0120`
bitmask and falls back from `010C1`-style single-frame hints to plain PIDs.

## Capture protocol (how every file in `captures/` was made)

1. Key on, engine in the required state, logger on AP, page open.
2. `Start` (clears buffers) → **one action only**, steady ~10 s each side.
3. `Stop` → download. ELM **unplugged** for pure-CAN runs (its probing
   pollutes the bus); plugged in for cross-reference runs.
4. Tag the filename with the action + engine state (bus format differs
   engine on vs off).

## Decode pipeline

`tools/decode.py summary|bits|ref|candump` (stdlib only) plus 17 unit tests
against synthetic fixtures. Method per signal class:
cyclic ID inventory → one-action diffs (`bits`) → scalar correlation vs OBD
(`ref` auto-fits divisor/curve, reports exact-law RMSE: `raw/8`, `raw−40`).
Full diary with evidence: `docs/FINDINGS.md`.
