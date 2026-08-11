#pragma once
/* =========================================================
 * sms_diag.h — SMS diagnostic command channel (queued, non-intrusive)
 *
 * Out-of-band diagnostics for a device that is powered and registered on the
 * network but NOT reachable over MQTT (bad certificate, revoked policy, dead
 * PDP context, AWS outage). Those are exactly the failures telemetry cannot
 * report on, so this path deliberately shares nothing with the MQTT stack.
 *
 * WIRE FORMAT:  P2G#<COMMAND>#
 *
 * SCHEDULING — this module is a passenger, never a driver:
 *
 *   1. DETECTION IS FREE. checkIncoming() already reads the AT bus every loop.
 *      smsNoteUrc() scans that same buffer for "+CMTI:" and queues the message
 *      index. No AT command is issued to discover that mail arrived.
 *
 *   2. WORK IS QUEUED, NOT IMMEDIATE. An arriving command does not preempt
 *      telemetry, GNSS or BMS. The index sits in a queue until the main loop
 *      reports a genuine idle gap (SMS_MIN_SLACK_MS before its next deadline).
 *
 *   3. THE QUEUE HAS A DEADLINE. If no idle gap appears, a queued command is
 *      serviced anyway once it has waited SMS_MAX_DEFER_MS. Worst-case reply
 *      latency is therefore bounded, not best-effort.
 *
 *   4. ONE PER PASS. A burst drains at one command per loop iteration, so ten
 *      messages cannot monopolise the bus.
 *
 * DELIVERY: read from storage by index (AT+CMGR), never from the +CMT body URC.
 * This codebase flushes the modem RX buffer at the head of every AT command, so
 * an unsolicited URC carrying a message body would be destroyed. An index is
 * cheap to recover; a stored message survives until we choose to read it.
 *
 * ORDERING: a message is DELETED BEFORE its command is dispatched. Reversing
 * this bricks the device on P2G#REBOOT# — the reboot happens, the message is
 * still unread, and it reboots again forever.
 *
 * BUFFERS: char throughout, and static rather than stack. loop() already
 * carries ~2.9 KB of frame (jsonPayload[2560]) against an 8 KB task stack.
 * ========================================================= */

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "config.h"

// ---- externs supplied by main.cpp ----
extern HardwareSerial SerialAT;
extern bool       mqttConnected;
extern BMSData    bmsData;
extern GPSData    gpsData;
extern DeviceInfo deviceInfo;
extern uint32_t   currentBaud;
extern bool       baudRateLocked;
extern bool       bmeAvailable;
extern float      lastBmeTempC;      // last BME680 temperature actually read
extern uint16_t   gnssInView;        // cached from the last GSV sweep
extern uint16_t   gnssWithSignal;
extern uint16_t   gnssBestSnr;

/* SMS init is DEFERRED, not run from setup().
 *
 * The modem brings its SMS subsystem up asynchronously and announces it with
 * "+QIND: SMS DONE". Configuring SMS before that URC lands is unreliable, and
 * AT+CMGD=1,4 (delete all) keeps the modem's AT processor busy for longer than
 * any sane timeout — so the MQTT bring-up that followed it in setup() was
 * firing QMTOPEN into a modem that was still working, producing
 * "+QMTOPEN: 0,1" and a peer-reset MQTT CONNECT. */
#ifndef SMS_INIT_DELAY_MS
#define SMS_INIT_DELAY_MS 45000UL
#endif

// Idle gap required before starting SMS work voluntarily.
#ifndef SMS_MIN_SLACK_MS
#define SMS_MIN_SLACK_MS 3000UL
#endif

// ...but never make a queued command wait longer than this, idle gap or not.
#ifndef SMS_MAX_DEFER_MS
#define SMS_MAX_DEFER_MS 60000UL
#endif

/* Safety sweep for a "+CMTI:" that was missed. NOT the primary path — slow
 * while MQTT is up (real work to avoid disturbing), faster while it is down
 * (nothing to disturb, and SMS is then the only way in). */
