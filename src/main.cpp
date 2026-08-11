/* =========================================================
 * FIRMWARE (AWS ENTERPRISE V1.0.0)
 * 100% Zero-Allocation, Non-blocking, OTA-Safe Architecture
 * ========================================================= */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "driver/twai.h"
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "QuectelEC200U.h"
#include "device_health.h"   // must precede EC200U_AWS_OTA.h — it consumes healthAlive()
#include "EC200U_AWS_OTA.h"
#include "aws_iot_core.h"
#include "bms_can_parser.h"
#include "sms_diag.h"        // out-of-band P2G#<CMD># diagnostics over SMS

/* Arm ESP32 bootloader rollback. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the
 * Arduino core, but its weak verifyRollbackLater() returns false, so
 * initArduino() marks a freshly flashed image valid before it has proven
 * anything. Returning true defers that decision to healthOnCloudSuccess(),
 * which only runs once the new image has actually reached AWS IoT.
 *
 * Consequence: an image power-cycled before it ever connects is reverted by the
 * bootloader. Inert for USB flashes — esptool does not stage PENDING_VERIFY. */
#define OTA_ARM_ROLLBACK 1
#if OTA_ARM_ROLLBACK
bool verifyRollbackLater() { return true; }
#endif

// ============ GLOBALS ============
BMSData bmsData = {0};
GPSData gpsData = {0};
DeviceInfo deviceInfo = {"", "", "", "", 0, 0, 0, 0};
LocationData currentLocation = {0.0f, 0.0f, "NONE", 0};

float totalOdometer = 0.0;
float tripOdometer = 0.0;

bool mqttConnected = false;
bool gpsEnabled = false;
bool bmeAvailable = false;

// Last BME680 temperature actually read (0.0 when absent or the read failed).
// Consumed by the SMS diagnostic channel to answer P2G#TEST# with E_BME.
float lastBmeTempC = 0.0f;

// CAN/TWAI state — consumed by bms_can_parser.cpp via extern declarations.
bool     baudRateLocked = false;
uint32_t currentBaud    = 0;

HardwareSerial SerialAT(1);
QuectelEC200U modem(SerialAT, 115200, EC200U_RX_PIN, EC200U_TX_PIN);
Adafruit_BME680 bme;

unsigned long lastUpload = 0;
unsigned long lastBMSRead = 0;
unsigned long lastGPSRead = 0;
unsigned long lastDeviceInfoRead = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectCooldown = 20000;

// ============ ZERO-ALLOCATION HELPERS ============

void sendATCommandRaw(const char* cmd, char* outBuffer, size_t bufferSize, uint32_t timeoutMs = 5000) {
    // Smart buffer allocation: use local buffer if nullptr is passed
    char tempBuf[256] = {0};
    char* targetBuf = outBuffer ? outBuffer : tempBuf;
    size_t targetSize = outBuffer ? bufferSize : sizeof(tempBuf);

    memset(targetBuf, 0, targetSize);
    while (SerialAT.available()) SerialAT.read(); // Flush
    
    SerialAT.println(cmd);
    unsigned long start = millis();
    size_t idx = 0;
    
    while (millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        while (SerialAT.available() && idx < targetSize - 1) {
            targetBuf[idx++] = (char)SerialAT.read();
        }
        
        // Safely check for standard terminations
        if (strstr(targetBuf, "OK\r\n") || strstr(targetBuf, "ERROR\r\n") || strstr(targetBuf, "+CME ERROR")) {
            delay(50);
            while (SerialAT.available() && idx < targetSize - 1) {
                targetBuf[idx++] = (char)SerialAT.read();
            }
            break; // Exit loop immediately upon success/error
        }
    }
}

// ============ DEVICE INFO & GPS ============

void collectDeviceInfo() {
    char resp[128];
    sendATCommandRaw("AT+CGSN", resp, sizeof(resp), 3000);
    
    size_t iIdx = 0;
    for (size_t i = 0; i < strlen(resp); i++) {
        if (isdigit(resp[i]) && iIdx < 15) {
            deviceInfo.imei[iIdx++] = resp[i];
        }
    }
    deviceInfo.imei[iIdx] = '\0';

    sendATCommandRaw("AT+CSQ", resp, sizeof(resp), 3000);
    char* csqPtr = strstr(resp, "+CSQ: ");
    if (csqPtr) {
        int rssi_int = atoi(csqPtr + 6);
        deviceInfo.signal_strength = (rssi_int == 99) ? 0 : (-113 + (2 * rssi_int));
    }
}

