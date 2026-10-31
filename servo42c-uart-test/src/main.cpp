// ---------------------------------------------------------------------------
// 0163 — MKS SERVO42C UART bench self-test, instrumented (classic ESP32 devkit)
//
// Driver: MKS SERVO42C closed-loop stepper (Makerbase), firmware V1.1.x.
// Protocol reference: firmware/servo42c-uart-test/PROTOCOL.md (cited sources).
//
//   UART2 @ 9600 8N1, address 0xE0 (UartAddr default; baud set to 9600 to
//           match the driver's OLED UartBaud — see 0162)
//   frame  : <addr> <func> [data...] <chk>      chk = low byte of the byte sum
//   reply  : <addr> [data...] <chk>             high byte first, 0x01 = ok
//
// 0163 scope: 0162 measured 800 commanded pulses producing ~-25 deg of shaft
// angle instead of +90 (the sign was the driver's CCW Dir setting; Stephen has
// since set Dir=CW). The magnitude is the open question: 800 pulses at MStep 16
// must be 90 deg (MKS doc for 0xFD: "if xx xx xx xx = 00 00 0C 80, the motor
// (1.8 deg, subdivision 16) will rotate one circle"), yet the 0x36 read moved
// only ~4285 counts (23.5 deg at the documented 65536 counts/rev). Two
// explanations give the identical counts-per-pulse (5.12) and cannot be told
// apart from one 800-pulse move:
//     (A) MStep is really 64, not 16 -> 12800 pulses/rev, scale 65536/rev
//     (B) the 0x36 angle scale is 16384 counts/rev on this firmware
//         -> MStep 16 is right and the move really was ~94 deg
//
// This build separates them by measurement, not assumption: every move is
// instrumented with the driver's OWN pulse counter (0x33) and encoder (0x30)
// as well as the shaft angle (0x36), and a one-revolution probe
// (STEP_SUBDIVISION * MOTOR_FULL_STEPS pulses) runs after the graded steps:
//   - probe ends back on the start angle -> those pulses ARE one revolution
//     (MStep 16), so the angle scale is what is wrong -> (B)
//   - probe ends ~16384 counts away      -> the pulse count is a quarter turn
//     (MStep ~64), so the documented 65536/rev scale holds -> (A)
//
// Bench self-test on boot (PASS/FAIL line per step, USB serial @115200):
//   1 framing/protocol (checksum vs plain, V1.1 vs V1.0 firmware)
//   2 shaft angle read (0x36)
//   3 following error read (0x39)
//   4 motor enable (0xF3) + En status verify (0x3A)
//   5 commanded +90 deg move accepted (0xFD)
//   6 measured move is +90 deg (+/- 5 deg, SIGN GRADED)
//   7 commanded return move accepted (0xFD, reverse)
//   8 measured return lands back on the start angle (+/- 5 deg)
// ---------------------------------------------------------------------------

#include <Arduino.h>

// ----------------------------- configuration -------------------------------
static const char    *BUILD_TAG   = "0163-C (direction corrected, verified)";
static const uint32_t DRIVER_BAUD = 9600;    // OLED UartBaud (0162: driver set to 9600)
static const uint8_t  ADDR        = 0xE0;    // UartAddr default (MKS manual)
static const int      PIN_RX2     = 16;      // ESP32 UART2 RX  <- driver TX
static const int      PIN_TX2     = 17;      // ESP32 UART2 TX  -> driver RX

// Pulses per revolution = STEP_SUBDIVISION * motor full steps.
// The protocol has no "read MStep" command, so this MUST match the driver.
static const uint16_t STEP_SUBDIVISION = 16;   // MStep on the OLED (0163-A measures it; 0163-B sets the measured value)
static const uint16_t MOTOR_FULL_STEPS = 200;  // 1.8 deg motor (MotType default)

// 0x36 shaft-angle scale. Documented as 65536 counts/rev ("the motor rotates
// one circle, the corresponding angle value range is 0~65535", 0x4000 = 90 deg).
// 0163-A measures it; 0163-B sets the measured value.
static const uint32_t ANGLE_COUNTS_PER_REV = 65536UL;

