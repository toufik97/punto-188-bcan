#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLE.h>
#include "driver/twai.h"

// Punto 188 1.2 8V B-CAN 50kbps LISTEN-ONLY + BLE ELM327 cross-reference.
// CAN wiring: SN65HVD230 VCC->3V3 GND->GND TXD->GPIO4 RXD->GPIO5 RS->GND
// CANH->car CAN-H CANL->car CAN-L. 120R jumper OFF on live bus.
// OBD: ESP32-S3 talks BLE to the ELM (no BT Classic on S3). No wires.
// Power: OBD16 -> 1A fuse -> TVS -> buck 5V -> USB/VIN, GND to OBD4/5.
#define CAN_TX 4
#define CAN_RX 5
#define AP_SSID "PUNTO-CAN"
#define AP_PASS "punto1234"
#define RING_N 4000
// CALIBRATION_MODE: poll every PID for B-CAN cross-reference. When false,
// 010C/0105/010D are skipped (B-CAN supplies them) and only non-CAN vitals
// (0104/010B/0111/010F/ATRV/0101) are polled.
#define CALIBRATION_MODE true
// ELM BLE: blank = first device advertising a UART service (FFE0/NUS).
// If several BLE gadgets are nearby, set to ELM name substring, e.g. "OBD".
#define ELM_NAME ""
#define ELM_PROTO "5" // KWP2000 fast (IAW 59F). Fall back to "0" (auto) if init fails.

struct Frame { uint32_t ms; uint32_t id; uint8_t dlc; uint8_t d[8]; };
static Frame ring[RING_N];
static volatile uint16_t head = 0;
static volatile uint16_t count = 0;
static volatile bool capturing = false;
static uint32_t capStartMs = 0;

// Decimated logging with per-ID floors so jittery IDs can't flood the ring:
// fast IDs (0DD/0E5) min 500 ms, rest min 1 s, slow/static IDs min 5 s;
// any payload change still logged (catches CITY/door/blinker/step events).
// Slow marathon mode (/start?slow=1): floors x4 → ~15+ min per 4000-ring.
static bool logSlowMode = false;
#define LOG_FAST_MS 1000
#define LOG_SLOW_MS 5000
#define LOG_FLOOR_MS 200
struct SeenId { uint32_t id; uint8_t dlc; uint8_t d[8]; uint32_t lastMs; bool has; };
static SeenId seen[16];

static bool isSlowId(uint32_t id) {
  return id == 0x0F2 || id == 0x0F3 || (id >= 0x700 && id <= 0x703);
}

static bool isFastId(uint32_t id) { return id == 0x0DD || id == 0x0E5; }

static void logPush(const Frame& f) {
  SeenId* slot = nullptr;
  for (auto& s : seen) {
    if (s.has && s.id == f.id) { slot = &s; break; }
    if (!s.has && !slot) slot = &s;
  }
  if (!slot) return; // table full (16 IDs max on this bus: 10) — drop
  bool changed = !slot->has || slot->dlc != f.dlc;
  if (!changed)
    for (uint8_t i = 0; i < f.dlc; i++)
      if (slot->d[i] != f.d[i]) { changed = true; break; }
  uint32_t interval, floor;
  if (logSlowMode) {
    interval = isSlowId(f.id) ? LOG_SLOW_MS * 2 : (isFastId(f.id) ? 2000 : LOG_FAST_MS * 2);
    floor = 500;
  } else {
    interval = isSlowId(f.id) ? LOG_SLOW_MS : (isFastId(f.id) ? 500 : LOG_FAST_MS);
    floor = LOG_FLOOR_MS;
  }
  if (slot->has && (f.ms - slot->lastMs) < (changed ? floor : interval)) return;
  slot->id = f.id;
  slot->dlc = f.dlc;
  for (uint8_t i = 0; i < 8; i++) slot->d[i] = f.d[i];
  slot->lastMs = f.ms;
  slot->has = true;
  ring[head] = f;
  head = (head + 1) % RING_N;
  if (count < RING_N) count++;
}

// Live CAN decode for 1.2 8V - laws locked by revved+driving OBD runs.
// RPM = 0DD.b6 * 32   (same law as the 1.4 16V "byte 6 = rpm/32"; fits 010C on par
//                      or better than a 16-bit b6:b5 read, and b5 is a separate byte)
// Coolant = 0DD.b2 - 40.  Speed = 0E5 b0:b1 BE / 10.  Fuel = 0F0.b5 (~percent, +-5).
// ENGINE ON = RPM > 0. Do NOT use 0DD.b0/b1 == 0: it also appears at key-on with
// the engine stopped (0x80 0x10 = engine off). Positive state: 0F0.b2 == 0x40.
// Proven: CITY=0F2.b0 bit0x20; doors 0F0.b1: 0x04/0x08/0x10/0x20/0x40; handbrake 0F0.b0+F3.b0 bit0x20;
// foot brake 0CF.b0 bit0x80; lights 0CF.b1 ladder; blinkers 0CF.b2 0x40 L / 0x20 R; defog bit0x10;
// engine state ladder 0F0.b2: 0x10 asleep -> 0x48 key-on/engine-off -> 0xC8 cranking -> 0x40 running.
#define RPM_FACTOR 32.0f
static float liveRpm = 0;        // 0DD.b6 * 32
static uint8_t liveCoolRaw = 0;  // 0DD.b2
static float liveCoolEst = 0;    // 0DD.b2-40
static float liveSpeedCan = -1;  // 0E5 (b0<<8|b1)/10, -1 = no drive data yet
static int liveFuel = -1;        // 0F0.b5 sender units (~percent)
static uint8_t liveLoad = 0;     // 0DD.b4 raw (watch; suspected hi byte of b4:b5 injection)
static bool liveCity = false;    // 0F2.b0 bit 0x20 (proven)
static bool liveDoorDrv = false; // 0F0.b1 bit 0x04 (proven)
static bool liveDoorPsg = false; // 0F0.b1 bit 0x08 (proven)
static bool liveDoorLr = false;  // 0F0.b1 bit 0x10 (proven)
static bool liveDoorRr = false;  // 0F0.b1 bit 0x20 (proven)
static bool liveBoot = false;    // 0F0.b1 bit 0x40 (proven)
static bool liveHandbrake = false; // 0F0.b0 bit 0x20 (proven both directions)
static bool liveFootBrake = false; // 0CF.b0 bit 0x80 (proven both directions)
static bool liveBlinkL = false;  // 0CF.b2 bit 0x40 (proven, flashes at blink rate)
static bool liveBlinkR = false;  // 0CF.b2 bit 0x20 (proven by retest file 2026-11-22)
static bool liveDefog = false;   // 0CF.b2 bit 0x10 (proven on->off)
static bool liveEngOn = false;   // RPM > 0 (0F0.b2 == 0x40 is the positive state flag)
static bool liveIgnOn = false;   // 0F0.b2 bit 0x40
static bool liveCranking = false; // 0F0.b2 bit 0x80
static char liveLights[20] = "?"; // decoded 0CF.b1: OFF/POS/LOW/HIGH/FOG combos
static uint8_t liveF0B0 = 0, liveF0B5 = 0;
static uint8_t liveCFB0 = 0, liveF3B0 = 0, liveE5B3 = 0;
static Frame prev[12];
static uint8_t prevHead = 0;
static uint8_t prevN = 0;