/* ============ GNSS / Q-GPS STACK ============
 *
 * A one-shot AT+QGPS=1 in setup() latched gpsEnabled=false permanently whenever
 * the modem answered 504 (already on) or a transient 503 (subsystem busy from
 * arming too soon after boot). readGPS() then returned on its first line for the
 * life of the image and the payload shipped lat 0 / lon 0 / source "NONE" — while
 * the receiver was tracking satellites the whole time.
 *
 * So: never trust a one-shot arm. AT+QGPS? is the authoritative state, and a
 * failed arm is retried from the loop rather than disabling GNSS permanently. */
static unsigned long       lastGnssArmMs  = 0;
static bool                gnssEverFixed  = false;
static uint32_t            noFixPolls     = 0;
static const unsigned long gnssArmRetryMs = 30000;

// Cached for the SMS diagnostic channel (Phase G), which must answer P2G#GNSS#
// without taking the AT bus for a fresh GSV sweep.
uint16_t gnssInView     = 0;
uint16_t gnssWithSignal = 0;
uint16_t gnssBestSnr    = 0;

static void logGnssSatellites();   // forward declaration

static bool ensureGnssOn() {
    if (gpsEnabled) return true;
    if (lastGnssArmMs != 0 && millis() - lastGnssArmMs < gnssArmRetryMs) return false;
    lastGnssArmMs = millis();

    char resp[160];

    // Already running (the common case after an ESP32-only reset)?
    sendATCommandRaw("AT+QGPS?", resp, sizeof(resp), 1000);
    if (strstr(resp, "+QGPS: 1")) {
        Serial.println("[GNSS] Session already active on modem.");
        gpsEnabled = true;
        return true;
    }

    char arm[160];
    sendATCommandRaw("AT+QGPS=1", arm, sizeof(arm), 3000);
    if (!strstr(arm, "OK")) {
        // Re-read the state rather than pattern-matching error codes: 504 means
        // it is on despite the error, anything else means it really is off.
        sendATCommandRaw("AT+QGPS?", resp, sizeof(resp), 1000);
        if (!strstr(resp, "+QGPS: 1")) {
            Serial.printf("[GNSS] Arm failed, retrying in 30s. Modem said: %s\n", arm);
            return false;
        }
    }
    Serial.println("[GNSS] Receiver armed. Waiting for first fix...");
    gpsEnabled = true;
    // Needed before AT+QGPSGNMEA will return anything (manual 2.3.1.2).
    sendATCommandRaw("AT+QGPSCFG=\"nmeasrc\",1", nullptr, 0, 1000);
    return true;
}

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

/* Resolve a raw NMEA satellite number into (constellation, in-system number).
 *
 * The SAME satellite is numbered differently depending on which sentence carries
 * it: $GAGSV lists a Galileo bird as PRN 2, while a combined $GNGSA may list that
 * same bird as 302. Normalising both sides is what makes "is this satellite in
 * the fix?" answerable at all — comparing raw numbers marks nothing (different
 * namespaces) or, worse, confuses GPS PRN 2 with Galileo PRN 2. */
