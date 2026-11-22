# Safety — read this before touching the car

Working on a live vehicle bus carries real risk: a mistake can disable the
car, trigger airbags, or start a fire. This project is listen-only **by
design**, but the wiring you add is still your responsibility.

## The golden rules

1. **Never transmit on the live bus.** A node sending at the wrong bitrate
   or the wrong ID can wedge the Body Computer, light up the dash like a
   Christmas tree, or immobilize the car. This firmware has no transmit path
   (`TWAI_MODE_LISTEN_ONLY`); keep it that way and never `sendMsgBuf` at a
   running car.
2. **No extra termination.** The car already terminates both bus ends. Leave
   the transceiver's 120 Ω jumper **OFF**. A third terminator distorts every
   frame on the segment.
3. **Fuse everything from 12 V.** Any wire to OBD pin 16 or the battery gets
   its own fuse at the source (1 A is plenty). Unfused 12 V near the dash is
   how fires start.
4. **Ground the sniffer to the car.** Floating supplies (USB power banks,
   laptop USB) let the transceiver's ground drift from chassis ground; under
   cranking that offset can corrupt frames or, in the worst case, stress the
   transceiver. One wire: ESP32 GND → OBD pin 4/5 or a chassis bolt.
5. **Stay away from airbag wiring.** You're working at the instrument
   cluster — airbag lines (usually yellow connectors/sleeves) run nearby.
   Never back-probe them with powered tools, never tap them, give SRS
   capacitors time to discharge after battery disconnect before unplugging
   anything yellow.

## Tapping wires

- Back-probe; don't cut. If you must strip, stagger joints, solder, seal
  with adhesive heat-shrink — never bare twists under the dash.
- Verify CANH/CANL identity before relying on the tap (see HARDWARE.md).
  On a listen-only tap, trying one way and swapping on silence is safe.
- Disconnect the logger before jump-starting, welding, or battery work.
  Reconnect only with the key off, then power up normally.

## Adapters and software

- Cheap ELM327 clones can auto-probe protocols on power-up and spam frames;
  unplug them for pure-CAN captures (ours were: see capture notes).
- For permanent installs prefer a genuine adapter (e.g. vLinker MC+) with
  proper sleep current — clones are known parasitic drains.
- This project never writes to the car: no PROXI alignments, no actuator
  tests, no DTC clears through our tools. Warning-lamp bits stay unwritten.

## Testing discipline

- Do all first power-ups parked, handbrake on, clear of traffic.
- Driving captures need a passenger operating the logger, or Start/Stop
  pressed while stationary. Never tap a web UI at speed.
- Keep a phone charger or jump pack handy for long engine-off sessions —
  lights-on logging drains the battery.

## Disclaimer

This is a reverse-engineering project, not a certified tool. Everything here
worked on one 2001 Punto 188 1.2 8V; other years/trims may differ. You act at
your own risk — the authors accept no liability for damage to your vehicle.
If any of the above makes you uncomfortable, stop and get help from someone
who does automotive electrics for a living.