WebServer srv(80);
static bool twai_ok = false;
static char twai_err[48] = "not init";

static void logReset() {
  count = 0;
  head = 0;
  capStartMs = millis();
  for (auto& s : seen) s.has = false;
}

// Coolant = raw-40 (locked 2026-11-22: exact fit RMSE 0.2-0.5C over 37-87C, matches free fit).
static float coolToC(uint8_t raw) { return raw - 40.0f; }

static void decodeGuess(uint32_t id, uint8_t* d, uint8_t dlc) {
  if (id == 0x0DD && dlc >= 7) {
    liveRpm = d[6] * RPM_FACTOR;
    liveCoolRaw = d[2];
    liveCoolEst = coolToC(d[2]);
    liveLoad = d[4];
    liveEngOn = d[6] > 0; // RPM = b6*32, so RPM > 0 iff b6 > 0
  }
  if (id == 0x0F2 && dlc >= 1) liveCity = (d[0] & 0x20) != 0;
  if (id == 0x0F0 && dlc >= 6) {
    liveHandbrake = (d[0] & 0x20) != 0;
    liveDoorDrv = (d[1] & 0x04) != 0;
    liveDoorPsg = (d[1] & 0x08) != 0;
    liveDoorLr = (d[1] & 0x10) != 0;
    liveDoorRr = (d[1] & 0x20) != 0;
    liveBoot = (d[1] & 0x40) != 0;
    liveFuel = d[5]; // sender units (~percent, fill-test proven)
    liveIgnOn = (d[2] & 0x40) != 0;   // 0F0.b2 bit 0x40
    liveCranking = (d[2] & 0x80) != 0; // 0F0.b2 bit 0x80
    liveF0B0 = d[0];
    liveF0B5 = d[5];
  }
  if (id == 0x0CF && dlc >= 3) {
    liveCFB0 = d[0];
    liveFootBrake = (d[0] & 0x80) != 0;
    // b1: 00 off, 60 pos, 68 low(+08), 78 high(+10), 6A fog(+02)
    uint8_t b1 = d[1];
    char tmp[20];
    if (b1 == 0x00) snprintf(tmp, sizeof tmp, "OFF");
    else if ((b1 & 0x60) != 0x60) snprintf(tmp, sizeof tmp, "?%02X", b1);
    else {
      int p = 0;
      p += snprintf(tmp + p, sizeof tmp - p, "POS");
      if (b1 & 0x08) p += snprintf(tmp + p, sizeof tmp - p, "+LOW");
      if (b1 & 0x10) p += snprintf(tmp + p, sizeof tmp - p, "+HIGH");
      if (b1 & 0x02) p += snprintf(tmp + p, sizeof tmp - p, "+FOG");
      if (b1 & 0x05) p += snprintf(tmp + p, sizeof tmp - p, "+%02X", b1 & 0x05);
    }
    strncpy(liveLights, tmp, sizeof liveLights - 1);
    liveBlinkL = (d[2] & 0x40) != 0;
    liveBlinkR = (d[2] & 0x20) != 0; // proven by retest file 2026-11-22
    liveDefog = (d[2] & 0x10) != 0;
  }
  if (id == 0x0F3 && dlc >= 1) liveF3B0 = d[0];
  if (id == 0x0E5 && dlc >= 4) {
    liveE5B3 = d[3];
    liveSpeedCan = ((d[0] << 8) | d[1]) / 10.0f; // BE16/10, r=0.99 vs 010D
  }
}

static void drainCan() {
  if (!twai_ok) return;
  twai_message_t m;
  while (twai_receive(&m, 0) == ESP_OK) {
    if (m.extd || m.rtr) continue;
    Frame f{ millis(), m.identifier, m.data_length_code, {0} };
    for (uint8_t i = 0; i < f.dlc && i < 8; i++) f.d[i] = m.data[i];
    decodeGuess(f.id, f.d, f.dlc); // live page stays full-rate
    if (capturing && f.id != 0x0C6 && f.id != 0x0C7) logPush(f); // drop diag visitors
    else {
      prev[prevHead] = f;
      prevHead = (prevHead + 1) % 12;
      if (prevN < 12) prevN++;
    }
  }
}