static const uint8_t  SPEED_GEAR      = 16;    // 0xF6/0xFD speed gear (low 7 bits)
// 0xFD direction bit (bit 7 of the speed byte). Measured on this unit: with the
// driver's OLED Dir set to CW, bit7=0 moves the shaft in the NEGATIVE 0x36
// angle direction (0163-A: 800 pulses -> -90.12 deg). Setting Dir on the drive
// does not flip it, because the driver remaps motor phases and encoder together
// — the reported sign is unchanged. So "forward = increasing angle" is bit7=1.
static const uint8_t  DIR_FORWARD_BIT = 0x80;
static const uint32_t SETTLE_TIMEOUT_MS = 8000;
static const float    ANGLE_TOL_DEG   = 5.0f;

// ------------------------------ protocol -----------------------------------
static const uint8_t CMD_READ_ENCODER = 0x30;  // uint16 encoder value
static const uint8_t CMD_READ_PULSES  = 0x33;  // int32 received pulses
static const uint8_t CMD_READ_ANGLE   = 0x36;  // int32 shaft angle, 0..65535/rev
static const uint8_t CMD_READ_ERROR   = 0x39;  // int16 following error
static const uint8_t CMD_READ_EN      = 0x3A;  // uint8 En pin status 01/02/00
static const uint8_t CMD_READ_SHAFT   = 0x3E;  // uint8 blocked/unblocked
static const uint8_t CMD_ENABLE       = 0xF3;  // 01 enable, 00 disable
static const uint8_t CMD_STOP         = 0xF7;  // stop motion
static const uint8_t CMD_MOVE_PULSES  = 0xFD;  // speed/dir + int32 pulse count

enum Protocol { PROTO_UNKNOWN = 0, PROTO_CHECKSUM, PROTO_PLAIN };
static Protocol proto = PROTO_UNKNOWN;

// ------------------------------ helpers ------------------------------------
static void uartFlush() {
  while (Serial2.available()) (void)Serial2.read();
}

// Read whatever the driver sends, stopping after a 15 ms idle gap.
// 0162: at 9600 the driver echoes the command frame back before its reply, and
// a reply sometimes carries 1-2 bytes of line-turnaround noise in front of it,
// so collect raw bytes here and let extractFrame() find the reply inside them.
static int readReply(uint8_t *buf, size_t cap, uint32_t timeoutMs) {
  size_t n = 0;
  uint32_t t0 = millis();
  uint32_t last = 0;
  while (millis() - t0 < timeoutMs) {
    if (Serial2.available()) {
      uint8_t b = (uint8_t)Serial2.read();
      if (n < cap) buf[n++] = b;
      last = millis();
    } else if (n > 0 && millis() - last > 15) {
      break;                       // frame finished (no trailing bytes)
    }
    delay(1);
  }
  return (int)n;
}

static uint8_t checksum(const uint8_t *buf, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; i++) s += buf[i];
  return s;
}

static void printFrame(const char *tag, const uint8_t *buf, size_t n) {
  Serial.print("    ");
  Serial.print(tag);
  for (size_t i = 0; i < n; i++) {
    Serial.print(' ');
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
  }
  Serial.println();
}

// Send one frame and collect the raw reply bytes.
static int xfer(uint8_t func, const uint8_t *data, uint8_t nData,
                uint8_t *raw, size_t cap, bool withChk) {
  uint8_t frame[12];
  size_t k = 0;
  frame[k++] = ADDR;
  frame[k++] = func;
  for (uint8_t i = 0; i < nData; i++) frame[k++] = data[i];
  if (withChk) {
    const uint8_t chk = checksum(frame, k);   // sum BEFORE the check byte
    frame[k++] = chk;
  }

  uartFlush();
  Serial2.write(frame, k);
  Serial2.flush();
  printFrame("TX:", frame, k);

  int n = readReply(raw, cap, 300);
  if (n > 0) printFrame("RX:", raw, (size_t)n);
  else       Serial.println("    RX: (no reply)");
  return n;
}

// Extract the driver's reply from the raw capture. The reply is the LAST valid
// frame in the buffer (anything before it is the command echo and/or noise),
// so scan backwards: addr + nData payload + (optional) checksum.
static bool extractFrame(const uint8_t *raw, int n, uint8_t nData, bool withChk,
                         uint8_t *out, int *prefixLen) {
  const int want = 1 + nData + (withChk ? 1 : 0);
  for (int i = n - want; i >= 0; i--) {
    if (raw[i] != ADDR) continue;
    const uint8_t *f = raw + i;
    if (withChk && checksum(f, (size_t)want - 1) != f[want - 1]) continue;
    memcpy(out, f, (size_t)want);
    if (prefixLen) *prefixLen = i;
    return true;
  }
  return false;
}