/* 30 s, not 10 min. A "+CMTI:" can still be lost despite the capture points in
 * sendATCommand(), and the sweep is what bounds the worst case. It is cheap:
 * AT+CMGL on empty storage returns a bare OK in ~50 ms, less than the GPS poll
 * this loop already does every 10 s. Detection therefore costs at most 30 s,
 * which keeps the end-to-end reply inside the one-minute target even when the
 * URC never arrives. */
#ifndef SMS_SWEEP_ONLINE_MS
#define SMS_SWEEP_ONLINE_MS  30000UL
#endif
#ifndef SMS_SWEEP_OFFLINE_MS
#define SMS_SWEEP_OFFLINE_MS 60000UL    // 1 min
#endif

/* Sender authorisation is handled UPSTREAM, by the Airtel M2M SIM: only
 * whitelisted MSISDNs can deliver SMS to this SIM at all, so an unauthorised
 * P2G#REBOOT# never reaches the modem. That is deliberately better than a
 * firmware check — it is enforced before the air interface, and the allowlist
 * is edited in the operator portal without reflashing the fleet.
 *
 * No number is hardcoded anywhere: replies are addressed to whichever sender
 * the message came from.
 *
 * The optional define below is defence-in-depth for a device fitted with an
 * ordinary consumer SIM, where no network-side filtering exists. Suffix match,
 * so +91 / 0 / bare forms of the same MSISDN all pass. */
// #define SMS_ALLOWED_SENDER "9876543210"

#define SMS_REPLY_MAX 160            // one GSM-7 segment; longer replies split
#define SMS_QUEUE_LEN 8

struct SmsQueued { uint8_t index; unsigned long arrivedMs; };

static SmsQueued     _smsQ[SMS_QUEUE_LEN];
static uint8_t       _smsQHead      = 0;
static uint8_t       _smsQCount     = 0;
static unsigned long _smsLastSweep  = 0;
static bool          _smsReady      = false;

// ======================= Queue =======================

static void _smsEnqueue(int index) {
  // Index 0 is VALID. This modem's ME storage is zero-based -- the first
  // message to arrive after a clear sits at 0, and rejecting it stranded that
  // message permanently: every sweep re-found it, every enqueue threw it away.
  if (index < 0 || index > 255) return;

  // Ignore an index already queued: the same message can be announced by both
  // a "+CMTI:" URC and the safety sweep.
  for (uint8_t i = 0; i < _smsQCount; i++) {
    if (_smsQ[(_smsQHead + i) % SMS_QUEUE_LEN].index == (uint8_t)index) return;
  }

  if (_smsQCount >= SMS_QUEUE_LEN) {
    // Not a loss: the message stays in modem storage and the safety sweep will
    // re-queue it once the backlog clears.
    Serial.println("[SMS] Queue full — deferring to the next sweep.");
    return;
  }

  uint8_t slot = (uint8_t)((_smsQHead + _smsQCount) % SMS_QUEUE_LEN);
  _smsQ[slot].index     = (uint8_t)index;
  _smsQ[slot].arrivedMs = millis();
  _smsQCount++;
  Serial.printf("[SMS] Queued message %d (%u waiting).\n", index, _smsQCount);
}

static bool _smsDequeue(SmsQueued* out) {
  if (_smsQCount == 0) return false;
  *out = _smsQ[_smsQHead];
  _smsQHead = (uint8_t)((_smsQHead + 1) % SMS_QUEUE_LEN);
  _smsQCount--;
  return true;
}

// ======================= Low-level AT helper =======================

// Bounded, char-based. Keeps the TAIL on overflow so the terminating OK/ERROR
// is never the part that gets discarded.
static void _smsAT(const char* cmd, char* out, size_t cap, uint32_t timeoutMs) {
  static char sink[64];
  char*  buf  = (out && cap) ? out : sink;
  size_t size = (out && cap) ? cap : sizeof(sink);

  memset(buf, 0, size);
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);

  unsigned long start = millis();
  size_t idx = 0;
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      if (idx >= size - 1) {
        size_t shift = size / 2;
        memmove(buf, buf + shift, size - shift);
        idx -= shift;
        memset(buf + idx, 0, size - idx);
      }
      buf[idx++] = (char)SerialAT.read();
      buf[idx]   = '\0';
    }
    if (strstr(buf, "OK\r\n") || strstr(buf, "ERROR")) {
      delay(30);
      while (SerialAT.available() && idx < size - 1) {
        buf[idx++] = (char)SerialAT.read();
        buf[idx]   = '\0';
      }
      return;
    }
    delay(1);
  }
}