// ---------------- ELM327 over BLE (mirrors car-obd elm327.dart) ----------------
struct ObdRow { uint32_t ms; uint8_t pid; float value; }; // pid numeric, 0xFE=ATRV
#define PID_ATRV 0xFE
#define OBD_N 400
static ObdRow obdRing[OBD_N];
static uint16_t obdHead = 0, obdCount = 0;
static float obdRpm = -1, obdCool = -1000, obdSpeed = -1;
static float obdLoad = -1, obdMap = -1, obdThr = -1, obdIat = -1000, obdVolt = -1, obdDtc = -1;
static bool obdMil = false;
static uint32_t obdRpmMs = 0, obdCoolMs = 0, obdSpeedMs = 0;
static char elmState[48] = "idle";
static char elmInfo[64] = "";
// Supported-PID mask from 0100 (+0120), bytes for 0x01-0x40. Valid once parsed.
static uint8_t elmMask[8] = {0};
static bool elmMaskValid = false;
static uint8_t elmSkip[8] = {0}; // session-rejected PIDs (syntax errors), skip silently

static String elmRx;
static BLEClient elmClient;
static BLERemoteCharacteristic elmWr;
static BLERemoteCharacteristic elmNt;
static bool elmWrResp = true;
static bool elmHaveClient = false;

static void elmOnNotify(BLERemoteCharacteristic& chr, const uint8_t* p, size_t n, BLESubscription) {
  (void)chr;
  for (size_t i = 0; i < n; i++) elmRx += (char)p[i];
}

static void obdPush(uint8_t pid, float v) {
  obdRing[obdHead] = { millis(), pid, v };
  obdHead = (obdHead + 1) % OBD_N;
  if (obdCount < OBD_N) obdCount++;
  uint32_t now = millis();
  switch (pid) {
    case 0x0C: obdRpm = v; obdRpmMs = now; break;
    case 0x05: obdCool = v; obdCoolMs = now; break;
    case 0x0D: obdSpeed = v; obdSpeedMs = now; break;
    case 0x04: obdLoad = v; break;
    case 0x0B: obdMap = v; break;
    case 0x11: obdThr = v; break;
    case 0x0F: obdIat = v; break;
    case PID_ATRV: obdVolt = v; break;
    case 0x01: obdDtc = v; break;
  }
}

// 0100/0120 mask: byte0 bit7 = PID 01 ... byte3 bit0 = PID 20, etc.
static void maskSet(uint8_t pid) {
  uint8_t b = pid - 1;
  elmMask[b >> 3] |= (0x80 >> (b & 7));
}
static bool maskBit(const uint8_t* m, uint8_t pid) {
  if (pid == 0 || pid > 0x40) return false;
  uint8_t b = pid - 1;
  return m[b >> 3] & (0x80 >> (b & 7));
}
static bool maskHas(uint8_t pid) {
  if (!elmMaskValid) return true; // unknown yet — try it
  uint8_t b = pid - 1;
  if (pid == 0 || pid > 0x40) return false;
  if (elmSkip[b >> 3] & (0x80 >> (b & 7))) return false; // session-rejected
  return maskBit(elmMask, pid);
}
static void maskReject(uint8_t pid) {
  uint8_t b = pid - 1;
  elmSkip[b >> 3] |= (0x80 >> (b & 7));
}

static int hexByte(const String& s, int pos) {
  if (pos + 1 >= (int)s.length()) return -1;
  auto hv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int hi = hv(s[pos]), lo = hv(s[pos + 1]);
  return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
}

// Same framing rules as car-obd parseResponse: strip echo/SEARCHING/prompt,
// find "41<PID>", decode bytes after it. Returns false on NO DATA/short frame.
static bool parseObd(const String& raw, const char* pid, float& out) {
  String s = raw;
  s.toUpperCase();
  s.replace("SEARCHING...", " ");
  s.replace(">", " ");
  s.replace(String("01") + pid, " "); // one leading command echo only
  s.replace(" ", "");
  String marker = String("41") + pid;
  int i = s.indexOf(marker);
  if (i < 0) return false;
  String h = s.substring(i + marker.length());
  if (!strcmp(pid, "0C")) {
    int a = hexByte(h, 0), b = hexByte(h, 2);
    if (a < 0 || b < 0) return false;
    out = (a * 256 + b) / 4.0f;
    return true;
  }
  if (!strcmp(pid, "05")) {
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a - 40;
    return true;
  }
  if (!strcmp(pid, "0D")) { // speed, single byte km/h
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a;
    return true;
  }
  if (!strcmp(pid, "04")) { // load, A*100/255
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a * 100.0f / 255.0f;
    return true;
  }
  if (!strcmp(pid, "0B")) { // MAP kPa
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a;
    return true;
  }
  if (!strcmp(pid, "11")) { // throttle, A*100/255
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a * 100.0f / 255.0f;
    return true;
  }
  if (!strcmp(pid, "0F")) { // IAT, A-40
    int a = hexByte(h, 0);
    if (a < 0) return false;
    out = a - 40;
    return true;
  }
  if (!strcmp(pid, "01")) { // MIL + DTC count in first byte
    int a = hexByte(h, 0);
    if (a < 0) return false;
    obdMil = (a & 0x80) != 0;
    out = a & 0x7F;
    return true;
  }
  if (!strcmp(pid, "RV")) { // ATRV local: "12.6V" — first decimal number wins
    int st = -1;
    for (int k = 0; k < (int)s.length(); k++) {
      char c = s[k];
      if ((c >= '0' && c <= '9')) { st = k; break; }
    }
    if (st < 0) return false;
    int en = st;
    while (en < (int)s.length() && ((s[en] >= '0' && s[en] <= '9') || s[en] == '.')) en++;
    out = s.substring(st, en).toFloat();
    return out > 5 && out < 20;
  }
  return false;
}

static bool elmCmd(const char* cmd, String& out, uint32_t timeout_ms) {
  if (!(bool)elmWr) return false;
  elmRx = "";
  String s = String(cmd) + "\r";
  if (elmWrResp) elmWr.writeValue((uint8_t*)s.c_str(), s.length(), true);
  else elmWr.writeValue((uint8_t*)s.c_str(), s.length(), false);
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (elmRx.indexOf('>') >= 0) break;
    drainCan();
    delay(5);
  }
  out = elmRx;
  return out.indexOf('>') >= 0;
}