// Query returning nData payload bytes in out[]. Retried: the 0162 runs lost
// steps to occasional damaged replies, so a single bad read must not be
// reported as a driver failure. Every attempt is printed.
static bool query(uint8_t func, uint8_t nData, uint8_t *out, uint8_t tries = 3) {
  uint8_t raw[48], frame[16];
  bool withChk = (proto == PROTO_CHECKSUM);
  for (uint8_t t = 0; t < tries; t++) {
    int n = xfer(func, nullptr, 0, raw, sizeof(raw), withChk);
    if (extractFrame(raw, n, nData, withChk, frame, nullptr)) {
      memcpy(out, frame + 1, nData);
      return true;
    }
    delay(20);
  }
  return false;
}

// Command taking one payload byte; verifies result==0x01.
static bool command1(uint8_t func, uint8_t arg, uint8_t tries = 3) {
  uint8_t raw[48], frame[16];
  bool withChk = (proto == PROTO_CHECKSUM);
  for (uint8_t t = 0; t < tries; t++) {
    int n = xfer(func, &arg, 1, raw, sizeof(raw), withChk);
    if (extractFrame(raw, n, 1, withChk, frame, nullptr) && frame[1] == 0x01)
      return true;
    delay(20);
  }
  return false;
}

static int32_t be32(const uint8_t *p) {
  return ((int32_t)p[0] << 24) | ((int32_t)p[1] << 16) |
         ((int32_t)p[2] << 8) | (int32_t)p[3];
}

static bool readAngleRaw(int32_t *raw) {
  uint8_t d[4];
  if (!query(CMD_READ_ANGLE, 4, d)) return false;
  *raw = be32(d) & 0xFFFF;   // one revolution = 0..65535 (modular position)
  return true;
}

static bool readEncoder(uint16_t *v) {
  uint8_t d[2];
  if (!query(CMD_READ_ENCODER, 2, d)) return false;
  *v = (uint16_t)((d[0] << 8) | d[1]);
  return true;
}

static bool readPulses(int32_t *v) {
  uint8_t d[4];
  if (!query(CMD_READ_PULSES, 4, d)) return false;
  *v = be32(d);
  return true;
}

static float rawToDeg(int32_t raw) {
  return raw * 360.0f / (float)ANGLE_COUNTS_PER_REV;
}

// Shortest signed difference between two 16-bit angle positions.
static int32_t angleDelta(int32_t from, int32_t to) {
  int32_t d = (to - from + 32768) & 0xFFFF;
  if (d < 0) d += 65536;
  return d - 32768;
}

// ------------------------ protocol detection -------------------------------
// V1.1 firmware answers with a trailing 8-bit sum; V1.0 has no check byte.
static Protocol detectProtocol() {
  uint8_t raw[48], frame[16];

  // Try the checksummed frame first: a V1.1 driver answers 6 bytes for 0x36.
  int n = xfer(CMD_READ_ANGLE, nullptr, 0, raw, sizeof(raw), true);
  if (extractFrame(raw, n, 4, true, frame, nullptr)) return PROTO_CHECKSUM;

  // Plain frame: a V1.0 driver answers 5 bytes for 0x36.
  n = xfer(CMD_READ_ANGLE, nullptr, 0, raw, sizeof(raw), false);
  if (extractFrame(raw, n, 4, false, frame, nullptr)) return PROTO_PLAIN;

  return PROTO_UNKNOWN;
}

// --------------------------- move + settle ---------------------------------
static bool movePulses(bool forward, uint32_t pulses) {
  uint8_t arg = (uint8_t)((forward ? DIR_FORWARD_BIT : 0x00) | (SPEED_GEAR & 0x7F));
  // The pulse field grew from int16 (V1.0) to int32 (V1.1).
  uint8_t data[5];
  uint8_t nData;
  if (proto == PROTO_CHECKSUM) {          // V1.1: dir/speed + int32 pulses
    if (pulses > 0x7FFFFFFFUL) pulses = 0x7FFFFFFFUL;
    data[0] = arg;
    data[1] = (uint8_t)(pulses >> 24);
    data[2] = (uint8_t)(pulses >> 16);
    data[3] = (uint8_t)(pulses >> 8);
    data[4] = (uint8_t)(pulses);
    nData = 5;
  } else {                                // V1.0: dir/speed + int16 pulses
    if (pulses > 0xFFFFUL) pulses = 0xFFFFUL;
    data[0] = arg;
    data[1] = (uint8_t)(pulses >> 8);
    data[2] = (uint8_t)(pulses);
    nData = 3;
  }
  uint8_t raw[48], frame[16];
  bool withChk = (proto == PROTO_CHECKSUM);
  for (uint8_t t = 0; t < 3; t++) {
    int n = xfer(CMD_MOVE_PULSES, data, nData, raw, sizeof(raw), withChk);
    if (extractFrame(raw, n, 1, withChk, frame, nullptr) && frame[1] == 0x01)
      return true;
    delay(20);
  }
  return false;
}