// ======================= Parsing =======================

/* Extract the command from "P2G#<COMMAND>#".
 *
 * Reads only until the closing '#', and never past `cap`. A message with no
 * closing '#' is REJECTED rather than truncated-and-executed: "P2G#REBOOT"
 * arriving split across a concatenated SMS must not fire a reboot. */
static bool _smsExtractCommand(const char* raw, char* out, size_t cap) {
  const char* p = strstr(raw, "P2G#");
  if (!p) return false;
  p += 4;

  size_t n = 0;
  while (*p && *p != '#' && *p != '\r' && *p != '\n' && n < cap - 1) {
    out[n++] = (char)toupper((unsigned char)*p);
    p++;
  }
  out[n] = '\0';
  return (*p == '#') && n > 0;     // closing '#' mandatory
}

// Copy the n-th (1-based) double-quoted field from one line.
static bool _smsQuotedField(const char* line, int n, char* out, size_t cap) {
  const char* p = line;
  int found = 0;
  while (*p && *p != '\n') {
    if (*p == '"') {
      const char* start = ++p;
      while (*p && *p != '"' && *p != '\n') p++;
      if (*p != '"') return false;
      if (++found == n) {
        size_t len = (size_t)(p - start);
        if (len >= cap) len = cap - 1;
        memcpy(out, start, len);
        out[len] = '\0';
        return true;
      }
      p++;
    } else {
      p++;
    }
  }
  return false;
}

// ======================= Reply =======================

static bool _smsSend(const char* number, const char* text) {
  static char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);

  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);

  // Wait for the '>' prompt before writing the body.
  unsigned long t0 = millis();
  bool prompt = false;
  while (millis() - t0 < 5000 && !prompt) {
    if (SerialAT.available()) { if (SerialAT.read() == '>') prompt = true; }
    else delay(1);
  }
  if (!prompt) {
    Serial.println("[SMS] No '>' prompt — send aborted.");
    return false;
  }

  SerialAT.print(text);
  SerialAT.write(26);            // Ctrl+Z

  static char resp[96];
  memset(resp, 0, sizeof(resp));
  size_t idx = 0;
  t0 = millis();
  // Bounded so one unlucky reply cannot stall telemetry for a quarter minute.
  // A network that cannot accept an SMS in 10 s will not do better at 15.
  while (millis() - t0 < 10000) {
    while (SerialAT.available() && idx < sizeof(resp) - 1) {
      resp[idx++] = (char)SerialAT.read();
      resp[idx]   = '\0';
    }
    if (strstr(resp, "+CMGS:") || strstr(resp, "ERROR")) break;
    delay(1);
  }
  bool ok = strstr(resp, "+CMGS:") != nullptr;
  Serial.printf("[SMS] Reply to %s: %s\n", number, ok ? "sent" : "FAILED");
  return ok;
}

// ======================= Command helpers =======================

// Mirrors the bms.valid rule used in the telemetry payload: values are only
// trustworthy if a CAN frame landed within three read intervals.
static bool _smsCanValid() {
  if (!baudRateLocked || bmsData.lastUpdate == 0) return false;
  unsigned long age = (millis() - bmsData.lastUpdate) / 1000UL;
  return age <= (bmsReadInterval / 1000UL) * 3;
}

// ======================= Command dictionary =======================