static const char* prnResolve(int raw, const char* talker, int* outPrn) {
    *outPrn = raw;

    // No constellation numbers its own satellites in 120-158, so this range is
    // unambiguously SBAS whichever talker carried it.
    if (raw >= 120 && raw <= 158) return "SBAS";

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

/* NMEA 4.11 appends a systemId to GSA, after VDOP. Without it a combined $GNGSA
 * is unreadable: this module emits one GSA per constellation, all under the GN
 * talker, each numbering satellites in ITS OWN namespace. BeiDou PRN 8 and GPS
 * PRN 8 are different satellites sharing an integer. */
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

/* Comma-separated field <n> of an NMEA sentence body, non-destructive and
 * allocation-free. strtok_r would clobber the buffer, which matters because the
 * same response is scanned repeatedly for different fields. */
static size_t _nmeaField(const char* s, size_t len, int n, char* out, size_t cap) {
    size_t from = 0;
    int    idx  = 0;
    out[0] = '\0';
    while (from <= len) {
        const void* cp = (from < len) ? memchr(s + from, ',', len - from) : nullptr;
        size_t end = cp ? (size_t)((const char*)cp - s) : len;
        if (idx == n) {
            size_t w = end - from;
            if (w >= cap) w = cap - 1;
            memcpy(out, s + from, w);
            out[w] = '\0';
            return w;
        }
        if (!cp) break;
        from = end + 1;
        idx++;
    }
    return 0;
}

static int _nmeaFieldInt(const char* s, size_t len, int n) {
    char v[16];
    _nmeaField(s, len, n, v, sizeof(v));
    return atoi(v);
}

/* Per-satellite GNSS report.
 *
 * "+CME ERROR: 516" says there is no fix but not whether the receiver can see
 * anything, which is the distinction that actually matters during bring-up:
 *   satellites with signal, no fix -> poor sky view, needs time or open sky
 *   zero satellites with signal    -> GNSS antenna disconnected, dead, or on the
 *                                     wrong connector (separate from LTE)
 *
 * Buffers are static rather than stack: a multi-constellation GSV sweep runs to
 * well over a kilobyte and the loop task only has 8 KB. */
static void logGnssSatellites() {
    static char gsa[768];
    static char gsv[1536];

    sendATCommandRaw("AT+QGPSGNMEA=\"GSA\"", gsa, sizeof(gsa), 3000);

    static bool dumpedRawGsa = false;
    if (!dumpedRawGsa) {
        dumpedRawGsa = true;
        Serial.printf("[GNSS] raw GSA >>> %s\n", gsa);
    }

    const char* usedSys[32];
    int         usedPrn[32];
    bool        usedSeen[32];
    int         usedN = 0;
    char        hdopStr[16] = {0};

    for (const char* p = gsa;;) {
        const char* g = strstr(p, "GSA,");
        if (!g) break;
        const char* eol = strchr(g, '\r');
        if (!eol) eol = gsa + strlen(gsa);
        const char* body = g + 4;
        size_t blen = (size_t)(eol - body);

        // systemId sits after PDOP/HDOP/VDOP; "1*0C" parses to 1. Absent on
        // older firmware, in which case fall back to the talker.
        int sysId = _nmeaFieldInt(body, blen, 17);
        const char* gsaSys = sysId ? gsaSystemName(sysId)
                                   : ((g >= gsa + 2) ? gnssTalker(*(g - 2), *(g - 1)) : "???");

        // PRN is only unique WITHIN a constellation, so key dedup on (system, PRN).
        for (int f = 2; f <= 13 && usedN < 32; f++) {
            int rawPrn = _nmeaFieldInt(body, blen, f);
            if (rawPrn == 0) continue;
            int prn;
            const char* sys = prnResolve(rawPrn, gsaSys, &prn);
            bool dup = false;
            for (int u = 0; u < usedN; u++) {
                if (usedPrn[u] == prn && strcmp(usedSys[u], sys) == 0) { dup = true; break; }
            }
            if (!dup) { usedSys[usedN] = sys; usedPrn[usedN] = prn; usedSeen[usedN] = false; usedN++; }
        }
        if (hdopStr[0] == '\0') _nmeaField(body, blen, 15, hdopStr, sizeof(hdopStr));
        p = eol;
    }

    sendATCommandRaw("AT+QGPSGNMEA=\"GSV\"", gsv, sizeof(gsv), 2000);
    int  listed = 0, withSignal = 0, bestSnr = 0;
    bool sawSentence = false;

    for (const char* p = gsv;;) {
        const char* g = strstr(p, "GSV,");
        if (!g) break;
        if (!sawSentence) Serial.println("[GNSS] ---- satellites ----");
        sawSentence = true;
        const char* sys = (g >= gsv + 2) ? gnssTalker(*(g - 2), *(g - 1)) : "???";
        const char* eol = strchr(g, '\r');
        if (!eol) eol = gsv + strlen(gsv);
        const char* body = g + 4;
        size_t blen = (size_t)(eol - body);
        p = eol;

        // <numMsgs>,<msgNum>,<inView>[,<prn>,<elev>,<azim>,<snr>] x4
        for (int k = 0; k < 4; k++) {
            int rawPrn = _nmeaFieldInt(body, blen, 3 + 4 * k);
            if (rawPrn == 0) continue;         // padding slot in a partial GSV
            int snr = _nmeaFieldInt(body, blen, 6 + 4 * k);

            int prn;
            const char* realSys = prnResolve(rawPrn, sys, &prn);
            bool inFix = false;
            for (int u = 0; u < usedN; u++) {
                if (usedPrn[u] == prn && strcmp(usedSys[u], realSys) == 0) {
                    inFix = true;
                    usedSeen[u] = true;
                    break;
                }
            }

            Serial.printf("  %-4s PRN %-4d el %-3d az %-4d SNR %-3d %s\n",
                          realSys, prn,
                          _nmeaFieldInt(body, blen, 4 + 4 * k),
                          _nmeaFieldInt(body, blen, 5 + 4 * k),
                          snr, inFix ? "<- IN FIX" : "");
            listed++;
            if (snr > 0) { withSignal++; if (snr > bestSnr) bestSnr = snr; }
        }
    }

    if (!sawSentence) {
        Serial.println("[GNSS] No GSV sentences returned -- receiver is not reporting.");
        return;
    }

    gnssInView     = (uint16_t)listed;
    gnssWithSignal = (uint16_t)withSignal;
    gnssBestSnr    = (uint16_t)bestSnr;

    // Satellites in the solution with no GSV row are real and working — their
    // constellation simply has NMEA sentence output disabled.
    int hidden = 0;
    for (int u = 0; u < usedN; u++) if (!usedSeen[u]) hidden++;

    Serial.printf("[GNSS] %d in view, %d with signal, %d in fix, best SNR %d dBHz",
                  listed, withSignal, usedN, bestSnr);
    if (hdopStr[0]) Serial.printf(", HDOP %s", hdopStr);
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

    static char gpsResp[384];
    sendATCommandRaw("AT+QGPSLOC=2", gpsResp, sizeof(gpsResp), 3000);
    gpsData.gpsFixed = false;

    /* Match the header without assuming the space after the colon, then slice
     * the payload. Anchoring on "+QGPSLOC: " silently yielded an empty string on
     * any firmware that omits the space, which parsed to all zeros. */
    const char* hdr = strstr(gpsResp, "+QGPSLOC:");
    if (hdr) {
        const char* ds = hdr + 9;
        while (*ds == ' ') ds++;
        const char* de = strchr(ds, '\n');
        size_t dlen = de ? (size_t)(de - ds) : strlen(ds);
        while (dlen && (ds[dlen - 1] == '\r' || ds[dlen - 1] == ' ')) dlen--;

        // <UTC>,<lat>,<lon>,<HDOP>,<alt>,<fix>,<COG>,<spkm>,<spkn>,<date>,<nsat>
        int fields = 1;
        for (size_t i = 0; i < dlen; i++) if (ds[i] == ',') fields++;

        if (fields >= 11) {
            char v[24];
            _nmeaField(ds, dlen, 1,  v, sizeof(v)); gpsData.latitude   = atof(v);
            _nmeaField(ds, dlen, 2,  v, sizeof(v)); gpsData.longitude  = atof(v);
            _nmeaField(ds, dlen, 3,  v, sizeof(v)); gpsData.hdop       = atof(v);
            _nmeaField(ds, dlen, 4,  v, sizeof(v)); gpsData.altitude   = atof(v);
            gpsData.fix        = _nmeaFieldInt(ds, dlen, 5);
            _nmeaField(ds, dlen, 7,  v, sizeof(v)); gpsData.speed      = atof(v);
            gpsData.satellites = _nmeaFieldInt(ds, dlen, 10);
            gpsData.gpsFixed   = (gpsData.satellites > 0 && gpsData.fix >= 2);
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
                    logGnssSatellites();
                }
                currentLocation.latitude  = gpsData.latitude;
                currentLocation.longitude = gpsData.longitude;
                strncpy(currentLocation.source, "GPS", sizeof(currentLocation.source) - 1);
                currentLocation.source[sizeof(currentLocation.source) - 1] = '\0';

                unsigned long now = millis();
                if (currentLocation.timestamp > 0 && now >= currentLocation.timestamp) {
                    float timeDiffHours = (now - currentLocation.timestamp) / 3600000.0f;
                    if (gpsData.speed > 2.0f) {
                        float dist = gpsData.speed * timeDiffHours;
                        totalOdometer += dist;
                        tripOdometer  += dist;
                    }
                }
                currentLocation.timestamp = now;
            }
        } else {
            Serial.printf("[GNSS] Short frame (%d/11 fields)\n", fields);
        }
    } else {
        /* No +QGPSLOC header: the modem answered with an error.
         *   516 Not fixed now      -- normal while the receiver is still searching
         *   505 Session not active -- GNSS died or was ended; re-arm next pass
         *   507 Function not enabled / 503 busy -- also worth re-arming
         * Anchored on "ERROR: " so an MQTT downlink payload sharing the buffer
         * and containing "505" cannot drop the session. */
        if (strstr(gpsResp, "ERROR: 505") || strstr(gpsResp, "ERROR: 507")) {
            Serial.println("[GNSS] Session dropped, will re-arm.");
            gpsEnabled    = false;
            lastGnssArmMs = 0;             // re-arm immediately, don't wait 30s
        } else if (!gnssEverFixed) {
            // Verbose only until the first fix lands — this is the window where
            // antenna/sky/TTFF problems actually show up. Quiet afterwards.
            const char* e = strstr(gpsResp, "ERROR:");
            Serial.printf("[GNSS] No fix yet (%s)\n", e ? e + 6 : gpsResp);

            // Every 6th miss (~60 s) report what the receiver can actually see,
            // so a roof overhead is distinguishable from a dead antenna.
            if (++noFixPolls % 6 == 0) logGnssSatellites();
        }
    }

    if (!gpsData.gpsFixed) {
        /* NOTE: AT+QCELLLOC belongs to the BG96/EC25 QuecLocator 1.0 set and is
         * NOT implemented on EC200U -- it answers +CME ERROR here, so this
         * fallback has never produced a coordinate. EC200U needs QuecLocator 2.0
         * (AT+QLBSCFG token/APN setup, then AT+QLBS), which requires a
         * Quectel-issued token. Left in place so the GPS -> LBS path is obvious,
         * but it is a no-op. */
        char lbsResp[128];
        sendATCommandRaw("AT+QCELLLOC=1", lbsResp, sizeof(lbsResp), 5000);
        char* cellStart = strstr(lbsResp, "+QCELLLOC: ");
        if (cellStart) {
            cellStart += 11;
            char* commaPtr = strchr(cellStart, ',');
            if (commaPtr) {
                *commaPtr = '\0';
                currentLocation.longitude = atof(cellStart);
                currentLocation.latitude  = atof(commaPtr + 1);
                strncpy(currentLocation.source, "LBS (approx)", sizeof(currentLocation.source) - 1);
                currentLocation.source[sizeof(currentLocation.source) - 1] = '\0';
                currentLocation.timestamp = millis();
            }
        }
    }
}

