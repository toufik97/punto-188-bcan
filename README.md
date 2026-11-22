# Punto 188 B-CAN — reverse-engineered body CAN for the Mk2 Punto 1.2 8V

Listen-only ESP32-S3 sniffer + decoder for the Fiat Punto 188 (Mk2)
**1.2 8V Fire** body CAN bus (50 kbps, 11-bit). Every signal below was captured
from a real car and cross-checked against an OBD-II K-Line adapter.

As far as we can tell, this is the **first public signal map and DBC** for the
Punto 188 body bus: the 1.4 16V notes floating around use a different ID set
that does not appear on the 1.2 8V at all.

## What is decoded (all OBD-verified unless noted)

| Signal | Source | Law |
|---|---|---|
| Engine RPM | `0DD` b6:b5 LE16 | `raw / 8` |
| Coolant temp | `0DD.b2` | `raw − 40` °C |
| Vehicle speed | `0E5` bytes 0:1 BE | `raw / 10` km/h |
| Fuel sender | `0F0.b5` | ~percent, provisional ±5 (brim fill pending) |
| Doors (4) + boot | `0F0.b1` bits | `04/08/10/20/40` |
| CITY button | `0F2.b0` bit 0x20 | on = `0x28` |
| Handbrake | `0F0.b0` + `0F3.b0` bit 0x20 | |
| Foot brake | `0CF.b0` bit 0x80 | |
| Lights | `0CF.b1` ladder | `00` off, `60` pos, `68` low, `78` high, `6A` fog |
| Blinkers / hazard / defog | `0CF.b2` | `0x40` L, `0x20` R, `0x60` both, `0x10` defog |
| Load (proxy) | `0DD.b4` | 0–10 buckets, correlates with OBD load (exact law TBD) |

Unknown / watch: `0E5.b3` (fast, uncorrelated), `0F0.b6` (constant `0x0D`),
warning-lamp bits (all dark so far — see `docs/FINDINGS.md`). Reverse lamp and
central locking show no B-CAN trace (hard-wired / off-bus).

## Repo layout

```
firmware/esp32-can-logger/   ESP32-S3 + SN65HVD230 listen-only logger + live dash
tools/decode.py              CLI decoder + correlator (stdlib only)
tools/test_*.py              17 unit tests (no pip needed)
dbc/punto_188_8v.dbc         the map, machine-readable
docs/FINDINGS.md             full decode diary with evidence
captures/                    37 raw capture logs (merged CAN+OBD schema)
```

## Quick start

1. **Wire it** — see [HARDWARE.md](HARDWARE.md). TL;DR: cluster connector
   CAN pair → SN65HVD230 (GPIO4/5, 120R jumper **OFF**) → ESP32-S3.
   Read [SAFETY.md](SAFETY.md) first — a transmitter on this bus can strand the car.
2. **Flash it** — Arduino IDE *or* `arduino-cli`, ESP32 core 4.x:
   - **Arduino IDE:** install the ESP32 core (Boards Manager URL:
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json`),
     open `firmware/esp32-can-logger/esp32-can-logger.ino`, select
     **ESP32S3 Dev Module**, then in the Tools menu set
     **USB CDC On Boot: Enabled** and **Partition Scheme: Huge APP
     (3MB No OTA)**. Pick your board's serial port (below) and Upload.
   - **arduino-cli:**
     ```
     cd firmware/esp32-can-logger
     FQBN=esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app
     arduino-cli compile -b $FQBN .
     arduino-cli upload -b $FQBN -p <YOUR_PORT> .
     ```
   - **Finding `<YOUR_PORT>`:**
     - Windows: a `COMx` port (e.g. `COM5`) — check Device Manager →
       Ports, or watch which new port appears when you plug the board in.
     - macOS: `/dev/cu.usbmodemXXXX` (native USB) or `/dev/cu.SLAB_USBtoUART`
       / `/dev/cu.wchusbserial*` (external UART chip, needs its driver).
     - Linux: `/dev/ttyACM0` (native USB) or `/dev/ttyUSB0` (UART chip);
       add your user to the `dialout` group if permission is denied.
   - If the board sits in download mode afterwards, open the serial monitor
     once — that resets it into the app.
3. **Capture**: join WiFi `PUNTO-CAN` / `punto1234`, open `http://192.168.4.1`,
   `Start` → do one thing → `Stop` → download. `/start?slow=1` = marathon mode.
4. **Decode**:
   ```
   cd tools
   python3 decode.py summary ../captures/drive-1.csv
   python3 decode.py bits ../captures/idle.csv ../captures/drive-1.csv
   python3 decode.py ref ../captures/drive-1.csv   # merged CAN+OBD in one file
   python3 test_decode.py && python3 test_dbc.py
   ```
   Log schema (`t_ms,src,id,dlc,data`): CAN rows carry hex payloads,
   OBD rows carry decoded values — one shared `millis()` timeline.
   Pre-v0.8 split files (`ms,id,dlc,b0..` + `t_ms,pid,value`) still read.

## Contributing

Captures welcome — especially: revved OBD runs (RPM linearity),
a brim fill (fuel scale lock), central-lock with a door open, and anything
that lights up the dark bytes (`0E5.b3`, `0F0.b6`, warning lamps).
One action per file, steady 10 s each side, ELM unplugged for pure-CAN runs.
See open items in `docs/FINDINGS.md`.

## License

MIT — see [LICENSE](LICENSE). Use at your own risk; this is a
reverse-engineering project, not a certified tool. Read SAFETY.md.