static void _smsDispatch(const char* cmd, const char* sender) {
  static char reply[SMS_REPLY_MAX];
  reply[0] = '\0';

  // 1. Server IP and port
  if (strcmp(cmd, "PIP,POP") == 0) {
    snprintf(reply, sizeof(reply), "IP:%s POP:%d", AWS_IOT_ENDPOINT, mqtt_port);

  // 2. System health roll-up
  } else if (strcmp(cmd, "TEST") == 0) {
    char errs[64] = "";
    if (!_smsCanValid()) strncat(errs, "E_CAN, ", sizeof(errs) - strlen(errs) - 1);
    // "reads 0" covers both an absent sensor and a present-but-dead one.
    if (!bmeAvailable || lastBmeTempC == 0.0f)
      strncat(errs, "E_BME, ", sizeof(errs) - strlen(errs) - 1);

    size_t e = strlen(errs);
    if (e >= 2) errs[e - 2] = '\0';        // drop the trailing ", "
    snprintf(reply, sizeof(reply), "TEST: %s", errs[0] ? errs : "OK");

  // 3. GNSS state
  } else if (strcmp(cmd, "GNSS") == 0) {
    snprintf(reply, sizeof(reply),
             "GNSS Sats:%u/%u Fix:%s SNR:%u HDOP:%.2f",
             (unsigned)gnssWithSignal, (unsigned)gnssInView,
             gpsData.gpsFixed ? (gpsData.fix >= 3 ? "3D" : "2D") : "NOFIX",
             (unsigned)gnssBestSnr, gpsData.hdop);

  // 4. Firmware version
  } else if (strcmp(cmd, "FW") == 0) {
    snprintf(reply, sizeof(reply), "FW:%s", FW_VERSION);

  // 5. APN
  } else if (strcmp(cmd, "APN") == 0) {
    snprintf(reply, sizeof(reply), "APN:%s", apn.c_str());

  // 6. CAN validity as a bare flag
  } else if (strcmp(cmd, "CAN") == 0) {
    snprintf(reply, sizeof(reply), "CAN:%d", _smsCanValid() ? 1 : 0);

  // 7. Raw CAN registers.
  //    Reconstructed from the scaled values, since readBMS() keeps only the
  //    decoded floats. Exact, because both sources are integer/100.
  } else if (strcmp(cmd, "CANRAW") == 0) {
    uint16_t rawV = (uint16_t)lroundf(bmsData.packVoltage * 100.0f);
    int16_t  rawI = (int16_t) lroundf(bmsData.packCurrent * 100.0f);
    uint16_t rawS = (uint16_t)bmsData.soc;
    snprintf(reply, sizeof(reply), "CANRAW V:0x%04X A:0x%04X SOC:0x%04X",
             rawV, (uint16_t)rawI, rawS);

  // 8. Locked CAN baud rate
  } else if (strcmp(cmd, "CANBDRT") == 0) {
    snprintf(reply, sizeof(reply), "CANBDRT:%lu",
             baudRateLocked ? (unsigned long)currentBaud : 0UL);

  // 9. IMEI
  } else if (strcmp(cmd, "IMEI") == 0) {
    // DeviceInfo.imei is a fixed char[] here, not a String — this codebase keeps
    // the identity fields allocation-free (see config.h).
    snprintf(reply, sizeof(reply), "IMEI:%s",
             deviceInfo.imei[0] ? deviceInfo.imei : "UNKNOWN");

  // 10. SIM ICCID — queried live; collectDeviceInfo() never populates it.
  } else if (strcmp(cmd, "ICCID") == 0) {
    static char resp[128];
    _smsAT("AT+QCCID", resp, sizeof(resp), 3000);
    char* p = strstr(resp, "+QCCID:");
    char iccid[32] = "UNKNOWN";
    if (p) {
      p += 7;
      while (*p == ' ') p++;
      size_t n = 0;
      while (isdigit((unsigned char)*p) && n < sizeof(iccid) - 1) iccid[n++] = *p++;
      iccid[n] = '\0';
      if (n == 0) strcpy(iccid, "UNKNOWN");
    }
    snprintf(reply, sizeof(reply), "ICCID:%s", iccid);

  // 11. Signal strength
  } else if (strcmp(cmd, "SS") == 0) {
    snprintf(reply, sizeof(reply), "SS:%d dBm", deviceInfo.signal_strength);

  // 12. Reboot. Reply FIRST — after esp_restart() there is no second chance,
  //     and a silent reboot is indistinguishable from an ignored command.
  } else if (strcmp(cmd, "REBOOT") == 0) {
    snprintf(reply, sizeof(reply), "REBOOT: restarting now");
    _smsSend(sender, reply);
    Serial.println("[SMS] REBOOT command — restarting.");
    Serial.flush();
    delay(1000);
    ESP.restart();
    return;

  } else {
    snprintf(reply, sizeof(reply), "ERR: unknown cmd '%s'", cmd);
  }

  _smsSend(sender, reply);
}

