/* =========================================================
 * POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.0)
 * Modular, Zero-Allocation, Non-blocking Architecture
 * ========================================================= */

#include <Arduino.h>
#include <QuectelEC200U.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "driver/twai.h"
#include "config.h"
#include "aws_iot_core.h"
#include "device_health.h"
#include "EC200U_AWS_OTA.h"
#include "bms_can_parser.h"
#include "sms_diag.h"

// Arm ESP32 rollback. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the Arduino
// core, but its weak verifyRollbackLater() returns false, so initArduino()
// marks a freshly flashed image valid before it has proven anything. Returning
// true defers that decision to otaConfirmHealthy(), which only runs once the
// new image has reached AWS IoT. Consequence: if a new image is power-cycled
// before it ever connects, the bootloader reverts to the previous bank.
#define OTA_ARM_ROLLBACK 1
#if OTA_ARM_ROLLBACK
bool verifyRollbackLater() { return true; }
#endif

// ============ GLOBALS ============
BMSData bmsData = {0};
GPSData gpsData = {0};
DeviceInfo deviceInfo = {"", "", "", "", 0, 0, 0, 0};
LocationData currentLocation = {0, 0, "NONE", 0};
String v8RawHex = ""; 

float totalOdometer = 0.0;
float tripOdometer = 0.0;

bool chMOS = false; 
bool dchMOS = false; 
bool baudRateLocked = false;
uint32_t currentBaud = 0;
bool mqttConnected = false;
bool gpsEnabled = false;
bool bmeAvailable = false;

// Last values published, kept so the SMS diagnostic channel can report device
// state without re-reading hardware (P2G#TEST#, P2G#GNSS#).
float    lastBmeTempC  = 0.0f;
uint16_t gnssInView    = 0;
uint16_t gnssWithSignal = 0;
uint16_t gnssBestSnr   = 0;

// Global Instances
HardwareSerial SerialAT(1);
QuectelEC200U modem(SerialAT, 115200, EC200U_RX_PIN, EC200U_TX_PIN);
Adafruit_BME680 bme;

unsigned long lastUpload = 0;
unsigned long lastBMSRead = 0;
unsigned long lastGPSRead = 0;
unsigned long lastDeviceInfoRead = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectCooldown = 20000;

// AWS IoT Core drops the connection outright when a client touches a topic its
// policy does not authorize. The $next/get exchange did exactly that on this
// certificate, so it starts DISABLED: job delivery falls back to notify-next,
// which the policy already allows. Set to true once the policy grants
// iot:Publish + iot:Subscribe on $aws/things/<thing>/jobs/*.
// Trade-off: notify-next only fires when the pending-job list CHANGES, so a job
// queued while this device is offline is not picked up until it changes again.
bool          jobFetchEnabled = true;
unsigned long lastJobFetchMs  = 0;

// ============ HELPER FUNCTIONS ============
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

String sendATCommand(const String &cmd, uint32_t timeoutMs = 5000) {
  /* Whatever is already buffered is about to be discarded. LOOK at it first:
   * an asynchronous "+CMTI:" (new SMS) that landed between commands dies here
   * otherwise, which left the SMS channel depending entirely on its slow
   * storage sweep — a 2-3 minute reply instead of a couple of seconds.
   * Keeps the tail on overflow, since a URC is newer than the noise before it. */
  {
    static char pre[256];
    size_t pn = 0;
    while (SerialAT.available()) {
      if (pn >= sizeof(pre) - 1) {
        size_t shift = sizeof(pre) / 2;
        memmove(pre, pre + shift, sizeof(pre) - shift);
        pn -= shift;
      }
      pre[pn++] = (char)SerialAT.read();
    }
    if (pn) { pre[pn] = '\0'; smsNoteUrc(pre); }
  }

  SerialAT.println(cmd);

  String resp = "";
  resp.reserve(256);          // avoid a realloc per appended character
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      resp += (char)SerialAT.read();
    }
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1 || resp.indexOf("+CME ERROR") != -1) {
      delay(50);
      while (SerialAT.available()) resp += (char)SerialAT.read();
      smsNoteUrc(resp.c_str());   // a URC can also land inside a command window
      break;
    }
  }
  return resp;
}

String extractBetween(const String &response, const String &start_delim, const String &end_delim) {
  int start = response.indexOf(start_delim);
  if (start == -1) return "";
  start += start_delim.length();
  int end = response.indexOf(end_delim, start);
  if (end == -1) end = response.length();
  String result = response.substring(start, end);
  result.trim();
  return result;
}

// ============ SENSOR & DEVICE INFORMATION ============

void collectDeviceInfo() {
  String imeiResp = sendATCommand("AT+CGSN", 3000);
  String parsedImei = "";
  for (int i = 0; i < imeiResp.length(); i++) {
    if (isDigit(imeiResp[i])) parsedImei += imeiResp[i];
  }
  if (parsedImei.length() >= 15) deviceInfo.imei = parsedImei.substring(0, 15);
  
  String signalResp = sendATCommand("AT+CSQ", 3000);
  String rssi = extractBetween(signalResp, "+CSQ: ", ",");
  if (rssi.length() > 0) {
    int rssi_int = rssi.toInt();
    deviceInfo.signal_strength = (rssi_int == 99) ? 0 : (-113 + (2 * rssi_int));
  }
  
  String qengResp = sendATCommand("AT+QENG=\"servingcell\"", 3000);
  String payload = extractBetween(qengResp, "+QENG: ", "\n");
  payload.trim();
  
  if (payload.length() > 0) {
    String rat = getValue(payload, ',', 2);
    rat.replace("\"", "");
    
    if (rat == "LTE") {
      deviceInfo.data_mode = 4;
      String mcc = getValue(payload, ',', 4);
      String mnc = getValue(payload, ',', 5);
      deviceInfo.operator_code = mcc + mnc;
    }
  }
}