// Poll the shaft angle until it stops changing (or the timeout expires).
// 0163: ignore the first 250 ms so a slow ramp cannot be read as "settled",
// then require the angle to be stable for 5 consecutive 100 ms samples.
static bool waitForSettle(int32_t *finalRaw) {
  int32_t prev = 0, cur = 0;
  uint8_t stable = 0;
  uint32_t t0 = millis();

  delay(250);
  if (!readAngleRaw(&prev)) return false;
  while (millis() - t0 < SETTLE_TIMEOUT_MS) {
    delay(100);
    if (!readAngleRaw(&cur)) continue;
    if (labs((long)angleDelta(prev, cur)) < 30) {   // < 0.17 deg per 100 ms @ 65536
      if (++stable >= 5) { *finalRaw = cur; return true; }
    } else {
      stable = 0;
    }
    prev = cur;
  }
  *finalRaw = cur;
  return false;
}

// ---------------------------- motion record --------------------------------
// One instrumented move: three independent views of the same motion.
struct Motion {
  bool     ack;
  bool     settled;
  int32_t  angleFrom, angleTo;
  int32_t  pulseFrom, pulseTo;
  uint16_t encFrom, encTo;
  bool     havePulses, haveEnc;
  float    deg() const { return angleDelta(angleFrom, angleTo) * 360.0f /
                                (float)ANGLE_COUNTS_PER_REV; }
  int32_t  angleCnt() const { return angleDelta(angleFrom, angleTo); }
  int32_t  pulseDelta() const { return pulseTo - pulseFrom; }
  int16_t  encDelta() const { return (int16_t)(uint16_t)(encTo - encFrom); }
};

static Motion doMove(bool forward, uint32_t pulses) {
  Motion m{};
  m.havePulses = readPulses(&m.pulseFrom);
  m.haveEnc    = readEncoder(&m.encFrom);
  readAngleRaw(&m.angleFrom);

  m.ack     = movePulses(forward, pulses);
  m.settled = waitForSettle(&m.angleTo);

  m.havePulses = m.havePulses && readPulses(&m.pulseTo);
  m.haveEnc    = m.haveEnc    && readEncoder(&m.encTo);
  return m;
}

static void printMotion(const Motion &m) {
  Serial.printf("    angle  %ld -> %ld  (delta %+ld cnt = %+.2f deg @ %lu/rev)\n",
                (long)m.angleFrom, (long)m.angleTo, (long)m.angleCnt(),
                m.deg(), (unsigned long)ANGLE_COUNTS_PER_REV);
  if (m.havePulses)
    Serial.printf("    0x33   %ld -> %ld  (delta %+ld pulses)\n",
                  (long)m.pulseFrom, (long)m.pulseTo, (long)m.pulseDelta());
  else
    Serial.println("    0x33   pulse-counter read failed on one side");
  if (m.haveEnc)
    Serial.printf("    0x30   %u -> %u  (delta %+d counts)\n",
                  m.encFrom, m.encTo, (int)m.encDelta());
  else
    Serial.println("    0x30   encoder read failed on one side");
}

// ------------------------------- test --------------------------------------
static int failures = 0;