// ======================= Servicing one queued message =======================

static void _smsHandleIndex(int index) {
  static char cmgr[384];
  static char cmd[40];
  snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
  _smsAT(cmd, cmgr, sizeof(cmgr), 5000);

  char* h = strstr(cmgr, "+CMGR:");

  // DELETE FIRST, unconditionally — before any dispatch, and even if the read
  // produced nothing parseable. If dispatch reboots, or the modem resets
  // mid-reply, the message must already be gone or it re-executes every boot.
  static char delCmd[32];
  snprintf(delCmd, sizeof(delCmd), "AT+CMGD=%d", index);
  _smsAT(delCmd, nullptr, 0, 5000);

  if (!h) { Serial.printf("[SMS] Message %d unreadable.\n", index); return; }

  // Header: +CMGR: "<stat>","<sender>",,"<timestamp>"
  char sender[24] = {0};
  if (!_smsQuotedField(h, 2, sender, sizeof(sender))) {
    Serial.printf("[SMS] Message %d has no sender field.\n", index);
    return;
  }

  // Body is the line following the header.
  char* nl = strchr(h, '\n');
  char  body[192] = {0};
  if (nl) {
    nl++;
    size_t n = 0;
    while (nl[n] && nl[n] != '\r' && nl[n] != '\n' && n < sizeof(body) - 1) {
      body[n] = nl[n];
      n++;
    }
    body[n] = '\0';
  }
  if (!body[0]) return;

#ifdef SMS_ALLOWED_SENDER
  size_t sl = strlen(sender), al = strlen(SMS_ALLOWED_SENDER);
  if (sl < al || strcmp(sender + (sl - al), SMS_ALLOWED_SENDER) != 0) {
    Serial.printf("[SMS] Ignored command from unauthorised sender %s\n", sender);
    return;
  }
#endif

  char parsed[32];
  if (!_smsExtractCommand(body, parsed, sizeof(parsed))) {
    Serial.printf("[SMS] Malformed message from %s: %s\n", sender, body);
    return;
  }

  Serial.printf("[SMS] %s -> %s\n", sender, parsed);
  _smsDispatch(parsed, sender);
}

// Runs once, from the loop, SMS_INIT_DELAY_MS after boot.
static void _smsInit() {
  static char resp[192];

  _smsAT("AT+CMGF=1", resp, sizeof(resp), 2000);        // text mode
  if (!strstr(resp, "OK")) Serial.println("[SMS] WARN: AT+CMGF=1 rejected.");

  _smsAT("AT+CSCS=\"GSM\"", nullptr, 0, 2000);

  /* Do NOT force storage. Forcing "ME" on a module that only receives into
   * "SM" files arriving messages somewhere AT+CMGL is not looking — they land,
   * the network confirms delivery, and the device never sees them. Accept the
   * modem's own default and report it, so read and receive always agree. */
  _smsAT("AT+CPMS?", resp, sizeof(resp), 5000);
  {
    char* p = strstr(resp, "+CPMS:");
    if (p) { char* e = strchr(p, '\r'); if (e) *e = '\0'; Serial.printf("[SMS] %s\n", p); }
    else   { Serial.println("[SMS] WARN: AT+CPMS? gave no answer."); }
  }

  _smsAT("AT+CNMI=2,1,0,0,0", resp, sizeof(resp), 2000);  // store + notify index
  if (!strstr(resp, "OK"))
    Serial.println("[SMS] WARN: AT+CNMI rejected — relying on the 30s sweep.");

  /* NO boot wipe.
   *
   * There used to be an AT+CMGD=1,4 here, to stop a stale P2G#REBOOT# firing
   * on every boot. That reasoning was wrong: _smsHandleIndex() deletes a
   * message BEFORE dispatching it, so a stale reboot command costs exactly one
   * restart and is then gone — there is no loop to prevent. All the wipe
   * actually did was destroy every command that arrived in the 45 s before
   * this ran, including ones sent while the device was powered down.
   *
   * Anything already in storage is picked up by the first sweep instead. */

  _smsReady = true;
  // Backdate so the first sweep runs on the next loop pass rather than 30 s
  // later: anything queued while the device was down should be answered now.
  _smsLastSweep = millis() - SMS_SWEEP_ONLINE_MS;
  Serial.println("[SMS] Diagnostic command channel ready (P2G#<CMD>#).");
}