// GNSS session arming.
//
// The EC200U owns its GNSS session independently of the ESP32: an ESP32 reset
// does NOT power-cycle the modem, so a session started before the reset is
// still running afterwards. AT+QGPS=1 then answers "+CME ERROR: 504" (session
// is ongoing) instead of OK.
//
// The old code did `if (resp.indexOf("OK") != -1) gpsEnabled = true;` exactly
// once, in setup(). On every boot after the first, 504 latched gpsEnabled to
// false permanently, readGPS() returned on its first line forever, and the
// payload shipped latitude 0 / longitude 0 / source "NONE" indefinitely -- the
// modem was tracking satellites the whole time. Same latch for the transient
// 503 (GNSS subsystem busy) when QGPS=1 lands too soon after modem boot.
//
// So: never trust a one-shot arm. AT+QGPS? is the authoritative state, and a
// failed arm is retried from the loop instead of disabling GNSS for the life
// of the image.
static unsigned long lastGnssArmMs   = 0;
static bool          gnssEverFixed   = false;
static uint32_t      noFixPolls      = 0;
static const unsigned long gnssArmRetryMs = 30000;

static bool ensureGnssOn() {
  if (gpsEnabled) return true;
  if (lastGnssArmMs != 0 && millis() - lastGnssArmMs < gnssArmRetryMs) return false;
  lastGnssArmMs = millis();

  // Already running (the common case after an ESP32-only reset)?
  if (sendATCommand("AT+QGPS?", 1000).indexOf("+QGPS: 1") != -1) {
    Serial.println("[GNSS] Session already active on modem.");
    gpsEnabled = true;
    return true;
  }

  String arm = sendATCommand("AT+QGPS=1", 3000);
  if (arm.indexOf("OK") == -1) {
    // Re-read the state rather than pattern-matching error codes: 504 means it
    // is on despite the error, anything else means it really is off.
    if (sendATCommand("AT+QGPS?", 1000).indexOf("+QGPS: 1") == -1) {
      arm.trim();
      Serial.println("[GNSS] Arm failed, retrying in 30s. Modem said: " + arm);
      return false;
    }
  }
  Serial.println("[GNSS] Receiver armed. Waiting for first fix...");
  gpsEnabled = true;
  // Needed before AT+QGPSGNMEA will return anything (manual 2.3.1.2). Costs one
  // AT round trip and enables the satellite-visibility diagnostic below.
  sendATCommand("AT+QGPSCFG=\"nmeasrc\",1", 1000);
  return true;
}

// Per-satellite GNSS report.
//
// "+CME ERROR: 516" says there is no fix but not whether the receiver can see
// anything, which is the distinction that actually matters during bring-up:
//   satellites with signal, no fix -> poor sky view, needs time or open sky
//   zero satellites with signal    -> GNSS antenna disconnected, dead, or on
//                                     the wrong connector (separate from LTE)
//
// GSV lists everything in view (PRN, elevation, azimuth, SNR), with the NMEA
// talker ID naming the constellation. GSA lists the PRNs actually used in the
// position solution. Together they answer "which satellites produced this fix".

// Constellation from the two talker characters preceding "GSV"/"GSA".
static const char* gnssTalker(char a, char b) {
  if (a == 'G' && b == 'P') return "GPS";
  if (a == 'G' && b == 'L') return "GLO";
  if (a == 'G' && b == 'A') return "GAL";
  if (a == 'G' && b == 'B') return "BDS";
  if (a == 'B' && b == 'D') return "BDS";
  if (a == 'P' && b == 'Q') return "BDS";   // per QGPSCFG="beidounmeaformat"
  if (a == 'G' && b == 'N') return "MIX";
  return "???";
}

// Resolve a raw NMEA satellite number into (constellation, in-system number).
//
// Necessary because the SAME satellite is numbered differently depending on
// which sentence carries it: $GAGSV lists a Galileo bird as PRN 2, while a
// combined $GNGSA may list that same bird as 302. Normalising both sides is
// what makes "is this satellite in the fix?" answerable at all -- comparing raw
// numbers marks nothing (different namespaces) or, worse, confuses GPS PRN 2
// with Galileo PRN 2.
//
// SBAS is reported as 33-64, which maps to real PRN 120-151 by adding 87 --
// worth resolving, since 127/128 are GAGAN.
static const char* prnResolve(int raw, const char* talker, int* outPrn) {
  *outPrn = raw;

  // No constellation numbers its own satellites in 120-158, so this range is
  // unambiguously SBAS whichever talker carried it (EGNOS birds sometimes turn
  // up under $GAGSV).
  if (raw >= 120 && raw <= 158) return "SBAS";

  // Dedicated talkers are already unambiguous, but tolerate firmwares that use
  // the offset ranges anyway.
  if (strcmp(talker, "GAL") == 0) { if (raw >= 301 && raw <= 336) *outPrn = raw - 300; return "GAL"; }
  if (strcmp(talker, "GLO") == 0) { if (raw >= 65  && raw <= 96)  *outPrn = raw - 64;  return "GLO"; }
  if (strcmp(talker, "BDS") == 0) { if (raw >= 201 && raw <= 237) *outPrn = raw - 200; return "BDS"; }

  // $GPxxx and $GNxxx are ambiguous: this module reports SBAS and QZSS inside
  // $GPGSV, and $GNGSA lumps every constellation together. Classify by range.
  if (strcmp(talker, "GPS") != 0 && strcmp(talker, "MIX") != 0) return talker;
  if (raw >= 1   && raw <= 32)  return "GPS";
  if (raw >= 33  && raw <= 64)  { *outPrn = raw + 87; return "SBAS"; }
  if (raw >= 65  && raw <= 96)  { *outPrn = raw - 64; return "GLO";  }
  if (raw >= 193 && raw <= 202) return "QZSS";
  if (raw >= 201 && raw <= 237) { *outPrn = raw - 200; return "BDS"; }
  if (raw >= 301 && raw <= 336) { *outPrn = raw - 300; return "GAL"; }
  return talker;
}