static void step(int n, const char *name, bool ok, const char *fmt, ...) {
  char detail[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  Serial.printf("STEP %d: %-38s %s  (%s)\n", n, name, ok ? "PASS" : "FAIL", detail);
  if (!ok) failures++;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.printf("=== MKS SERVO42C UART bench self-test (build %s) ===\n", BUILD_TAG);
  Serial.printf("UART2 9600 8N1  addr 0x%02X  RX=GPIO%d TX=GPIO%d\n",
                ADDR, PIN_RX2, PIN_TX2);
  Serial.printf("Assume MStep=%u, motor %u full steps -> %lu pulses/rev, "
                "90 deg = %lu pulses; angle scale %lu counts/rev\n",
                STEP_SUBDIVISION, MOTOR_FULL_STEPS,
                (unsigned long)STEP_SUBDIVISION * MOTOR_FULL_STEPS,
                (unsigned long)(STEP_SUBDIVISION * MOTOR_FULL_STEPS / 4),
                (unsigned long)ANGLE_COUNTS_PER_REV);

  Serial2.begin(DRIVER_BAUD, SERIAL_8N1, PIN_RX2, PIN_TX2);
  delay(50);
  uartFlush();

  // ---- step 1: framing / protocol generation
  Serial.println("-- probing driver framing (0x36 both generations) --");
  proto = detectProtocol();
  step(1, "uart framing / protocol", proto != PROTO_UNKNOWN,
       proto == PROTO_CHECKSUM ? "V1.1 firmware, checksummed frames"
       : proto == PROTO_PLAIN  ? "V1.0 firmware, no checksum byte"
                               : "no valid reply on either framing");
  if (proto == PROTO_UNKNOWN) {
    Serial.println("HINTS: check TX/RX swap, common GND, UartAddr=0xE0, "
                   "UartBaud=9600, CtrMode=CR_UART on the OLED.");
    return;   // nothing else is meaningful without a link
  }

  // ---- step 2: shaft angle
  int32_t startRaw = 0;
  bool ok = readAngleRaw(&startRaw);
  step(2, "read shaft angle (0x36)", ok,
       ok ? "raw %ld = %.2f deg" : "no valid reply",
       (long)startRaw, rawToDeg(startRaw));
  if (!ok) { Serial.println("ABORT: no angle feedback, later steps meaningless."); return; }

  // ---- step 3: following error
  uint8_t eb[2];
  bool errOk = query(CMD_READ_ERROR, 2, eb);
  int16_t ferr = errOk ? (int16_t)((eb[0] << 8) | eb[1]) : 0;
  step(3, "read following error (0x39)", errOk,
       errOk ? "raw %d = %.2f deg" : "no valid reply",
       (int)ferr, ferr * 360.0f / 65536.0f);

  // ---- step 4: enable + confirm
  uint8_t enb[1];
  bool enAck = command1(CMD_ENABLE, 0x01);
  bool enRead = query(CMD_READ_EN, 1, enb);
  step(4, "motor enable (0xF3=01, 0x3A)", enAck && enRead && enb[0] == 0x01,
       "ack=%d status=0x%02X", (int)enAck, enRead ? enb[0] : 0xFF);
  delay(200);

  // ---- step 5/6: +90 deg relative move (0xFD; protocol has no absolute move)
  const uint32_t quarterTurn =
      (uint32_t)STEP_SUBDIVISION * MOTOR_FULL_STEPS / 4;
  Serial.printf("-- +90 deg move: %lu pulses fwd @ gear %u --\n",
                (unsigned long)quarterTurn, SPEED_GEAR);
  Motion fwd = doMove(true, quarterTurn);
  printMotion(fwd);
  step(5, "command +90 deg move (0xFD)", fwd.ack,
       fwd.ack ? "%lu pulses fwd @ gear %u" : "driver rejected the frame",
       (unsigned long)quarterTurn, SPEED_GEAR);
  if (!fwd.ack)
    Serial.println("HINT: a rejected 0xFD usually means CtrMode is not "
                   "CR_UART on the driver's OLED menu.");

  const float movedDeg = fwd.deg();
  step(6, "measured move is +90 deg", fwd.settled && fwd.ack &&
       fabsf(movedDeg - 90.0f) <= ANGLE_TOL_DEG,
       "moved %+.2f deg in %s", movedDeg, fwd.settled ? "time" : "TIMEOUT");
  if (fwd.ack && fwd.settled && fabsf(movedDeg - 90.0f) > ANGLE_TOL_DEG)
    Serial.printf("HINT: %+.2f deg measured for %lu pulses at %lu counts/rev -> "
                  "%.2f counts/pulse (documented MStep 16 = 20.48)\n",
                  movedDeg, (unsigned long)quarterTurn,
                  (unsigned long)ANGLE_COUNTS_PER_REV,
                  (double)fwd.angleCnt() / (double)quarterTurn);

  // ---- step 7/8: return to the start angle with a reverse relative move
  Motion back = doMove(false, quarterTurn);
  printMotion(back);
  step(7, "command return move (0xFD rev)", back.ack,
       back.ack ? "%lu pulses rev @ gear %u" : "driver rejected the frame",
       (unsigned long)quarterTurn, SPEED_GEAR);

  const float residDeg = angleDelta(startRaw, back.angleTo) * 360.0f /
                         (float)ANGLE_COUNTS_PER_REV;
  step(8, "returned to start angle", back.settled && back.ack &&
       fabsf(residDeg) <= ANGLE_TOL_DEG,
       "residual %+.2f deg from start", residDeg);

  // ---- INFO: pulse-unit / scale probe (not graded)
  // 0163-A finding: ONE 3200-pulse command (a nominal revolution at MStep 16) is
  // NOT executed in full — the driver acknowledged it, but its own pulse counter
  // (0x33) advanced only 2224, and the angle matched that exactly (20.48
  // counts/pulse). Four consecutive 800-pulse moves stay inside that limit, so
  // the revolution is demonstrated in four steps, each checked against 0x33.
  Serial.println("\n-- INFO: pulse-unit probe, 4 x 800 pulses (nominal 1 revolution) --");
  int32_t probeStartRaw = 0;
  readAngleRaw(&probeStartRaw);
  float   probeDegSum    = 0.0f;
  int32_t probeCntSum    = 0;
  int32_t probePulsesSum = 0;
  bool    probeCountersOk = true;
  for (uint8_t i = 0; i < 4; i++) {
    Motion m = doMove(true, quarterTurn);
    Serial.printf("  probe move %u/4:\n", (unsigned)(i + 1));
    printMotion(m);
    probeDegSum += m.deg();
    probeCntSum += m.angleCnt();
    if (m.havePulses) {
      probePulsesSum += m.pulseDelta();
      probeCountersOk = probeCountersOk &&
                        (m.pulseDelta() == (int32_t)quarterTurn);
    } else {
      probeCountersOk = false;
    }
  }
  int32_t probeEndRaw = 0;
  if (readAngleRaw(&probeEndRaw)) {
    const int32_t totalCnt = angleDelta(probeStartRaw, probeEndRaw);
    Serial.printf("INFO: 4 x %lu pulses -> %+ld cnt (%.2f deg) net; 0x33 "
                  "tracked %+ld pulses (%s)\n",
                  (unsigned long)quarterTurn, (long)totalCnt,
                  totalCnt * 360.0f / (float)ANGLE_COUNTS_PER_REV,
                  (long)probePulsesSum,
                  probeCountersOk ? "exactly as commanded" : "SHORT/NOISY");
    if (probePulsesSum != 0)
      Serial.printf("INFO: counts per pulse = %.3f (documented %.2f at "
                    "65536 cnt/rev, MStep 16)\n",
                    (double)probeCntSum / (double)probePulsesSum,
                    65536.0 / (double)(quarterTurn * 4));
  }
  // put the shaft back where the probe found it
  {
    int32_t undoPulses = 0;
    bool undoOk = true;
    for (uint8_t i = 0; i < 4; i++) {
      Motion u = doMove(false, quarterTurn);
      if (u.havePulses) undoPulses += u.pulseDelta(); else undoOk = false;
      undoOk = undoOk && u.ack && u.settled;
    }
    Serial.printf("INFO: probe undone in 4 reverse moves, 0x33 %+ld pulses (%s)\n",
                  (long)undoPulses, undoOk ? "all settled" : "INCOMPLETE");
  }

  // ---- teardown / diagnostics (informational, not graded)
  uint8_t stopRes[1];
  if (!query(CMD_STOP, 1, stopRes) || stopRes[0] != 0x01)
    Serial.println("INFO: stop (0xF7) did not acknowledge — motor may still be enabled.");
  uint8_t sh[1];
  if (query(CMD_READ_SHAFT, 1, sh))
    Serial.printf("INFO: shaft status 0x%02X (%s)\n", sh[0],
                  sh[0] == 0x01 ? "blocked" : sh[0] == 0x02 ? "unblocked" : "error");

  Serial.println();
  Serial.printf("SELF-TEST: %s (%d of 8 steps failed)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
  Serial.println("=== self-test complete ===");
}

void loop() {
  delay(1000);   // one-shot bench test; nothing runs continuously
}