// ============ MQTT CONNECT & VERIFY ============

void connectAndVerify() {
    Serial.println("[MQTT] Connecting to AWS IoT Core...");
    sendATCommandRaw("AT+QMTDISC=0", nullptr, 0, 3000);
    sendATCommandRaw("AT+QMTCLOSE=0", nullptr, 0, 5000);
    delay(500);
    
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QMTOPEN=0,\"%s\",%d", AWS_IOT_ENDPOINT, mqtt_port);
    
    // Send QMTOPEN manually because it's an Asynchronous command
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cmd);
    
    char resp[128] = {0};
    size_t idx = 0;
    unsigned long start = millis();
    bool openSuccess = false;
    
    while (millis() - start < 15000) {
        esp_task_wdt_reset(); // Feed Watchdog
        while (SerialAT.available() && idx < sizeof(resp) - 1) {
            resp[idx++] = (char)SerialAT.read();
        }
        if (strstr(resp, "+QMTOPEN: 0,0")) {
            openSuccess = true;
            break;
        }
    }
    
    if (!openSuccess) {
        Serial.println("[ERROR] Failed to open MQTT socket to AWS!");
        mqttConnected = false;
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"%s\"", THING_NAME);
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cmd);
    
    memset(resp, 0, sizeof(resp));
    idx = 0;
    start = millis();
    bool connSuccess = false;
    
    while (millis() - start < 15000) {
        esp_task_wdt_reset(); // Feed Watchdog
        while (SerialAT.available() && idx < sizeof(resp) - 1) {
            resp[idx++] = (char)SerialAT.read();
        }
        if (strstr(resp, "+QMTCONN: 0,0,0")) {
            connSuccess = true;
            break;
        }
    }
    
    if (connSuccess) {
        mqttConnected = true;
        Serial.println("[SUCCESS] AWS mTLS Connected!");

        /* Cloud contact confirmed: resets the silence timer, ends probation for
         * a freshly OTA'd image, and cancels the bootloader's pending-verify.
         * This is the single point that proves a new image actually works. */
        healthOnCloudSuccess();
        
        snprintf(cmd, sizeof(cmd), "AT+QMTSUB=0,1,\"$aws/things/%s/jobs/notify-next\",1", THING_NAME);
        sendATCommandRaw(cmd, nullptr, 0, 10000);
    } else {
        Serial.println("[ERROR] AWS MQTT Auth Failed!");
        mqttConnected = false;
    }
}