/* NMEA 4.11 appends a systemId to GSA, after VDOP. Without it a combined
 * $GNGSA is unreadable: this module emits one GSA per constellation, all under
 * the GN talker, each numbering satellites in ITS OWN namespace. BeiDou PRN 8
 * and GPS PRN 8 are different satellites sharing an integer, and matching on
 * that integer marked a zero-SNR GPS row as "in fix" when the real occupant of
 * the solution was a BeiDou bird. */
static const char* gsaSystemName(int sysId) {
  switch (sysId) {
    case 1:  return "GPS";
    case 2:  return "GLO";
    case 3:  return "GAL";
    case 4:  return "BDS";
    case 5:  return "QZSS";
    default: return "MIX";     // pre-4.11 firmware: fall back to range guessing
  }
}

// Comma-separated field <n> of an NMEA sentence body.
static String nmeaField(const String &s, int n) {
  int from = 0, idx = 0;
  while (from <= (int)s.length()) {
    int c = s.indexOf(',', from);
    bool last = (c == -1);
    if (last) c = s.length();
    if (idx == n) return s.substring(from, c);
    from = c + 1;
    idx++;
    if (last) break;
  }
  return "";
}

static void logGnssSatellites() {
  // --- Which PRNs are in the solution, from GSA (one sentence per system) ---
  String gsa = sendATCommand("AT+QGPSGNMEA=\"GSA\"", 3000);

  // One-shot raw dump. Whether the IN FIX marker can be matched at all depends
  // on how this firmware numbers satellites in GSA, and that varies: a combined
  // $GNGSA may list Galileo PRN 2 as "2" (indistinguishable from GPS PRN 2) or
  // as "302", and NMEA 4.10+ appends a systemID field to disambiguate. Printed
  // once so the scheme is readable from a normal boot log.
  static bool dumpedRawGsa = false;
  if (!dumpedRawGsa) {
    dumpedRawGsa = true;
    String raw = gsa; raw.trim();
    Serial.println("[GNSS] raw GSA >>> " + raw);
  }

  const char* usedSys[32]; int usedPrn[32]; bool usedSeen[32]; int usedN = 0;
  String hdopStr;
  for (int p = 0;;) {
    int g = gsa.indexOf("GSA,", p);
    if (g == -1) break;
    int eol = gsa.indexOf('\r', g);
    if (eol == -1) eol = gsa.length();
    String body = gsa.substring(g + 4, eol);

    // systemId sits after PDOP/HDOP/VDOP; "1*0C" parses to 1. Absent on older
    // firmware, in which case fall back to the talker.
    int sysId = nmeaField(body, 17).toInt();
    const char* gsaSys = sysId ? gsaSystemName(sysId)
                               : ((g >= 2) ? gnssTalker(gsa[g - 2], gsa[g - 1]) : "???");
    // <mode1>,<fixType>,<prn>x12,<PDOP>,<HDOP>,<VDOP>
    //
    // PRN is only unique WITHIN a constellation -- GPS PRN 2 and Galileo PRN 2
    // are different satellites carrying the same number. Matching on the bare
    // integer both mismarked rows and collapsed distinct satellites during
    // dedup, so key on (system, PRN) instead.
    for (int f = 2; f <= 13 && usedN < 32; f++) {
      String v = nmeaField(body, f);
      if (v.length() == 0) continue;
      int rawPrn = v.toInt();
      if (rawPrn == 0) continue;
      int prn; const char* sys = prnResolve(rawPrn, gsaSys, &prn);
      bool dup = false;
      for (int u = 0; u < usedN; u++) {
        if (usedPrn[u] == prn && strcmp(usedSys[u], sys) == 0) { dup = true; break; }
      }
      if (!dup) { usedSys[usedN] = sys; usedPrn[usedN] = prn; usedSeen[usedN] = false; usedN++; }
    }
    if (hdopStr.length() == 0) hdopStr = nmeaField(body, 15);
    p = eol;
  }

  // --- Everything in view, from GSV ---
  String gsv = sendATCommand("AT+QGPSGNMEA=\"GSV\"", 2000);
  int listed = 0, withSignal = 0, bestSnr = 0;
  bool sawSentence = false;

  for (int p = 0;;) {
    int g = gsv.indexOf("GSV,", p);
    if (g == -1) break;
    if (!sawSentence) Serial.println("[GNSS] ---- satellites ----");
    sawSentence = true;
    const char* sys = (g >= 2) ? gnssTalker(gsv[g - 2], gsv[g - 1]) : "???";
    int eol = gsv.indexOf('\r', g);
    if (eol == -1) eol = gsv.length();
    String body = gsv.substring(g + 4, eol);
    p = eol;

    // <numMsgs>,<msgNum>,<inView>[,<prn>,<elev>,<azim>,<snr>] x4
    for (int k = 0; k < 4; k++) {
      String prnStr = nmeaField(body, 3 + 4 * k);
      if (prnStr.length() == 0) continue;
      int rawPrn = prnStr.toInt();
      if (rawPrn == 0) continue;         // padding slot in a partly-filled GSV
      int snr = nmeaField(body, 6 + 4 * k).toInt();   // trailing "*7A" ignored

      int prn; const char* realSys = prnResolve(rawPrn, sys, &prn);
      bool inFix = false;
      for (int u = 0; u < usedN; u++) {
        if (usedPrn[u] == prn && strcmp(usedSys[u], realSys) == 0) {
          inFix = true;
          usedSeen[u] = true;    // matched to a visible row
          break;
        }
      }

      Serial.printf("  %-4s PRN %-4d el %-3d az %-4d SNR %-3d %s\n",
                    realSys, prn,
                    nmeaField(body, 4 + 4 * k).toInt(),
                    nmeaField(body, 5 + 4 * k).toInt(),
                    snr, inFix ? "<- IN FIX" : "");
      listed++;
      if (snr > 0) { withSignal++; if (snr > bestSnr) bestSnr = snr; }
    }
  }

  if (!sawSentence) {
    Serial.println("[GNSS] No GSV sentences returned -- receiver is not reporting.");
    return;
  }
  // Cache for the SMS diagnostic channel, which must answer P2G#GNSS# without
  // taking the AT bus for a fresh GSV sweep.
  gnssInView     = (uint16_t)listed;
  gnssWithSignal = (uint16_t)withSignal;
  gnssBestSnr    = (uint16_t)bestSnr;

  // Satellites in the solution with no GSV row are real and working -- their
  // constellation simply has NMEA sentence output disabled (QGPSCFG
  // "beidounmeatype" / "glonassnmeatype" / "galileonmeatype"). Say so, rather
  // than leaving an unexplained gap between the counts.
  int hidden = 0;
  for (int u = 0; u < usedN; u++) if (!usedSeen[u]) hidden++;

  Serial.printf("[GNSS] %d in view, %d with signal, %d in fix, best SNR %d dBHz",
                listed, withSignal, usedN, bestSnr);
  if (hdopStr.length() > 0) Serial.print(", HDOP " + hdopStr);
  Serial.println();
  if (hidden > 0) {
    Serial.printf("[GNSS] %d in fix are not listed above (that constellation's "
                  "GSV output is off):", hidden);
    for (int u = 0; u < usedN; u++)
      if (!usedSeen[u]) Serial.printf(" %s-%d", usedSys[u], usedPrn[u]);
    Serial.println();
  }
  if (withSignal == 0) {
    Serial.println("[GNSS] 0 satellites with signal -- check the GNSS antenna "
                   "(separate connector from the LTE antenna).");
  }
}