struct ElmStep { const char* cmd; uint32_t timeout; bool needOk; };
static const ElmStep ELM_INIT[] = {
  {"ATZ", 3000, false}, {"ATD", 2000, true}, {"ATE0", 2000, true},
  {"ATL0", 2000, true}, {"ATS0", 2000, true}, {"ATH1", 2000, true},
  {"ATCAF1", 2000, false}, {"ATSP" ELM_PROTO, 2000, true},
  {"ATI", 3000, false}, {"ATDPN", 3000, false}, {"0100", 12000, false},
};
// 0100/0120 supported-PID bitmask (bytes for 0x01-0x20, 0x21-0x40).
static void parseMask(const String& raw, const char* mpid, uint8_t base) {
  String s = raw;
  s.toUpperCase();
  s.replace("SEARCHING...", " ");
  s.replace(">", " ");
  s.replace(String("01") + mpid, " "); // command echo
  s.replace(" ", "");
  int i = s.indexOf(String("41") + mpid);
  if (i < 0) return;
  String h = s.substring(i + 2 + strlen(mpid));
  for (int b = 0; b < 4; b++) {
    int v = hexByte(h, b * 2);
    if (v < 0) break;
    for (int k = 0; k < 8; k++)
      if (v & (0x80 >> k)) maskSet(base + b * 8 + k + 1);
  }
}
static int elmStep = -1; // -1 scan, -2 connect, >=0 init index, 99 polling
static uint32_t elmNextMs = 0, elmPollMs = 0;

static bool elmConnected() {
  return elmHaveClient && (bool)elmClient && elmClient.isConnected();
}