void checkIncoming() {
    if (!SerialAT.available()) return;
    
    // AWS OTA Job payloads with Pre-signed URLs are huge. Increased buffer to 1536 bytes.
    char buf[1536]; 
    size_t idx = 0;
    unsigned long start = millis();
    
    // Increased timeout and dynamic reset to ensure the full payload is received
    while (millis() - start < 500) { 
        while (SerialAT.available() && idx < sizeof(buf) - 1) {
            buf[idx++] = (char)SerialAT.read();
            start = millis(); // Reset timer when new data arrives to catch the whole chunk
        }
    }
    buf[idx] = '\0';
    if (idx == 0) return;
    
    // Debug print so you can actually SEE the job arriving!
    Serial.println("\n[DEBUG] Incoming MQTT Message Received:");
    Serial.println(buf);

    /* Free ride: this buffer was read anyway, so scan it for the SMS "+CMTI:"
     * indication rather than having smsPoll() spend AT commands looking for one.
     * Before the OTA early-return below, or an OTA downlink would mask it. */
    smsNoteUrc(buf);

    /* Returning here only exits checkIncoming(); loop() must also bail out before
     * touching SerialAT again, which it does by testing otaPending(). */
    if (otaCheckDownlink(buf)) return;

    if (strstr(buf, "+QMTSTAT:")) {
        mqttConnected = false;
    }
}