void readGPS() {
  if (!ensureGnssOn()) return;

  String gpsResp = sendATCommand("AT+QGPSLOC=2", 3000);
  gpsData.gpsFixed = false;

  // Match the header without assuming the space after the colon, then slice the
  // payload ourselves. extractBetween(resp, "+QGPSLOC: ", ...) silently yielded
  // an empty string on any firmware that omits it, which parsed to all zeros.
  int hdr = gpsResp.indexOf("+QGPSLOC:");
  if (hdr != -1) {
    int ds = hdr + 9;                                   // past "+QGPSLOC:"
    while (ds < (int)gpsResp.length() && gpsResp[ds] == ' ') ds++;
    int de = gpsResp.indexOf('\n', ds);
    if (de == -1) de = gpsResp.length();
    String data = gpsResp.substring(ds, de);
    data.trim();

    // <UTC>,<lat>,<lon>,<HDOP>,<alt>,<fix>,<COG>,<spkm>,<spkn>,<date>,<nsat>
    int fields = 0; String values[11]; int lastIdx = 0;

    for (int i = 0; i <= data.length(); i++) {
      if (data[i] == ',' || i == data.length()) {
        if (fields < 11) {
          values[fields] = data.substring(lastIdx, i);
          values[fields].trim();
          lastIdx = i + 1;
          fields++;
        }
      }
    }

    if (fields >= 11) {
      gpsData.latitude = values[1].toFloat();
      gpsData.longitude = values[2].toFloat();
      gpsData.hdop = values[3].toFloat();
      gpsData.altitude = values[4].toFloat();
      gpsData.fix = values[5].toInt();
      gpsData.speed = values[7].toFloat();
      gpsData.satellites = values[10].toInt();
      gpsData.gpsFixed = (gpsData.satellites > 0 && gpsData.fix >= 2);
      gpsData.lastUpdate = millis();

      if (!gpsData.gpsFixed) {
        Serial.printf("[GNSS] Frame rejected: fix=%d sats=%d\n",
                      gpsData.fix, gpsData.satellites);
      } else {
        if (!gnssEverFixed) {
          gnssEverFixed = true;
          Serial.printf("[GNSS] FIRST FIX %.6f,%.6f  sats=%d hdop=%.1f fix=%dD\n",
                        gpsData.latitude, gpsData.longitude,
                        gpsData.satellites, gpsData.hdop, gpsData.fix);
          // One-shot breakdown of which satellites and constellations produced
          // it -- this is the measurement to compare across gnssconfig values.
          logGnssSatellites();
        }
        currentLocation.latitude = gpsData.latitude;
        currentLocation.longitude = gpsData.longitude;
        currentLocation.source = "GPS";
        
        if (currentLocation.timestamp > 0) {
           float timeDiffHours = (millis() - currentLocation.timestamp) / 3600000.0;
           if (gpsData.speed > 2.0) { 
              float dist = gpsData.speed * timeDiffHours;
              totalOdometer += dist;
              tripOdometer += dist;
           }
        }
        currentLocation.timestamp = millis();
      }
    } else {
      Serial.printf("[GNSS] Short frame (%d/11 fields): %s\n", fields, data.c_str());
    }
  } else {
    // No +QGPSLOC header: the modem answered with +CME ERROR.
    //   516 Not fixed now      -- normal while the receiver is still searching
    //   505 Session not active -- GNSS died or was ended; re-arm next pass
    //   507 Function not enabled / 503 busy -- also worth re-arming
    // Anchored on "ERROR: " so an MQTT downlink payload that happens to sit in
    // the same buffer and contain "505" cannot drop the session. Matches both
    // +CME and +CMS forms -- the manual returns 505 as +CMS ERROR (p.32).
    if (gpsResp.indexOf("ERROR: 505") != -1 || gpsResp.indexOf("ERROR: 507") != -1) {
      Serial.println("[GNSS] Session dropped, will re-arm.");
      gpsEnabled = false;
      lastGnssArmMs = 0;             // re-arm immediately, don't wait 30s
    } else if (!gnssEverFixed) {
      // Verbose only until the first fix lands -- this is the window where the
      // antenna/sky/TTFF problems actually show up. Quiet afterwards.
      int e = gpsResp.indexOf("ERROR:");
      String code = (e != -1) ? gpsResp.substring(e + 6) : gpsResp;
      code.trim();
      Serial.println("[GNSS] No fix yet (err " + code + ")");

      // Every 6th miss (~60 s) report what the receiver can actually see, so a
      // roof overhead is distinguishable from a dead antenna without guesswork.
      if (++noFixPolls % 6 == 0) logGnssSatellites();
    }
  }

  if (!gpsData.gpsFixed) {
    // NOTE: AT+QCELLLOC belongs to the BG96/EC25 QuecLocator 1.0 set and is NOT
    // implemented on EC200U -- it answers +CME ERROR here, so this fallback has
    // never produced a coordinate. EC200U needs QuecLocator 2.0 (AT+QLBSCFG
    // token/APN setup, then AT+QLBS), which requires a Quectel-issued token.
    // Left in place so the "GPS -> LBS" path is obvious, but it is a no-op.
    String lbsResp = sendATCommand("AT+QCELLLOC=1", 5000);
    if (lbsResp.indexOf("+QCELLLOC:") != -1) {
      String locData = extractBetween(lbsResp, "+QCELLLOC: ", "\n");
      int commaIdx = locData.indexOf(",");
      if (commaIdx != -1) {
        currentLocation.longitude = locData.substring(0, commaIdx).toFloat();
        currentLocation.latitude = locData.substring(commaIdx + 1).toFloat();
        currentLocation.source = "LBS (approx)";
        currentLocation.timestamp = millis();
      }
    }
  }
}