// ======================= Public API =======================

/* Feed this whatever checkIncoming() already read off the bus. Free: a strstr
 * over a buffer that was going to be read anyway, no AT command issued. */
inline void smsNoteUrc(const char* buf) {
  if (!buf || !_smsReady) return;
  const char* p = buf;
  while ((p = strstr(p, "+CMTI:")) != nullptr) {
    const char* c = strchr(p, ',');
    if (!c) break;
    _smsEnqueue(atoi(c + 1));
    p += 6;
  }
}

/* Call from loop(), outside any OTA window — otaRun() owns the AT bus.
 *
 * `slackMs` is how long until the main loop's next scheduled task (telemetry
 * publish, GNSS read, BMS read). Pass 0 if something is due right now. Nothing
 * touches the bus unless there is real work AND either room to do it or a
 * command that has waited too long. */
inline void smsPoll(unsigned long slackMs) {
  if (!_smsReady) {
    if (millis() < SMS_INIT_DELAY_MS) return;
    _smsInit();
    return;                    // let the modem settle before the first poll
  }

  if (_smsQCount > 0) {
    unsigned long waited = millis() - _smsQ[_smsQHead].arrivedMs;

    /* SMS NEVER PREEMPTS. Priority order is MQTT > GNSS > CAN/sensors > SMS,
     * so an ageing command lowers the bar for what counts as a usable gap --
     * it does not remove it. Waiting longer must never translate into a
     * delayed telemetry publish or a stale CAN frame.
     *
     * This still meets the one-minute target comfortably: the tightest cycle
     * is the 5 s BMS read, so a >=1.5 s gap exists for most of every cycle and
     * is tested on every loop iteration. */
    unsigned long need = (waited >= SMS_MAX_DEFER_MS) ? (SMS_MIN_SLACK_MS / 2)
                                                      : SMS_MIN_SLACK_MS;
    if (slackMs < need) {
      // Genuinely starved: report it rather than barging in. A loop this busy
      // is itself the fault worth knowing about.
      if (waited >= SMS_MAX_DEFER_MS * 2 && (waited / 1000UL) % 30 == 0)
        Serial.printf("[SMS] %d still queued after %lus — loop has no idle gap.\n",
                      _smsQ[_smsQHead].index, waited / 1000UL);
      return;
    }

    SmsQueued job;
    if (_smsDequeue(&job)) {
      _smsHandleIndex(job.index);   // one per pass — a burst cannot hog the bus
    }
    return;
  }

  // Nothing queued. Sweep storage occasionally in case a "+CMTI:" was lost.
  unsigned long sweepEvery = mqttConnected ? SMS_SWEEP_ONLINE_MS
                                           : SMS_SWEEP_OFFLINE_MS;
  if (millis() - _smsLastSweep < sweepEvery) return;
  if (slackMs < SMS_MIN_SLACK_MS) return;      // the sweep is never urgent
  _smsLastSweep = millis();

  /* "ALL", not "REC UNREAD". Status strings vary between firmwares, and a
   * message already flagged read — by the modem, or by a previous partial
   * read — is invisible to the UNREAD filter and would never be answered.
   * Safe to sweep everything: each message is deleted once handled, and boot
   * clears storage, so this list is normally empty. */
  static char list[512];
  _smsAT("AT+CMGL=\"ALL\"", list, sizeof(list), 5000);

  // One-shot raw dump: proves whether messages are reaching the storage we
  // read from, which no amount of reasoning about CPMS can settle.
  static bool dumpedRawList = false;
  if (!dumpedRawList) {
    dumpedRawList = true;
    Serial.printf("[SMS] raw CMGL >>> %s\n", list[0] ? list : "(empty)");
  }

  // Queue what it found; the normal path drains it one per pass.
  const char* p = list;
  while ((p = strstr(p, "+CMGL:")) != nullptr) {
    _smsEnqueue(atoi(p + 6));
    p += 6;
  }
}