// ============ SETUP ============

void setup() {
    Serial.begin(115200);

    /* First real statement in setup(). Starts the hang-watchdog task (which
     * stays DISARMED until loop() runs once, so the long blocking modem bring-up
     * below is never mistaken for a hang) and evaluates boot-health probation
     * for an image that arrived via OTA. */
    healthBegin();

    // ⚠️ CRITICAL FIX: MUST be called BEFORE SerialAT.begin() or modem.begin()
    SerialAT.setRxBufferSize(8192);
    
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
    pinMode(MQ2_PIN, INPUT); pinMode(MQ8_PIN, INPUT);

    Serial.println("\n=== POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.3) ===");
    
    // Rollback confirmation is deferred to otaBootReport() below — the image
    // is marked valid only AFTER connectivity self-test passes, and a pending
    // OTA job is reported SUCCEEDED at that point.

    Wire.begin(21, 22);
    if (!bme.begin(0x77)) {
        bmeAvailable = false;
    } else {
        bmeAvailable = true;
    }

    // Now it's safe to start the modem — the UART RX ring is already 8192 bytes.
    if (!modem.begin()) {
        Serial.println("[ERROR] Modem init failed!");
    }
    
    bool netOk = false;
    for (int i = 0; i < 15; i++) {
        if (modem.waitForNetwork(1000)) { netOk = true; break; }
        Serial.print(".");
    }
    
    modem.attachData(apn.c_str());
    
    sendATCommandRaw("AT+QIACT=1", nullptr, 0, 10000);
    sendATCommandRaw("AT+QMTCFG=\"pdpcid\",0,1", nullptr, 0, 2000);
    sendATCommandRaw("AT+QMTCFG=\"ssl\",0,1,0", nullptr, 0, 2000);
    sendATCommandRaw("AT+QSSLCFG=\"sslversion\",0,4", nullptr, 0, 2000);
    sendATCommandRaw("AT+QSSLCFG=\"seclevel\",0,2", nullptr, 0, 2000);
    sendATCommandRaw("AT+QSSLCFG=\"cacert\",0,\"UFS:rootCA.pem\"", nullptr, 0, 2000);
    sendATCommandRaw("AT+QSSLCFG=\"clientcert\",0,\"UFS:cert.pem\"", nullptr, 0, 2000);
    sendATCommandRaw("AT+QSSLCFG=\"clientkey\",0,\"UFS:privkey.pem\"", nullptr, 0, 2000);

    /* GNSS is NOT armed here. ensureGnssOn() owns arming and retries it from the
     * loop — a one-shot arm in setup() latched gpsEnabled=false forever whenever
     * the modem answered 504/503, silently killing GPS for the life of the image. */

    /* --- CAN INIT: auto-negotiate the BMS bus rate ---
     * Probe 500 kbps first, fall back to 250 kbps. A pack that answers 0x100
     * proves both the wiring and the rate, so nothing downstream has to guess.
     * readBMS() is a no-op until baudRateLocked, which keeps a missing harness
     * from costing a blocking sweep every cycle. */
    Serial.println("[CAN] Probing BMS bus at 500kbps...");
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
    connectAndVerify();

    // Post-boot: confirm image health + report SUCCEEDED for a completed job.
    if (mqttConnected) otaBootReport();
}