// ============ AWS MQTT HANDLING ============
// publishToAWS() lives in aws_iot_core.cpp — main.cpp used to carry a private
// duplicate of it, which left the refactored version as dead code.

/* Publish, then capture what comes back.
 * The shared publishToAWS() spends 500 ms discarding bytes to consume the
 * publish ack, which destroys any URC arriving in that window — including the
 * reply to our own $next/get request. */
static String publishAndCapture(const char* topic, const char* payload, uint32_t waitMs) {
  char cmdBuf[128];
  int n = snprintf(cmdBuf, sizeof(cmdBuf), "AT+QMTPUBEX=0,1,1,0,\"%s\",%u",
                   topic, (unsigned)strlen(payload));
  if (n < 0 || (size_t)n >= sizeof(cmdBuf)) {
    Serial.println("[ERROR] MQTT command truncated!");
    return "";
  }

  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmdBuf);

  unsigned long t0 = millis();
  bool prompt = false;
  while (millis() - t0 < 3000 && !prompt) {
    if (SerialAT.available()) { if (SerialAT.read() == '>') prompt = true; }
    else delay(1);
  }
  if (!prompt) { Serial.println("[ERROR] Modem timeout waiting for '>' prompt."); return ""; }

  SerialAT.print(payload);

  String resp;
  t0 = millis();
  while (millis() - t0 < waitMs) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
    // Stop as soon as a complete downlink payload has landed.
    if (resp.indexOf("+QMTRECV:") >= 0 && resp.indexOf('}') >= 0) break;
    delay(1);
  }
  return resp;
}