static bool bleFindElm(BTAddress& addrOut, char* nameOut) {
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(true);
  BLEScan::Results res = scan.startBlocking(5000);
  bool found = false;
  int8_t bestRssi = -127;
  for (const auto& d : res) {
    String nm = d.haveName() ? d.getName() : "";
    String up = nm;
    up.toUpperCase();
    bool nameHit = ELM_NAME[0] ? (nm.indexOf(ELM_NAME) >= 0) : false;
    bool knownHit = up.indexOf("OBD") >= 0 || up.indexOf("VGATE") >= 0 ||
                    up.indexOf("VIECAR") >= 0 || up.indexOf("ELM") >= 0;
    bool svcHit = d.isAdvertisingService(BLEUUID((uint16_t)0xFFE0)) ||
                  d.isAdvertisingService(BLEUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
    Serial.printf("BLE seen: %s rssi=%d\n", nm.c_str(), (int)d.getRSSI());
    if ((nameHit || knownHit || svcHit) && d.getRSSI() > bestRssi) {
      bestRssi = d.getRSSI();
      addrOut = d.getAddress();
      snprintf(nameOut, 32, "%s", nm.c_str());
      found = true;
    }
    drainCan();
  }
  BLE.getScan().clearResults();
  return found;
}

static bool bleGrabChars(BLERemoteService& svc, const char* wrUuid, const char* ntUuid) {
  if (!svc) return false;
  BLERemoteCharacteristic w = svc.getCharacteristic(BLEUUID(wrUuid));
  BLERemoteCharacteristic n = svc.getCharacteristic(BLEUUID(ntUuid));
  if (!w || !n) return false;
  if (!(w.canWrite() || w.canWriteNoResponse())) return false;
  if (!(n.canNotify() || n.canIndicate())) return false;
  elmWr = w;
  elmNt = n;
  elmWrResp = w.canWrite();
  return (bool)elmNt.subscribe(n.canNotify() ? BLESubscription::Notify : BLESubscription::Indicate, elmOnNotify);
}

static bool bleConnectElm(const BTAddress& addr) {
  elmClient = BLE.createClient();
  if (!elmClient) return false;
  elmHaveClient = true;
  if (!elmClient.connect(addr)) return false;
  const char* svcs[] = {"ffe0", "6e400001-b5a3-f393-e0a9-e50e24dcca9e", nullptr};
  const char* wrs[] = {"ffe1", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", nullptr};
  const char* nts[] = {"ffe1", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", nullptr};
  for (int k = 0; svcs[k]; k++) {
    BLERemoteService s = elmClient.getService(BLEUUID(svcs[k]));
    if (bleGrabChars(s, wrs[k], nts[k])) return true;
  }
  // Fallback: first writable + notifiable pair within one service.
  for (auto s : elmClient.getServices()) {
    if (!s) continue;
    BLERemoteCharacteristic w, n;
    for (auto c : s.getCharacteristics()) {
      if (!c) continue;
      if (!(bool)w && (c.canWrite() || c.canWriteNoResponse())) w = c;
      if (!(bool)n && (c.canNotify() || c.canIndicate())) n = c;
    }
    if ((bool)w && (bool)n) {
      elmWr = w;
      elmNt = n;
      elmWrResp = w.canWrite();
      return (bool)n.subscribe(n.canNotify() ? BLESubscription::Notify : BLESubscription::Indicate, elmOnNotify);
    }
  }
  return false;
}

static void elmTask() {
  uint32_t now = millis();
  if ((int32_t)(now - elmNextMs) < 0) return;
  if (!elmConnected()) {
    if (elmStep != -1) { // (re)start scan cycle
      elmStep = -1;
      snprintf(elmState, sizeof elmState, "scan");
      elmNextMs = now;
      return;
    }
    // SCAN
    Serial.println("ELM: scanning...");
    BTAddress addr;
    char nm[32] = "";
    if (!bleFindElm(addr, nm)) {
      snprintf(elmState, sizeof elmState, "no ELM found");
      Serial.println("ELM: none found, retry in 15s (/blescan lists what was seen)");
      elmNextMs = now + 15000;
      return;
    }
    elmStep = -2;
    snprintf(elmInfo, sizeof elmInfo, "%s", nm);
    snprintf(elmState, sizeof elmState, "connect %s", nm);
    Serial.printf("ELM: connecting %s...\n", nm);
    if (!bleConnectElm(addr)) {
      snprintf(elmState, sizeof elmState, "connect fail");
      elmNextMs = now + 8000;
      elmStep = -1;
      return;
    }
    elmStep = 0;
    elmNextMs = now;
    return;
  }
  if (elmStep >= 0 && elmStep < (int)(sizeof(ELM_INIT) / sizeof(ELM_INIT[0]))) {
    const ElmStep& st = ELM_INIT[elmStep];
    snprintf(elmState, sizeof elmState, "init %s", st.cmd);
    Serial.printf("ELM> %s\n", st.cmd);
    String resp;
    bool ok = elmCmd(st.cmd, resp, st.timeout);
    resp.replace("\r", " ");
    Serial.printf("ELM< %s\n", resp.substring(0, 96).c_str());
    String up = resp;
    up.toUpperCase();
    bool pass = ok && (!st.needOk || up.indexOf("OK") >= 0);
    if (!strcmp(st.cmd, "ATI") && ok) snprintf(elmInfo, sizeof elmInfo, "%s", resp.substring(0, 40).c_str());
    if (!strcmp(st.cmd, "0100") && ok && up.indexOf("41 00") < 0 && up.indexOf("4100") < 0) pass = false;
    if (!pass && strcmp(st.cmd, "ATZ") && strcmp(st.cmd, "ATCAF1") && strcmp(st.cmd, "ATI") && strcmp(st.cmd, "ATDPN") && strcmp(st.cmd, "0100")) {
      snprintf(elmState, sizeof elmState, "init fail %s", st.cmd);
      if (elmConnected()) elmClient.disconnect();
      elmStep = -1;
      elmNextMs = now + 8000;
      return;
    }
    if (!strcmp(st.cmd, "0100") && pass) {
      parseMask(resp, "00", 0);
      if (maskBit(elmMask, 0x20)) {
        String r2;
        if (elmCmd("0120", r2, 6000)) parseMask(r2, "20", 0x20);
      }
      elmMaskValid = true;
      Serial.printf("ELM mask: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    elmMask[0], elmMask[1], elmMask[2], elmMask[3],
                    elmMask[4], elmMask[5], elmMask[6], elmMask[7]);
    }
    elmStep++;
    elmNextMs = now + 150;
    if (elmStep >= (int)(sizeof(ELM_INIT) / sizeof(ELM_INIT[0]))) {
      elmStep = 99;
      snprintf(elmState, sizeof elmState, "polling");
      Serial.println("ELM: init done, polling 010C/0105");
    }
    return;
  }
  if (elmStep == 99) { // tiered poller: fast every cycle, slow /5s, 0101 /30s
    static const char* fast[] = {"0C", "0D", "04", "0B", "11"};
    static uint8_t fi = 0;
    static const char* slow[] = {"05", "0F", "RV"}; // RV = ATRV (local, zero K-Line cost)
    static uint8_t si = 0;
    static uint32_t slowMs = 0, bgMs = 5000;
    const char* pid2 = nullptr;
    bool suffixed = false;
    if ((int32_t)(now - bgMs) >= 0) {
      bgMs = now + 30000;
      pid2 = "01"; // MIL + DTC count
    } else if ((int32_t)(now - slowMs) >= 0) {
      slowMs = now + 5000;
      for (uint8_t k = 0; k < 3; k++) { // one slow PID per 5 s tick (round-robin)
        si = (si + 1) % 3;
        const char* c = slow[si];
        uint8_t p = !strcmp(c, "RV") ? PID_ATRV : (uint8_t)strtoul(c, nullptr, 16);
        if (p == 0x05 && !CALIBRATION_MODE) continue; // B-CAN covers coolant
        if (p != PID_ATRV && !maskHas(p)) continue;
        pid2 = c;
        break;
      }
      if (!pid2) return; // all slow skipped — fast next call
    } else {
      if ((int32_t)(now - elmPollMs) < 0) return;
      for (uint8_t k = 0; k < 5; k++) {
        fi = (fi + 1) % 5;
        const char* c = fast[fi];
        uint8_t p = (uint8_t)strtoul(c, nullptr, 16);
        if (!CALIBRATION_MODE && (p == 0x0C || p == 0x0D)) continue; // B-CAN covers
        if (!maskHas(p)) continue;
        pid2 = c;
        suffixed = true; // "010C1": expect 1 frame, skip KWP2000 ATST wait
        break;
      }
      if (!pid2) { elmPollMs = now + 3000; return; }
    }
    uint8_t pnum;
    char cmd[8];
    if (!strcmp(pid2, "RV")) { strcpy(cmd, "ATRV"); pnum = PID_ATRV; }
    else {
      pnum = (uint8_t)strtoul(pid2, nullptr, 16);
      snprintf(cmd, sizeof cmd, "01%s%s", pid2, suffixed ? "1" : "");
    }
    String resp;
    bool gotPrompt = elmCmd(cmd, resp, 3000);
    String up = resp;
    up.toUpperCase();
    bool syntaxErr = up.indexOf("?") >= 0 || up.indexOf("ERROR") >= 0;
    if (suffixed && syntaxErr && pnum != PID_ATRV) {
      snprintf(cmd, sizeof cmd, "01%s", pid2); // fallback: plain 4-char
      gotPrompt = elmCmd(cmd, resp, 3000);
      up = resp;
      up.toUpperCase();
      syntaxErr = up.indexOf("?") >= 0 || up.indexOf("ERROR") >= 0;
    }
    if (!gotPrompt) {
      snprintf(elmState, sizeof elmState, "poll timeout");
      elmPollMs = now + 500;
      return;
    }
    if (syntaxErr) {
      if (pnum != PID_ATRV) maskReject(pnum);
      snprintf(elmState, sizeof elmState, "PID %s reject", pid2);
      elmPollMs = now + 200;
      return;
    }
    float v;
    if (parseObd(resp, !strcmp(pid2, "RV") ? "RV" : pid2, v)) {
      obdPush(pnum, v);
      snprintf(elmState, sizeof elmState, "polling");
    } else if (up.indexOf("NO DATA") >= 0) {
      snprintf(elmState, sizeof elmState, "ECU NO DATA");
    } else {
      snprintf(elmState, sizeof elmState, "parse fail %s", pid2);
    }
    elmPollMs = now + 300;
    return;
  }
}

// ---------------- web UI ----------------
static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><meta charset="utf-8"><meta name=viewport content="width=device-width,initial-scale=1">
<title>PUNTO CAN</title>
<style>
body{font-family:monospace;background:#111418;color:#e6edf3;margin:8px;font-size:15px}
h2{font-size:16px;margin:4px 0}
.sec{border:1px solid #30363d;border-radius:8px;padding:8px;margin:6px 0;background:#161b22}
.sec h3{font-size:12px;color:#8b949e;margin:0 0 4px;font-weight:normal}
.big{font-size:34px;font-weight:bold}
.mid{font-size:20px;font-weight:bold}
button{background:#1f262e;color:#e6edf3;border:1px solid #30363d;padding:8px 12px;border-radius:6px;font-family:monospace;font-size:14px;margin:2px}
.on{color:#3fb950;font-weight:bold}
.warn{color:#f0883e;font-weight:bold}
pre{background:#0d1117;padding:6px;white-space:pre-wrap;word-break:break-all;font-size:12px}
.row{display:flex;flex-wrap:wrap;gap:4px 14px}
</style>
<h2>PUNTO CAN v0.11 dash</h2>
<div class=sec><h3>DRIVE</h3><div class=row>
<div>RPM <span class=big id=r>0</span> <span id=or></span></div>
<div>SPEED <span class=mid id=cs>-</span> <span id=os></span> km/h</div>
<div>COOL <span class=mid id=c>?</span> (<span id=cr>?</span> vs OBD <span id=oc>-</span>)</div>
<div>FUEL <span class=mid id=fu>-</span>%</div>
<div><span id=eng>?</span></div>
</div></div>
<div class=sec><h3>PEDALS | LEVERS | LIGHTS</h3><div class=row>
<div>HB <b id=hb>?</b></div><div>FB <b id=fb>?</b></div>
<div>CITY <b id=ct>?</b></div><div>LIGHTS <b id=li>?</b></div>
<div>L <b id=bl>?</b></div><div>R <b id=br>?</b></div><div>DEF <b id=df>?</b></div>
</div><div class=row>
<div>Drv <b id=dd>?</b></div><div>Psg <b id=dp>?</b></div>
<div>LR <b id=lr>?</b></div><div>RR <b id=rr>?</b></div><div>Boot <b id=bt>?</b></div>
</div></div>
<div class=sec><h3>OBD BENCH <span id=elm>...</span> <span id=ei></span></h3><div class=row>
<div>L <span id=ol>-</span>%</div><div>MAP <span id=om>-</span></div><div>THR <span id=ot>-</span>%</div>
<div>IAT <span id=oi>-</span></div><div>V <span id=ov>-</span></div><div>DTC <span id=od>-</span><span id=omil></span></div>
</div></div>
<div class=sec><h3>LOG <span id=fl></span> <span id=fps></span>fps</h3><div>
<button onclick="fetch('/start')">Start</button>
<button onclick="fetch('/start?slow=1')">Start slow</button>
<button onclick="fetch('/stop')">Stop</button>
<button onclick="fetch('/clear')">Clear</button>
<a href="/csv"><button>Download log</button></a>
<a href="/candump"><button>candump</button></a>
</div><div>stored <b id=n>0</b>/<span id=cap>4000</span> <span id=el>0s</span> +obd <b id=no>0</b>
watch load <span id=ld>?</span></div>
<div><button onclick="let p=document.getElementById('raw');p.style.display=p.style.display=='none'?'block':'none'">Raw hex</button></div>
<pre id=raw style="display:none"><code id=l></code></pre>
</div>
<script>
let lastN=0,lastT=Date.now();
const T=(id,v,txt)=>{const e=document.getElementById(id);e.textContent=txt;e.className=v?'on':'';};
const V=(id,v,unit)=>{document.getElementById(id).textContent=v<0?'-':(+v).toFixed(unit?1:0);};
setInterval(async()=>{let r=await (await fetch('/live')).json();
document.getElementById('r').textContent=(+r.rpm).toFixed(0);
document.getElementById('or').textContent=r.obd_rpm<0?'':'OBD '+(+r.obd_rpm).toFixed(0);
document.getElementById('c').textContent=(+r.cool_est).toFixed(0)+'C';
document.getElementById('cr').textContent=r.cool_raw;
document.getElementById('oc').textContent=r.obd_cool<-900?'-':(+r.obd_cool).toFixed(0);
document.getElementById('cs').textContent=r.can_spd<0?'-':(+r.can_spd).toFixed(0);
document.getElementById('os').textContent=r.obd_spd<0?'':'OBD '+(+r.obd_spd).toFixed(0);
document.getElementById('fu').textContent=r.fuel<0?'-':(+r.fuel).toFixed(0);
document.getElementById('eng').textContent=r.eng?'ENG-ON':(r.crank?'CRANK':(r.ign?'IGN-ON':'off'));
document.getElementById('ct').textContent=r.city?'CITY-ON':'off';
document.getElementById('li').textContent=r.lights;
T('bl',r.bl,r.bl?'***':'off');T('br',r.br,r.br?'***':'off');T('df',r.defog,r.defog?'ON':'off');
T('hb',r.hbrake,r.hbrake?'HB-ON':'off');T('fb',r.fbrake,r.fbrake?'BRAKE':'off');
T('dd',r.drv,r.drv?'OPEN':'shut');T('dp',r.psg,r.psg?'OPEN':'shut');
T('lr',r.lr,r.lr?'OPEN':'shut');T('rr',r.rr,r.rr?'OPEN':'shut');T('bt',r.boot,r.boot?'OPEN':'shut');
V('ol',r.obd_load);V('om',r.obd_map);V('ot',r.obd_thr);V('oi',r.obd_iat);V('ov',r.obd_volt,1);
document.getElementById('od').textContent=r.obd_dtc<0?'-':(+r.obd_dtc).toFixed(0);
document.getElementById('omil').textContent=r.obd_mil?' MIL!':'';
document.getElementById('elm').textContent=r.elm;
document.getElementById('ei').textContent=r.elminfo||'';
document.getElementById('ld').textContent=r.load;
document.getElementById('n').textContent=r.count;
document.getElementById('cap').textContent=r.cap;
document.getElementById('el').textContent=r.elaps+'s';
document.getElementById('no').textContent=r.obd_n;
document.getElementById('fl').textContent=r.capturing?'(CAPTURING)':'(idle)';
let dt=(Date.now()-lastT)/1000; if(dt>0.5){document.getElementById('fps').textContent=((r.count-lastN)/dt).toFixed(0); lastN=r.count; lastT=Date.now();}
document.getElementById('l').textContent=r.last.join('\n');},90);
</script>)HTML";

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("PUNTO CAN v0.11 dash boot");
  pinMode(48, OUTPUT);
  digitalWrite(48, HIGH);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t ei = twai_driver_install(&g, &t, &f);
  esp_err_t es = (ei == ESP_OK) ? twai_start() : ei;
  if (ei == ESP_OK && es == ESP_OK) {
    twai_ok = true;
    snprintf(twai_err, sizeof twai_err, "ok");
    Serial.println("TWAI listen-only 50k OK");
  } else {
    snprintf(twai_err, sizeof twai_err, "install:%d start:%d", (int)ei, (int)es);
    Serial.print("TWAI fail: ");
    Serial.println(twai_err);
  }
  if (!BLE.begin("PuntoCAN")) Serial.println("BLE init FAILED (no ELM)");
  else Serial.println("BLE init, ELM scan starts in 5s");
  Serial.println("Open http://192.168.4.1");

  srv.on("/", [] { srv.send_P(200, "text/html", PAGE); });
  srv.on("/start", [] {
    logSlowMode = srv.hasArg("slow") && srv.arg("slow") == "1";
    logReset();
    obdHead = 0;
    obdCount = 0;
    capturing = true;
    srv.send(200, "text/plain", "cap");
  });
  srv.on("/stop", [] {
    capturing = false;
    srv.send(200, "text/plain", "idle");
  });
  srv.on("/clear", [] {
    count = 0;
    head = 0;
    obdHead = 0;
    obdCount = 0;
    srv.send(200, "text/plain", "clr");
  });
  srv.on("/live", [] {
    String last;
    if (capturing) {
      uint16_t n = count < 12 ? count : 12;
      for (uint16_t i = 0; i < n; i++) {
        uint16_t idx = (head + RING_N - 1 - i) % RING_N;
        char b[64];
        snprintf(b, sizeof b, "%lu %03X %u %02X%02X%02X%02X%02X%02X%02X%02X",
                 (unsigned long)ring[idx].ms, ring[idx].id, ring[idx].dlc,
                 ring[idx].d[0], ring[idx].d[1], ring[idx].d[2], ring[idx].d[3],
                 ring[idx].d[4], ring[idx].d[5], ring[idx].d[6], ring[idx].d[7]);
        last += String(b) + "\n";
      }
    } else {
      for (uint8_t i = 0; i < prevN; i++) {
        uint8_t idx = (prevHead + 12 - 1 - i) % 12;
        char b[64];
        snprintf(b, sizeof b, "%lu %03X %u %02X%02X%02X%02X%02X%02X%02X%02X",
                 (unsigned long)prev[idx].ms, prev[idx].id, prev[idx].dlc,
                 prev[idx].d[0], prev[idx].d[1], prev[idx].d[2], prev[idx].d[3],
                 prev[idx].d[4], prev[idx].d[5], prev[idx].d[6], prev[idx].d[7]);
        last += String(b) + "\n";
      }
    }
    char f0b0[8], f0b5[8], cf[8], f3[8], e5[8];
    snprintf(f0b0, sizeof f0b0, "%02X", liveF0B0);
    snprintf(f0b5, sizeof f0b5, "%02X", liveF0B5);
    snprintf(cf, sizeof cf, "%02X", liveCFB0);
    snprintf(f3, sizeof f3, "%02X", liveF3B0);
    snprintf(e5, sizeof e5, "%02X", liveE5B3);
    uint32_t now = millis();
    char j[800];
    snprintf(j, sizeof j, "{\"rpm\":%.0f,\"cool_raw\":%u,\"cool_est\":%.0f,\"can_spd\":%.0f,\"fuel\":%d,\"city\":%s,\"drv\":%s,\"psg\":%s,\"lr\":%s,\"rr\":%s,\"boot\":%s,\"hbrake\":%s,\"fbrake\":%s,\"bl\":%s,\"br\":%s,\"defog\":%s,\"eng\":%s,\"ign\":%s,\"crank\":%s,\"lights\":\"%s\",\"load\":%u,\"f0b0\":\"%s\",\"f0b5\":\"%s\",\"cfb0\":\"%s\",\"f3b0\":\"%s\",\"e5b3\":\"%s\",\"obd_rpm\":%.0f,\"obd_cool\":%.0f,\"obd_spd\":%.0f,\"obd_load\":%.0f,\"obd_map\":%.0f,\"obd_thr\":%.0f,\"obd_iat\":%.0f,\"obd_volt\":%.1f,\"obd_dtc\":%.0f,\"obd_mil\":%s,\"obd_age\":%lu,\"elm\":\"%s\",\"elminfo\":\"%s\",\"count\":%u,\"cap\":%u,\"elaps\":%lu,\"obd_n\":%u,\"capturing\":%s,\"last\":[",
             liveRpm, liveCoolRaw, liveCoolEst, liveSpeedCan, liveFuel, liveCity ? "true" : "false",
             liveDoorDrv ? "true" : "false", liveDoorPsg ? "true" : "false",
             liveDoorLr ? "true" : "false", liveDoorRr ? "true" : "false",
             liveBoot ? "true" : "false",
             liveHandbrake ? "true" : "false", liveFootBrake ? "true" : "false",
             liveBlinkL ? "true" : "false", liveBlinkR ? "true" : "false",
             liveDefog ? "true" : "false", liveEngOn ? "true" : "false",
             liveIgnOn ? "true" : "false", liveCranking ? "true" : "false", liveLights,
             liveLoad,
             f0b0, f0b5, cf, f3, e5,
             obdRpm, obdCool, obdSpeed, obdLoad, obdMap, obdThr, obdIat, obdVolt, obdDtc,
             obdMil ? "true" : "false",
             obdRpmMs ? (now - obdRpmMs) / 1000 : 9999,
             elmState, elmInfo, count, (unsigned)RING_N, capturing ? (now - capStartMs) / 1000 : 0,
             obdCount, capturing ? "true" : "false");
    String o = j;
    String e = last;
    e.replace("\\", "\\\\");
    e.replace("\"", "\\\"");
    e.replace("\n", "\",\"");
    o += "\"" + e + "\"]}";
    srv.send(200, "application/json", o);
  });
  // Merged log: CAN + OBD interleaved on one millis() timeline.
  // t_ms,src,id,dlc,data — CAN: id hex + hex payload; OBD: id PID/ATRV + value.
  srv.on("/csv", [] {
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.send(200, "text/csv", "t_ms,src,id,dlc,data\n");
    String chunk;
    chunk.reserve(2048);
    uint16_t n = count, m = obdCount, i = 0, j = 0;
    char hb[17];
    while (i < n || j < m) {
      bool takeCan;
      uint16_t ci, oj;
      if (i < n && j < m) {
        ci = (head + RING_N - n + i) % RING_N;
        oj = (obdHead + OBD_N - m + j) % OBD_N;
        takeCan = ring[ci].ms <= obdRing[oj].ms;
      } else takeCan = (i < n);
      char r[128];
      if (takeCan) {
        ci = (head + RING_N - n + i) % RING_N;
        const Frame& fr = ring[ci];
        int p = 0;
        for (uint8_t k = 0; k < fr.dlc && p < 16; k++) p += snprintf(hb + p, sizeof hb - p, "%02X", fr.d[k]);
        hb[p] = 0;
        snprintf(r, sizeof r, "%lu,CAN,%03X,%u,%s\n",
                 (unsigned long)fr.ms, fr.id, fr.dlc, hb);
        i++;
      } else {
        oj = (obdHead + OBD_N - m + j) % OBD_N;
        const ObdRow& o = obdRing[oj];
        if (o.pid == PID_ATRV)
          snprintf(r, sizeof r, "%lu,OBD,ATRV,0,%.1f\n", (unsigned long)o.ms, o.value);
        else
          snprintf(r, sizeof r, "%lu,OBD,01%02X,0,%.1f\n", (unsigned long)o.ms, o.pid, o.value);
        j++;
      }
      chunk += r;
      if (chunk.length() > 1800) {
        srv.sendContent(chunk);
        chunk = "";
      }
      if (((i + j) & 511) == 511) delay(1);
    }
    if (chunk.length()) srv.sendContent(chunk);
  });
  // OBD ground truth for decode.py ref: t_ms,pid,value (12=rpm, 05=coolant, 0D=speed)
  srv.on("/obd.csv", [] {
    String h = "t_ms,pid,value\n";
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.send(200, "text/csv", h);
    uint16_t n = obdCount;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t idx = (obdHead + OBD_N - n + i) % OBD_N;
      const ObdRow& r = obdRing[idx];
      char b[48];
      if (r.pid == PID_ATRV)
        snprintf(b, sizeof b, "%lu,ATRV,%.1f\n", (unsigned long)r.ms, r.value);
      else
        snprintf(b, sizeof b, "%lu,01%02X,%.1f\n", (unsigned long)r.ms, r.pid, r.value);
      srv.sendContent(b);
    }
  });
  srv.on("/candump", [] {
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.send(200, "text/plain", "");
    uint16_t n = count;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t idx = (head + RING_N - n + i) % RING_N;
      const Frame& fr = ring[idx];
      char r[96];
      int p = snprintf(r, sizeof r, "(%lu.%03lu) can0 %03X#",
                       (unsigned long)(fr.ms / 1000), (unsigned long)(fr.ms % 1000), fr.id);
      for (uint8_t k = 0; k < fr.dlc && p < (int)sizeof r - 3; k++) p += snprintf(r + p, sizeof r - p, "%02X", fr.d[k]);
      snprintf(r + p, sizeof r - p, "\n");
      srv.sendContent(r);
      if ((i & 63) == 63) delay(1);
    }
  });
  srv.begin();
  elmNextMs = millis() + 5000;
}

void loop() {
  drainCan();
  elmTask();
  static uint32_t lastTick = 0;
  if (millis() - lastTick > 2000) {
    lastTick = millis();
    digitalWrite(48, !digitalRead(48));
    Serial.printf("tick can=%s rpm=%.0f cool=%u(~%.0fC) eng=%d city=%d li=%s fu=%d cs=%.0f elm=%s obd=%.0f/%.0f/%.0f L%.0f M%.0f T%.0f I%.0f V%.1f DTC%.0f%s frames=%u obd=%u\n",
                  twai_err, liveRpm, liveCoolRaw, liveCoolEst, liveEngOn, liveCity, liveLights, liveFuel, liveSpeedCan,
                  elmState, obdRpm, obdCool, obdSpeed, obdLoad, obdMap, obdThr, obdIat, obdVolt, obdDtc,
                  obdMil ? " MIL" : "", count, obdCount);
  }
  srv.handleClient();
  delay(1);
}