// ============ MAIN LOOP ============

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
    /* Heartbeat + cloud-silence supervisor. First thing every iteration, before
     * any early return, so a device stuck in the reconnect branch below still
     * escalates to a modem reset and then a reboot. */
    healthTick();

    if (otaPending()) {
        // otaRun() blocks for minutes and deliberately drops MQTT. Tell both
        // supervisors this is deliberate work, not a stall or a cloud outage.
        healthNoteOtaActivity();
        otaRun();
        return;
    }

    /* Deliberately ABOVE the mqttConnected gate. The entire value of this channel
     * is reaching a device whose cloud path is broken, and that branch returns
     * early — putting the poll below it would make SMS work only when it is least
     * needed. Below the OTA guard, though: otaRun() owns the AT bus.
     *
     * Hand it the size of the idle gap so a queued command waits for a quiet
     * moment instead of elbowing telemetry aside. It relaxes the threshold on its
     * own once a command has waited too long, so the wait stays bounded. */
    smsPoll(loopSlackMs());

    if (!mqttConnected) {
        if (millis() - lastReconnectAttempt > reconnectCooldown) {
            lastReconnectAttempt = millis();
            healthCountReconnect();
            connectAndVerify();
            if (mqttConnected) {
                otaFlushDeferredReport();  // publish FAILED if an OTA just failed
                otaBootReport();           // confirm health + SUCCEEDED if boot was post-OTA
            }
        }
        return;
    }

    checkIncoming();

    /* A job may have just been queued by checkIncoming(). Nothing below may touch
     * SerialAT before otaRun() has had the bus — otherwise a full telemetry cycle
     * of AT traffic interleaves with the start of the download. */
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
        if (bmeAvailable && bme.performReading()) {
            temp = bme.temperature;
            hum = bme.humidity;
            press = bme.pressure / 100.0F;
            gasRes = bme.gas_resistance / 1000.0F;
        }
        lastBmeTempC = temp;   // 0.0 when absent or the read failed -> P2G#TEST# E_BME

        /* Snapshot health once, so every field in the payload describes the same
         * instant rather than drifting across the snprintf call. */
        HealthSnapshot health = {
            healthUptimeS(), healthResetReason(), healthHeapFree(),
            healthHeapLargest(), healthReconnects(), healthProbation()
        };

        // 2560 B: the bms{} block carries up to 32 cell voltages plus 6 temps.
        static char jsonPayload[2560];
        if (buildTelemetryPayload(jsonPayload, sizeof(jsonPayload), &gpsData, &currentLocation, &deviceInfo,
                                  &bmsData, &health,
                                  rawMQ2, rawMQ8, temp, hum, press, gasRes, totalOdometer)) {
            
            char topicBuf[64];
            snprintf(topicBuf, sizeof(topicBuf), "bms/data/%s/telemetry", THING_NAME);
            
            /* A confirmed publish is the ONLY proof the cloud path is alive end
             * to end. healthTick()'s silence timer is otherwise refreshed only
             * on reconnect, so a device that stays connected and publishes
             * happily would trip the 10-minute modem reset anyway. Feeding the
             * ack back here is what makes the supervisor measure real cloud
             * reachability instead of merely time-since-last-reconnect. */
            if (publishToAWS(SerialAT, topicBuf, jsonPayload)) {
                healthOnCloudSuccess();
                Serial.println("\n[MQTT] Published JSON Payload successfully.");
            } else {
                Serial.println("\n[MQTT] Publish NOT confirmed — payload lost.");
            }
        }
    }
}