void connectAndVerify() {
  Serial.println("[MQTT] Connecting to AWS IoT Core...");
  sendATCommand("AT+QMTDISC=0", 3000); 
  sendATCommand("AT+QMTCLOSE=0", 5000); 
  delay(500);
  
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTOPEN=0,\"" + String(AWS_IOT_ENDPOINT) + "\"," + String(mqtt_port));

  String openResp = ""; unsigned long start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) openResp += (char)SerialAT.read();
    if (openResp.indexOf("+QMTOPEN: 0,") != -1) break;
  }
  
  Serial.println("[DEBUG] QMTOPEN Response: " + openResp);

  if (openResp.indexOf("+QMTOPEN: 0,0") == -1) { 
    Serial.println("[ERROR] Failed to open MQTT socket to AWS!");
    mqttConnected = false; 
    return; 
  }

  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTCONN=0,\"" + String(THING_NAME) + "\"");

  String connResp = ""; start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) connResp += (char)SerialAT.read();
    if (connResp.indexOf("+QMTCONN: 0,") != -1) break;
  }
  
  if (connResp.indexOf("+QMTCONN: 0,0,0") != -1) {
    mqttConnected = true;
    Serial.println("[SUCCESS] AWS IoT Core Connected via mTLS!");

    // Reaching AWS is the definition of a healthy image: clears OTA probation
    // and resets the cloud-silence supervisor.
    healthOnCloudSuccess();
    
    String thing = String(THING_NAME);
    String sub1 = sendATCommand("AT+QMTSUB=0,1,\"$aws/things/" + thing + "/jobs/notify-next\",1", 10000);
    Serial.println("[MQTT] SUB notify-next -> " + sub1);

    // Confirm this image is healthy and acknowledge any OTA job that completed
    // before the last reboot. Until AWS sees SUCCEEDED/FAILED the execution
    // stays open, so this must happen before we ask for the next job or we get
    // handed the same one straight back.
    otaBootReport();
    otaFlushDeferredReport();

    // notify-next is only published when the pending-job list CHANGES. A job
    // queued while this device was offline or rebooting is never re-announced,
    // so ask for the current one explicitly instead of waiting forever.
    if (jobFetchEnabled) {
      String sub2 = sendATCommand("AT+QMTSUB=0,2,\"$aws/things/" + thing + "/jobs/$next/get/accepted\",1", 10000);
      Serial.println("[MQTT] SUB $next/get/accepted -> " + sub2);

      String getTopic = "$aws/things/" + thing + "/jobs/$next/get";
      lastJobFetchMs = millis();
      String jobResp = publishAndCapture(getTopic.c_str(), "{}", 4000);
      Serial.println("[AWS OTA] Requested pending job via $next/get");

      if (otaCheckDownlink(jobResp.c_str())) {
        // Job queued; loop() runs otaRun() on its next pass.
      } else if (jobResp.indexOf("get/accepted") >= 0) {
        Serial.println("[AWS OTA] No pending job.");
      } else {
        Serial.println("[AWS OTA] No reply to $next/get.");
      }
    }
  } else {
    Serial.println("[ERROR] AWS MQTT Auth Failed! Raw Response: " + connResp);
    mqttConnected = false; 
  }
}

void checkIncoming() {
  if (!SerialAT.available()) return;
  String line = "";
  line.reserve(1600);         // a job document with a pre-signed URL runs ~1.4 KB
  unsigned long start = millis();

  while (millis() - start < 500) {
      while (SerialAT.available()) line += (char)SerialAT.read();
  }
  if (line.length() == 0) return;

  // Free ride: this buffer was read anyway, so scan it for the SMS "+CMTI:"
  // indication rather than having smsPoll() spend AT commands looking for one.
  // Before the OTA early-return below, or an OTA downlink would mask it.
  smsNoteUrc(line.c_str());

  // Returning here only exits checkIncoming(); loop() must also bail out before
  // touching SerialAT again, which it does by testing otaPending().
  if (otaCheckDownlink(line.c_str())) return;

  if (line.indexOf("+QMTSTAT:") != -1) {
    Serial.println("[MQTT] Received disconnect URC");
    mqttConnected = false;

    // A drop within seconds of the jobs/$next exchange is AWS refusing an
    // unauthorized topic, not a network problem. Retrying it forever costs us
    // the whole connection, so fall back to notify-next only.
    if (jobFetchEnabled && lastJobFetchMs > 0 && millis() - lastJobFetchMs < 10000) {
      jobFetchEnabled = false;
      Serial.println("[AWS OTA] Dropped immediately after $next/get — the IoT policy on this");
      Serial.println("          certificate does not authorize the jobs topics.");
      Serial.println("          Needs iot:Publish + iot:Subscribe + iot:Receive on");
      Serial.println("          $aws/things/" + String(THING_NAME) + "/jobs/*");
      Serial.println("          Falling back to notify-next only (OTA push still works).");
    }
  }
}

// ============ SYSTEM SETUP ============

void setup() {
  Serial.begin(115200); 
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW); 
  delay(3000);
  
  pinMode(MQ2_PIN, INPUT);
  pinMode(MQ8_PIN, INPUT);

  // Track FW_VERSION rather than a literal — after an OTA the banner is the
  // first evidence of which image actually booted.
  Serial.printf("\n=== POINT-AI FIRMWARE (AWS ENTERPRISE V%s) ===\n", FW_VERSION);
  Serial.printf("[BOOT] reset=%s heap=%u/%u\n",
                healthResetReason(), healthHeapLargest(), healthHeapFree());

  // Starts the hang watchdog and evaluates OTA probation. Must run before any
  // long-blocking init so a wedged modem cannot hold the device down forever.
  healthBegin();
  
  Serial.println("[INIT] Checking BME680 Sensor...");
  Wire.begin(21, 22);
  if (!bme.begin(0x77)) {
    Serial.println("[WARN] BME680 not detected. Continuing without environmental data.");
    bmeAvailable = false;
  } else {
    Serial.println("[SUCCESS] BME680 Ready.");
    bmeAvailable = true;
  }

  Serial.println("[INIT] Contacting EC200U Modem...");
  if (!modem.begin()) {
    Serial.println("[ERROR] Modem not responding to AT. Check RX/TX and Power!");
  } else {
    Serial.println("[SUCCESS] Modem AT Bridge Ready.");
  }

  // CRITICAL: modem.begin() calls SerialAT.begin() which allocates a default
  // 256-byte Rx ring buffer. That is far too small for OTA streaming.
  // We must end() -> setRxBufferSize() -> begin() AFTER modem.begin() so
  // the 32 KB buffer is the final, active one.
  SerialAT.end();
  SerialAT.setRxBufferSize(32768);
  SerialAT.begin(115200, SERIAL_8N1, EC200U_RX_PIN, EC200U_TX_PIN);
  delay(100);
  Serial.println("[INIT] UART Rx buffer enlarged to 32 KB.");
  
  Serial.println("[INIT] Registering to Cellular Network...");
  bool netOk = false;
  for (int i = 0; i < 15; i++) { 
    if (modem.waitForNetwork(1000)) {
      netOk = true;
      break;
    }
    Serial.print(".");
  }
  
  if (!netOk) {
    Serial.println("\n[WARN] Cellular Network delay. Will attempt reconnecting in loop.");
  } else {
    Serial.println("\n[SUCCESS] Network Registered.");
  }
  
  modem.attachData(apn.c_str());
  
  sendATCommand("AT+QIACT=1", 10000);
  sendATCommand("AT+QMTCFG=\"pdpcid\",0,1"); 
  sendATCommand("AT+QMTCFG=\"version\",0,4"); 
  
  // --- START OF APPLIED FIXES ---
  Serial.println("[DEBUG] Applying SSL Context 2 Settings...");
  Serial.println(sendATCommand("AT+QMTCFG=\"ssl\",0,1,2"));   
  
  Serial.println(sendATCommand("AT+QSSLCFG=\"sslversion\",2,4")); 
  Serial.println(sendATCommand("AT+QSSLCFG=\"ciphersuite\",2,0xFFFF")); 
  Serial.println(sendATCommand("AT+QSSLCFG=\"ignorelocaltime\",2,1"));  
  Serial.println(sendATCommand("AT+QSSLCFG=\"sni\",2,1")); 
  
  Serial.println(sendATCommand("AT+QSSLCFG=\"seclevel\",2,2")); 
  Serial.println(sendATCommand("AT+QSSLCFG=\"cacert\",2,\"UFS:rootCA.pem\""));
  Serial.println(sendATCommand("AT+QSSLCFG=\"clientcert\",2,\"UFS:cert.pem\""));
  Serial.println(sendATCommand("AT+QSSLCFG=\"clientkey\",2,\"UFS:privkey.pem\""));
  // --- END OF APPLIED FIXES ---

  // Best-effort arm. A failure here is no longer terminal: readGPS() calls
  // ensureGnssOn() on every pass and retries every 30 s until it takes.
  ensureGnssOn();

  // --- START OF CAN INIT ---
  Serial.println("[INIT] Attempting CAN connection at 500kbps...");
  if (startCAN(500000)) {
    unsigned long startTime = millis();
    while (millis() - startTime < 3000) {
      uint8_t testBuf[8];
      if (requestFrame(0x100, testBuf)) { 
        baudRateLocked = true; 
        Serial.println("[SUCCESS] Locked at 500kbps!");
        break; 
      } 
      delay(200);
    }
  }
  
  if (!baudRateLocked) {
    Serial.println("[WARN] Failed at 500kbps. Attempting 250kbps...");
    if (startCAN(250000)) {
      unsigned long startTime = millis();
      while (millis() - startTime < 3000) {
        uint8_t testBuf[8];
        if (requestFrame(0x100, testBuf)) { 
          baudRateLocked = true; 
          Serial.println("[SUCCESS] Locked at 250kbps!");
          break; 
        } 
        delay(200);
      }
    }
  }

  if (!baudRateLocked) {
    Serial.println("[ERROR] Could not lock BMS baud rate. Check wiring.");
  }
  // --- END OF CAN INIT ---
  
  collectDeviceInfo();

  // NOTE: SMS diagnostics are NOT started here. Configuring SMS before the
  // modem emits "+QIND: SMS DONE" left it busy enough that the QMTOPEN below
  // failed outright. smsPoll() initialises the channel itself, 45 s in.
  connectAndVerify();
}

// ============ MAIN EVENT LOOP ============

/* Milliseconds until the soonest scheduled task. Anything that wants to borrow
 * the AT bus opportunistically asks this first, so background work lands in the
 * gaps between telemetry, GNSS and BMS rather than on top of them. */
static unsigned long _remainingMs(unsigned long last, unsigned long interval) {
  unsigned long elapsed = millis() - last;
  return (elapsed >= interval) ? 0UL : (interval - elapsed);
}

static unsigned long loopSlackMs() {
  unsigned long s = _remainingMs(lastUpload, uploadInterval);
  unsigned long g = _remainingMs(lastGPSRead, gpsReadInterval);
  unsigned long b = _remainingMs(lastBMSRead, bmsReadInterval);
  if (g < s) s = g;
  if (b < s) s = b;
  return s;
}

void loop() {
  // Heartbeat + cloud-silence supervisor. First thing every iteration, before
  // any early return, so a device stuck reconnecting still escalates.
  healthTick();

  // Cooperative OTA: otaRun() owns the AT bus for its whole duration. It runs
  // here, in the loop task, rather than in a parallel FreeRTOS task — SerialAT
  // has no mutex, so a second task issuing AT commands during the download
  // corrupts both streams.
  if (otaPending()) {
    healthNoteOtaActivity();
    otaRun();
    return;
  }

  // Deliberately ABOVE the mqttConnected gate. The entire value of this channel
  // is reaching a device whose cloud path is broken, and that branch returns
  // early -- putting the poll below it would make SMS work only when it is
  // least needed. Below the OTA guard, though: otaRun() owns the AT bus.
  //
  // Hand it the size of the idle gap so a queued command waits for a quiet
  // moment instead of elbowing telemetry aside. It overrides this on its own
  // once a command has waited too long, so the wait stays bounded.
  smsPoll(loopSlackMs());

  if (!mqttConnected) {
    if (millis() - lastReconnectAttempt > reconnectCooldown) {
      lastReconnectAttempt = millis();
      healthCountReconnect();
      connectAndVerify();
    }
    return;
  }

  checkIncoming();

  // A job may have just been queued by checkIncoming(). Nothing below may touch
  // SerialAT before otaRun() has had the bus.
  if (otaPending()) return;

  if (millis() - lastGPSRead >= gpsReadInterval) { 
    lastGPSRead = millis(); 
    readGPS(); 
  }
  
  if (millis() - lastDeviceInfoRead >= deviceInfoInterval) { 
    lastDeviceInfoRead = millis(); 
    collectDeviceInfo(); 
  }
  if (millis() - lastBMSRead >= bmsReadInterval) {
    lastBMSRead = millis();
    readBMS();
  }

  if (millis() - lastUpload >= uploadInterval) {
    lastUpload = millis();

    int rawMQ2 = analogRead(MQ2_PIN);
    int rawMQ8 = analogRead(MQ8_PIN);

    float temp = 0.0, hum = 0.0, press = 0.0, gasRes = 0.0;
    if (bmeAvailable) {
      if (bme.performReading()) {
        temp = bme.temperature;
        hum = bme.humidity;
        press = bme.pressure / 100.0F;
        gasRes = bme.gas_resistance / 1000.0F;
      }
    }
    lastBmeTempC = temp;   // 0.0 when absent or the read failed -> P2G#TEST# E_BME

    char cellStr[256] = "";
    for (int i = 0; i < bmsData.cellCount; i++) {
      char tmp[16];
      snprintf(tmp, sizeof(tmp), "%.3f%s", bmsData.cellV[i], (i == bmsData.cellCount - 1) ? "" : ",");
      strncat(cellStr, tmp, sizeof(cellStr) - strlen(cellStr) - 1);
    }
    char tempStr[128] = "";
    for (int i = 0; i < 6; i++) {
      char tmp[16];
      snprintf(tmp, sizeof(tmp), "%.1f%s", bmsData.temp[i], (i == 5) ? "" : ",");
      strncat(tempStr, tmp, sizeof(tempStr) - strlen(tempStr) - 1);
    }

    /* BMS data is only meaningful with its age attached. bmsData keeps its last
     * values when CAN drops, so without this the cloud reads frozen numbers as
     * live — the most dangerous failure mode for battery telemetry. */
    long bmsAgeS = (bmsData.lastUpdate == 0)
                   ? -1 : (long)((millis() - bmsData.lastUpdate) / 1000UL);
    int  bmsValid = (bmsAgeS >= 0 && bmsAgeS <= (long)(bmsReadInterval / 1000UL) * 3) ? 1 : 0;

    // Zero-Allocation JSON Construction using Static Buffer (No Memory Leaks)
    char jsonPayload[2560];
    int written = snprintf(jsonPayload, sizeof(jsonPayload),
        "{"
        "\"thing_name\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"health\":{\"uptime_s\":%u,\"reset\":\"%s\",\"heap_free\":%u,"
        "\"heap_largest\":%u,\"reconnects\":%u,\"probation\":%d},"
        "\"location\":{\"latitude\":%.6f,\"longitude\":%.6f,\"source\":\"%s\",\"speed_kmh\":%.2f,\"satellites\":%d},"
        "\"sensors\":{\"mq2_gas_raw\":%d,\"mq8_gas_raw\":%d,\"bme680\":{\"temp_c\":%.2f,\"humidity_pct\":%.2f,\"pressure_hpa\":%.2f,\"gas_res_kohm\":%.2f}},"
        "\"telemetry\":{\"rssi\":%d,\"imei\":\"%s\",\"total_odometer\":%.2f},"
        "\"bms\":{\"age_s\":%ld,\"valid\":%d,"
        "\"voltage\":%.2f,\"current\":%.2f,\"soc\":%d,\"residual_cap\":%.2f,\"full_cap\":%.2f,\"cycles\":%d,"
        "\"balance\":%d,\"protection\":%d,\"chg_mos\":%d,\"dsg_mos\":%d,\"temps\":[%s],\"cell_count\":%d,"
        "\"min_cell\":%.3f,\"max_cell\":%.3f,\"avg_cell\":%.3f,\"delta_cell\":%.3f,\"cells\":[%s]}"
        "}",
        THING_NAME, FW_VERSION,
        healthUptimeS(), healthResetReason(), healthHeapFree(),
        healthHeapLargest(), healthReconnects(), healthProbation() ? 1 : 0,
        currentLocation.latitude, currentLocation.longitude, currentLocation.source.c_str(), gpsData.speed, gpsData.satellites,
        rawMQ2, rawMQ8, temp, hum, press, gasRes,
        deviceInfo.signal_strength, deviceInfo.imei.c_str(), totalOdometer,
        bmsAgeS, bmsValid,
        bmsData.packVoltage, bmsData.packCurrent, bmsData.soc, bmsData.residualCapacity, bmsData.fullCapacity, bmsData.cycles,
        bmsData.balanceState, bmsData.protectionFlags, bmsData.chMOSFET_Act, bmsData.dchMOSFET_Act, tempStr, bmsData.cellCount,
        bmsData.minCellV, bmsData.maxCellV, bmsData.avgCellV, bmsData.cellDeltaV, cellStr
    );

    if (written > 0 && (size_t)written < sizeof(jsonPayload)) {
        char topicBuf[64];
        snprintf(topicBuf, sizeof(topicBuf), "bms/data/%s/telemetry", THING_NAME);
        
        publishToAWS(SerialAT, topicBuf, jsonPayload);

        Serial.println("\n[MQTT] Published JSON Payload to AWS (Zero-Allocation):");
        Serial.println(jsonPayload);
    } else {
        Serial.println("[ERROR] Payload buffer overflow prevented!");
    }
  }
}