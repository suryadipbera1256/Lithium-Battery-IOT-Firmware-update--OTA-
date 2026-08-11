#pragma once
/* =========================================================
 * EC200U_AWS_OTA.h  (COOPERATIVE, ROLLBACK-SAFE EDITION)
 * AWS IoT Jobs (Cloud-Push) OTA for ESP32 + EC200U-CN
 *
 * Model: SINGLE-BUS, IN-LOOP, COOPERATIVE. otaRun() owns the modem AT bus
 *        exclusively while it runs — NO concurrent FreeRTOS task (the shared
 *        SerialAT bus makes multi-core modem access unsafe).
 *
 * Download: QHTTPREADFILE -> modem UFS, then QFREAD fixed chunks -> Update.write.
 *        The ESP32 issues every read, so a flash sector erase only delays the
 *        next request. With raw QHTTPREAD the modem streams at line rate and
 *        cannot be throttled: the UART ring buffer overruns, bytes are lost,
 *        and the read stalls waiting for data that was already dropped.
 *
 * Job reporting is SEQUENCED around the mandatory MQTT teardown:
 *   - IN_PROGRESS  published BEFORE QMTDISC (MQTT still alive).
 *   - SUCCEEDED    persisted to NVS only AFTER Update.end() succeeds, then
 *                  reported once the new image boots and reconnects.
 *   - FAILED       reported after MQTT reconnects on the failure path
 *                  (device stays on the current known-good bank).
 *
 * ROLLBACK: dual-bank slots come from the partition table (app0/app1 + otadata).
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the stock Arduino core, but the
 * core's weak verifyRollbackLater() returns false, so initArduino() marks a new
 * image valid before it has proven anything. main.cpp overrides that stub when
 * OTA_ARM_ROLLBACK is set; otaConfirmHealthy() then cancels pending-verify only
 * after the new image reaches AWS.
 * ========================================================= */

#include <Arduino.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "aws_iot_core.h"   // publishToAWS(HardwareSerial&, topic, payload)
#include "config.h"         // THING_NAME, FW_VERSION
#include "device_health.h"  // healthAlive(), healthMarkFreshImage()

// External globals from main
extern HardwareSerial SerialAT;
extern bool           mqttConnected;

// OTA Tuning Parameters
#define OTA_HTTP_CONTEXT_ID   1
#define OTA_SSL_CTX_ID        1           // ctx 1 = OTA/HTTPS, ctx 2 = MQTT
#define OTA_GET_RSPTIME       120
#define OTA_READFILE_TIMEOUT  120         // seconds for QHTTPREADFILE
#define OTA_CHUNK             2048
#define OTA_IDLE_MS           8000        // inter-byte idle, NOT a transfer deadline
#define OTA_CHUNK_RETRIES     3
#define OTA_MAX_ATTEMPTS      3           // give up on a job after this many tries
#define MIN_OTA_RSSI          10          // minimum CSQ required to start
#define OTA_UFS_FILE          "UFS:as_fw.bin"
#define OTA_NVS_NS            "ota"

/* Publishing to $aws/things/<thing>/jobs/<jobId>/update requires iot:Publish on
 * that topic. AWS IoT closes the connection outright on an unauthorized publish,
 * so this is opt-in rather than assumed.
 *
 * 0 (default): status is logged locally only. The download, flash, reboot and
 *   loop protection all still work — notify-next delivers the job and NVS stops
 *   a re-flash. The cost is that the job execution never reaches a terminal
 *   state in AWS, and a non-terminal execution stays "$next" forever, which
 *   BLOCKS every later job. Delete the execution in the console after each OTA.
 * 1: full IN_PROGRESS / SUCCEEDED / FAILED reporting. Needs the policy grant. */
#ifndef OTA_REPORT_JOB_STATUS
#define OTA_REPORT_JOB_STATUS 1
#endif

// Static buffers for O(1) memory
static bool _otaPending   = false;
static char _otaUrl[1536] = {0};
static char _jobId[64]    = {0};

/* Conditional-OTA gates, populated per-job from the job document by the OTA
 * dashboard. Absent fields fall back to the compile-time defaults, so a job
 * document written before these existed behaves exactly as it always did. */
static int  _gateMinCsq   = MIN_OTA_RSSI;
static int  _gateMinKbps  = 0;
static int  _gateRetries  = 0;

// Deferred FAILED-report state (published after MQTT reconnects)
static bool _otaFailReport     = false;
static char _otaFailReason[48] = {0};

inline bool otaPending() { return _otaPending; }

// ======================= Low-level AT helpers =======================
//
// Every wait loop yields with delay(1). Busy-spinning here starves the idle
// task and trips the task watchdog, and these loops run for up to two minutes.

static void _otaATRaw(const char* cmd, char* outBuffer = nullptr, size_t bufferSize = 0, uint32_t timeoutMs = 5000) {
    char   tempBuf[128] = {0};
    char*  targetBuf  = (outBuffer != nullptr && bufferSize > 0) ? outBuffer : tempBuf;
    size_t targetSize = (outBuffer != nullptr && bufferSize > 0) ? bufferSize : sizeof(tempBuf);

    memset(targetBuf, 0, targetSize);
    while (SerialAT.available()) SerialAT.read();

    SerialAT.println(cmd);
    unsigned long start = millis();
    size_t idx = 0;

    while (millis() - start < timeoutMs) {
        while (SerialAT.available()) {
            if (idx >= targetSize - 1) {
                size_t shift = targetSize / 2;
                memmove(targetBuf, targetBuf + shift, targetSize - shift);
                idx -= shift;
                memset(targetBuf + idx, 0, targetSize - idx);
            }
            targetBuf[idx++] = (char)SerialAT.read();
            targetBuf[idx]   = '\0';
        }
        if (strstr(targetBuf, "OK\r\n") || strstr(targetBuf, "ERROR")) {
            delay(50);
            while (SerialAT.available() && idx < targetSize - 1) {
                targetBuf[idx++] = (char)SerialAT.read();
                targetBuf[idx]   = '\0';
            }
            return;
        }
        delay(1);
    }
}

// Config commands are fire-and-forget, but a silent ERROR here (missing cert
// file, unsupported parameter) shows up much later as an opaque socket failure.
// Stay quiet on success, complain loudly otherwise.
static void _otaCfg(const char* cmd) {
    char resp[128];
    _otaATRaw(cmd, resp, sizeof(resp), 5000);
    if (!strstr(resp, "OK")) {
        Serial.printf("[AWS OTA] WARN: %s -> %s\n", cmd, resp[0] ? resp : "(no response)");
    }
}

static bool _otaWaitForPattern(const char* pattern, uint32_t timeoutMs) {
    unsigned long start = millis();
    char   buf[192] = {0};
    size_t idx = 0;

    while (millis() - start < timeoutMs) {
        while (SerialAT.available()) {
            if (idx >= sizeof(buf) - 1) {
                // Retain the tail so a pattern straddling the shift survives.
                size_t shift = sizeof(buf) / 2;
                memmove(buf, buf + shift, sizeof(buf) - shift);
                idx -= shift;
                memset(buf + idx, 0, sizeof(buf) - idx);
            }
            buf[idx++] = (char)SerialAT.read();
            buf[idx]   = '\0';
            if (strstr(buf, pattern)) return true;
        }
        healthAlive();     // QHTTPREADFILE waits here for up to two minutes
        delay(1);
    }
    return false;
}

// Read one CRLF-terminated line into out. Returns length (0 on timeout).
static size_t _otaReadLine(char* out, size_t cap, uint32_t timeoutMs) {
    size_t n = 0;
    unsigned long start = millis();
    out[0] = '\0';
    while (millis() - start < timeoutMs) {
        while (SerialAT.available()) {
            char c = (char)SerialAT.read();
            if (c == '\n') {
                while (n && (out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
                out[n] = '\0';
                if (n) return n;              // skip empty lines
            } else if (n < cap - 1) {
                out[n++] = c;
                out[n]   = '\0';
            }
        }
        delay(1);
    }
    out[n] = '\0';
    return n;
}

// Read exactly len bytes, bounded by an inter-byte idle timeout so a slow but
// live transfer is never killed. Returns bytes actually read.
static size_t _otaReadFixed(uint8_t* buf, size_t len, uint32_t idleMs) {
    size_t got = 0;
    unsigned long last = millis();
    SerialAT.setTimeout(200);
    while (got < len) {
        int avail = SerialAT.available();
        if (avail > 0) {
            size_t want = len - got;
            if ((size_t)avail < want) want = (size_t)avail;
            size_t r = SerialAT.readBytes(buf + got, want);
            got += r;
            if (r) last = millis();
        } else {
            if (millis() - last > idleMs) break;
            delay(1);
        }
    }
    return got;
}

// ======================= Job status reporting =======================

static void _otaReportStatus(const char* status, const char* detailKey, const char* detailVal) {
    if (_jobId[0] == '\0') return;

#if OTA_REPORT_JOB_STATUS
    char topic[144];
    char payload[192];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/%s/update", THING_NAME, _jobId);
    if (detailKey && detailVal) {
        snprintf(payload, sizeof(payload),
                 "{\"status\":\"%s\",\"statusDetails\":{\"%s\":\"%s\"}}",
                 status, detailKey, detailVal);
    } else {
        snprintf(payload, sizeof(payload), "{\"status\":\"%s\"}", status);
    }
    publishToAWS(SerialAT, topic, payload);
    Serial.printf("[AWS OTA] Job %s -> %s\n", _jobId, status);
#else
    (void)detailKey; (void)detailVal;
    Serial.printf("[AWS OTA] Job %s -> %s (local only; reporting disabled)\n", _jobId, status);
#endif
}

// Persisted ONLY after Update.end() succeeds. Writing it earlier means a power
// cut mid-download makes the next boot report SUCCEEDED for a flash that never
// happened.
static void _otaPersistSuccess() {
    Preferences p;
    if (p.begin(OTA_NVS_NS, false)) {
        p.putString("job", _jobId);
        p.putBool("pend", true);
        p.end();
    }
}

// Attempt bookkeeping so a permanently broken job cannot loop forever.
// isKey() first: getString() on a missing key logs at ERROR level, which makes
// a normal first boot look like a fault.
static int _otaBumpAttempt() {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return 1;
    bool same = p.isKey("tryj") && p.getString("tryj", "") == String(_jobId);
    int  n    = (same ? p.getInt("tryc", 0) : 0) + 1;
    p.putString("tryj", _jobId);
    p.putInt("tryc", n);
    p.end();
    return n;
}

// 0 = runnable, 1 = already flashed, 2 = out of retries
static int _otaJobDisposition(const char* jobId) {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, true)) return 0;
    bool done  = p.isKey("done") && p.getString("done", "") == String(jobId);
    bool same  = p.isKey("tryj") && p.getString("tryj", "") == String(jobId);
    int  tries = same ? p.getInt("tryc", 0) : 0;
    p.end();
    if (done) return 1;
    return (tries >= OTA_MAX_ATTEMPTS) ? 2 : 0;
}

// ======================= Network / rollback =======================

static bool _isNetworkStable() {
    char csqResp[64];
    _otaATRaw("AT+CSQ", csqResp, sizeof(csqResp), 3000);
    char* csqPtr = strstr(csqResp, "+CSQ: ");
    if (!csqPtr) {
        Serial.println("[AWS OTA] Gate: no CSQ response from modem.");
        return false;
    }

    int rssi = atoi(csqPtr + 6);
    Serial.printf("[AWS OTA] CSQ=%d (job requires >=%d)\n", rssi, _gateMinCsq);
    if (rssi == 99 || rssi < _gateMinCsq) {
        Serial.printf("[AWS OTA] Gate: CSQ %d < required %d\n", rssi, _gateMinCsq);
        return false;
    }

    if (_gateMinKbps > 0) {
        /* The EC200U cannot measure real throughput before the transfer starts,
         * so gate on the radio access technology instead — a conservative proxy
         * for the floor a given RAT can sustain. An LTE device on a congested
         * cell still passes: this answers "is the radio fast enough", not "is
         * there bandwidth available right now". */
        char nwResp[96];
        _otaATRaw("AT+QNWINFO", nwResp, sizeof(nwResp), 3000);
        int floorKbps = strstr(nwResp, "LTE")   ? 2000
                      : strstr(nwResp, "HSPA")  ? 384
                      : strstr(nwResp, "WCDMA") ? 128
                      : strstr(nwResp, "EDGE")  ? 128
                      : strstr(nwResp, "GPRS")  ? 40
                      : 0;
        if (floorKbps < _gateMinKbps) {
            Serial.printf("[AWS OTA] Gate: RAT floor %d kbps < required %d\n",
                          floorKbps, _gateMinKbps);
            return false;
        }
    }
    return true;
}

/**
 * Cancel the pending-verify state once the running image is healthy.
 * Safe no-op if the image is not in PENDING_VERIFY (normal non-OTA boot, or
 * rollback not armed).
 */
inline void otaConfirmHealthy() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("[OTA] New image confirmed healthy; rollback cancelled.");
    }
}

// ======================= Downlink parsing =======================

// Copy from src until any terminator, never past the end of the source string.
static size_t _otaCopyUntil(char* dst, size_t cap, const char* src, const char* stops) {
    size_t n = 0;
    while (src[n] != '\0' && strchr(stops, src[n]) == nullptr && n < cap - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}

/* Reads <key>:<int> out of the raw URC line, where key is the BARE field name.
 *
 * Deliberately not anchored on a leading quote. Whether AT+QMTRECV delivers the
 * payload with literal quotes or backslash-escaped ones is firmware-dependent,
 * and searching for "\"minCsq\"" silently misses the escaped form — which would
 * fall back to MIN_OTA_RSSI and quietly bypass an operator-set threshold. A
 * safety gate must not fail open on a formatting difference, so the separators
 * between key and value are skipped rather than assumed.
 *
 * Returns the fallback when the key is absent or malformed, which is what keeps
 * legacy two-field job documents working unchanged. */
static int _otaParseInt(const char* line, const char* key, int fallback) {
    const char* p = strstr(line, key);
    if (!p) return fallback;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '"' || *p == '\\') p++;
    return (*p >= '0' && *p <= '9') ? atoi(p) : fallback;
}

inline bool otaCheckDownlink(const char* urcLine) {
    if (_otaPending) return true;
    if (!strstr(urcLine, "+QMTRECV:")) return false;

    // Two sources carry a job document: the notify-next push, and the reply to
    // our own $next/get request. A "no pending job" reply has no jobId, so the
    // extraction below rejects it on its own.
    if (!strstr(urcLine, "jobs/notify-next") &&
        !strstr(urcLine, "jobs/$next/get/accepted")) return false;

    const char* urlStart = strstr(urcLine, "https://");
    if (!urlStart) return false;
    _otaCopyUntil(_otaUrl, sizeof(_otaUrl), urlStart, "\"\\ \r\n},");

    const char* jobStart = strstr(urcLine, "\"jobId\":\"");
    _jobId[0] = '\0';
    if (jobStart) _otaCopyUntil(_jobId, sizeof(_jobId), jobStart + 9, "\"");

    if (strlen(_otaUrl) < 10 || _jobId[0] == '\0') return false;

    /* Per-job network-readiness gates injected by the OTA dashboard. Parsed here
     * rather than in otaRun() because the URC line is the only place these
     * fields exist — otaRun() runs after the buffer is gone. */
    _gateMinCsq  = _otaParseInt(urcLine, "minCsq",     MIN_OTA_RSSI);
    _gateMinKbps = _otaParseInt(urcLine, "minKbps",    0);
    _gateRetries = _otaParseInt(urcLine, "maxRetries", 0);
    Serial.printf("[AWS OTA] Gates: minCsq=%d minKbps=%d maxRetries=%d\n",
                  _gateMinCsq, _gateMinKbps, _gateRetries);

    // Re-acknowledge instead of re-flashing, otherwise notify-next keeps
    // handing us the same job and the device loops on it forever.
    int disp = _otaJobDisposition(_jobId);
    if (disp != 0) {
        Serial.printf("[AWS OTA] Ignoring job %s (%s).\n", _jobId,
                      disp == 1 ? "already flashed" : "out of retries");
        _otaReportStatus(disp == 1 ? "SUCCEEDED" : "FAILED", "detail",
                         disp == 1 ? "already_flashed" : "retries_exhausted");
        _jobId[0] = '\0';
        return true;
    }

    _otaPending = true;
    Serial.printf("\n[AWS OTA] Update Requested. Job ID: %s (attempt %d/%d, url %u chars)\n",
                  _jobId, _otaBumpAttempt(), OTA_MAX_ATTEMPTS, (unsigned)strlen(_otaUrl));
    return true;
}

// Quectel HTTP error codes (see the Quectel HTTP AT commands manual).
static const char* _otaHttpErrName(int code) {
    switch (code) {
        case 701: return "unknown error";
        case 702: return "timeout";
        case 703: return "busy";
        case 704: return "UART busy";
        case 705: return "no GET/POST request";
        case 706: return "network busy";
        case 707: return "network open failed";
        case 708: return "no network config";
        case 709: return "network deactivated (PDP context down)";
        case 710: return "network error";
        case 711: return "URL error";
        case 712: return "empty URL";
        case 713: return "IP address error";
        case 714: return "DNS error";
        case 715: return "socket create error";
        case 716: return "socket connect error (TCP/TLS to the host failed)";
        case 717: return "socket read error";
        case 718: return "socket write error";
        case 719: return "socket closed";
        case 720: return "data encode error";
        case 721: return "data decode error";
        case 722: return "read timeout";
        case 723: return "response failed";
        case 729: return "memory allocation failed";
        case 730: return "invalid parameter";
        default:  return "see Quectel HTTP AT manual";
    }
}

/* Returns >0 = Content-Length, 0 = HTTP 200 with no usable length (the staged
 * file size becomes authoritative), -1 = failure. */
static long _otaParseGetLen(const char* httpGetResp) {
    const char* p = strstr(httpGetResp, "+QHTTPGET:");
    if (!p) { Serial.println("[AWS OTA] ERROR: no +QHTTPGET response"); return -1; }

    int  err = -1, code = -1;
    long len = -1;
    int  n = sscanf(p, "+QHTTPGET: %d,%d,%ld", &err, &code, &len);

    // A single field is a modem-side failure: the request never reached HTTP.
    if (n == 1) {
        Serial.printf("[AWS OTA] ERROR: modem HTTP error %d — %s\n", err, _otaHttpErrName(err));
        return -1;
    }
    if (n < 2 || err != 0) {
        Serial.printf("[AWS OTA] ERROR: QHTTPGET err=%d code=%d\n", err, code);
        return -1;
    }
    if (code != 200) {
        Serial.printf("[AWS OTA] ERROR: S3 returned HTTP %d%s\n", code,
                      code == 403 ? " (pre-signed URL expired, or the job role lacks s3:GetObject)"
                                  : "");
        return -1;
    }
    return (len > 0) ? len : 0;
}

// Parse "+QFLDS: <free>,<total>"
static long _otaParseFreeSpace(const char* s) {
    const char* p = strstr(s, "+QFLDS:");
    return p ? atol(p + 7) : -1;
}

// Parse "+QFLST: "UFS:as_fw.bin",<size>"
static long _otaParseFileSize(const char* s) {
    const char* p = strstr(s, "+QFLST:");
    if (!p) return -1;
    const char* c = strchr(p, ',');
    return c ? atol(c + 1) : -1;
}

// Failure handler: abort flash, drop the staged file, stash the reason, force
// an MQTT reconnect so FAILED can be published (see otaFlushDeferredReport()).
static void _otaFail(const char* reason, int handle = -1) {
    if (Update.isRunning()) Update.abort();

    char cmd[64];
    if (handle >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d", handle);
        _otaATRaw(cmd, nullptr, 0, 3000);
    }
    snprintf(cmd, sizeof(cmd), "AT+QFDEL=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmd, nullptr, 0, 3000);

    Serial.printf("[AWS OTA] FAILED (%s) — staying on current bank.\n", reason);
    strncpy(_otaFailReason, reason, sizeof(_otaFailReason) - 1);
    _otaFailReason[sizeof(_otaFailReason) - 1] = '\0';
    _otaFailReport = true;
    mqttConnected  = false;   // main loop reconnects, then flushes the report
    delay(2000);              // let the modem release DNS/HTTP before QMTOPEN
}

// ======================= Main cooperative OTA runner =======================

inline void otaRun() {
    if (!_otaPending) return;
    _otaPending = false;

    Serial.println("\n===== AWS OTA START =====");

    // Step 0: IN_PROGRESS while MQTT is STILL connected.
    _otaReportStatus("IN_PROGRESS", nullptr, nullptr);

    Serial.println("[AWS OTA] Step 1: Checking network stability...");
    if (!_isNetworkStable()) { _otaFail("weak_signal"); return; }

    Serial.println("[AWS OTA] Step 2: Safe MQTT disconnect...");
    _otaATRaw("AT+QMTDISC=0",  nullptr, 0, 3000);
    _otaATRaw("AT+QMTCLOSE=0", nullptr, 0, 12000);
    delay(500);

    // A socket-connect failure later is usually a dead PDP context, so confirm
    // it survived the MQTT teardown.
    char actResp[128];
    _otaATRaw("AT+QIACT?", actResp, sizeof(actResp), 5000);
    Serial.printf("[AWS OTA] PDP state: %s\n", actResp);

    Serial.println("[AWS OTA] Step 3: Configuring HTTP & SSL engine...");
    char cmdBuf[96];
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPCFG=\"contextid\",%d", OTA_HTTP_CONTEXT_ID);
    _otaCfg(cmdBuf);
    _otaCfg("AT+QHTTPCFG=\"requestheader\",0");
    _otaCfg("AT+QHTTPCFG=\"responseheader\",0");

    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPCFG=\"sslctxid\",%d", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"sslversion\",%d,4", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"ciphersuite\",%d,0xFFFF", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    // seclevel 1 = verify the server. NOT 0: this is the firmware-update
    // channel, and an unverified peer can serve arbitrary firmware.
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"seclevel\",%d,1", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"cacert\",%d,\"UFS:rootCA.pem\"", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    // SNI is mandatory: without it a virtual-hosted-style bucket host gets the
    // wrong certificate and the handshake fails.
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"sni\",%d,1", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"ignorelocaltime\",%d,1", OTA_SSL_CTX_ID);
    _otaCfg(cmdBuf);

    Serial.println("[AWS OTA] Step 4: Submitting S3 pre-signed URL...");
    Serial.printf("[AWS OTA] URL (%u chars): %s\n", (unsigned)strlen(_otaUrl), _otaUrl);
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPURL=%u,30", (unsigned)strlen(_otaUrl));
    SerialAT.println(cmdBuf);
    if (!_otaWaitForPattern("CONNECT", 5000)) { _otaFail("qhttpurl_connect"); return; }
    SerialAT.print(_otaUrl);
    if (!_otaWaitForPattern("OK", 5000))      { _otaFail("qhttpurl_ok");      return; }

    Serial.println("[AWS OTA] Step 5: Requesting firmware (GET)...");
    char getResp[160] = {0};
    _otaATRaw(("AT+QHTTPGET=" + String(OTA_GET_RSPTIME)).c_str(), nullptr, 0, 3000);
    {
        unsigned long startWait = millis();
        size_t idx = 0;
        while (millis() - startWait < (uint32_t)OTA_GET_RSPTIME * 1000UL) {
            while (SerialAT.available()) {
                if (idx >= sizeof(getResp) - 1) {
                    size_t shift = sizeof(getResp) / 2;
                    memmove(getResp, getResp + shift, sizeof(getResp) - shift);
                    idx -= shift;
                    memset(getResp + idx, 0, sizeof(getResp) - idx);
                }
                getResp[idx++] = (char)SerialAT.read();
                getResp[idx]   = '\0';
            }
            char* p = strstr(getResp, "+QHTTPGET:");
            if (p && strchr(p, '\n')) break;
            delay(1);
        }
    }
    Serial.printf("[DEBUG] Raw GET response: %s\n", getResp);

    long len = _otaParseGetLen(getResp);
    if (len < 0) { _otaFail("get_len"); return; }
    if (len == 0) Serial.println("[AWS OTA] HTTP 200, no Content-Length — using staged size");
    else          Serial.printf("[AWS OTA] Firmware size: %ld bytes\n", len);

    // Step 6: stage the body into modem UFS. Check there is room first.
    Serial.println("[AWS OTA] Step 6: Staging firmware to modem UFS...");
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFDEL=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmdBuf, nullptr, 0, 3000);          // ignore error if absent

    char ldsResp[96];
    _otaATRaw("AT+QFLDS=\"UFS\"", ldsResp, sizeof(ldsResp), 5000);
    long freeSpace = _otaParseFreeSpace(ldsResp);
    if (freeSpace >= 0 && freeSpace < len + 4096) {
        Serial.printf("[AWS OTA] ERROR: UFS has %ld free, needs %ld\n", freeSpace, len);
        _otaFail("ufs_full"); return;
    }
    Serial.printf("[AWS OTA] UFS free: %ld bytes\n", freeSpace);

    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPREADFILE=\"%s\",%d", OTA_UFS_FILE, OTA_READFILE_TIMEOUT);
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cmdBuf);
    if (!_otaWaitForPattern("+QHTTPREADFILE: 0", (uint32_t)(OTA_READFILE_TIMEOUT + 30) * 1000UL)) {
        _otaFail("readfile"); return;
    }

    // The staged size must match Content-Length. A short file means a truncated
    // download, and flashing it would brick the device.
    char lstResp[96];
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFLST=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmdBuf, lstResp, sizeof(lstResp), 5000);
    long storedLen = _otaParseFileSize(lstResp);
    if (len > 0 && storedLen > 0 && storedLen != len) {
        Serial.printf("[AWS OTA] ERROR: staged %ld != expected %ld\n", storedLen, len);
        _otaFail("size_mismatch"); return;
    }
    if (len == 0) {
        if (storedLen <= 0) {
            Serial.println("[AWS OTA] ERROR: no Content-Length and QFLST gave no size");
            _otaFail("no_length"); return;
        }
        len = storedLen;   // staged size is authoritative
    }
    Serial.printf("[AWS OTA] Staged OK, size %ld (flashing %ld)\n", storedLen, len);

    Serial.println("[AWS OTA] Step 7: Initializing ESP32 dual-bank OTA...");
    if (!Update.begin((size_t)len, U_FLASH)) {
        Serial.printf("[AWS OTA] ERROR: Update.begin failed: %s\n", Update.errorString());
        _otaFail("update_begin"); return;
    }

    // Step 8: open the staged file and pull it out in fixed chunks.
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFOPEN=\"%s\",0", OTA_UFS_FILE);
    char openResp[96];
    _otaATRaw(cmdBuf, openResp, sizeof(openResp), 5000);
    char* hp = strstr(openResp, "+QFOPEN:");
    int handle = hp ? atoi(hp + 8) : -1;
    if (!hp || handle < 0) {
        Serial.printf("[AWS OTA] ERROR: QFOPEN failed: %s\n", openResp);
        _otaFail("qfopen"); return;
    }
    Serial.printf("[AWS OTA] File handle: %d\n", handle);

    Serial.println("[AWS OTA] Step 8: Downloading & writing chunks...");
    static uint8_t buf[OTA_CHUNK];
    long got = 0;
    int  nextMark = 10;
    bool firstBytes = true;
    char line[96];

    while (got < len) {
        size_t want = ((len - got) > OTA_CHUNK) ? OTA_CHUNK : (size_t)(len - got);
        size_t r    = 0;
        bool   ok   = false;

        for (int attempt = 0; attempt < OTA_CHUNK_RETRIES && !ok; attempt++) {
            // A retry must re-read the SAME offset. Without the seek the file
            // pointer stays where the failed read left it and the image
            // silently loses a chunk.
            if (attempt > 0) {
                snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFSEEK=%d,%ld,0", handle, got);
                _otaATRaw(cmdBuf, nullptr, 0, 3000);
                delay(100);
            }

            while (SerialAT.available()) SerialAT.read();
            SerialAT.printf("AT+QFREAD=%d,%u\r\n", handle, (unsigned)want);

            // Expect "CONNECT <len>", then raw bytes, then OK. ATE1 echo is on,
            // so the echoed command arrives first — skip lines until CONNECT.
            bool gotHdr = false;
            unsigned long hStart = millis();
            while (millis() - hStart < OTA_IDLE_MS) {
                if (!_otaReadLine(line, sizeof(line), OTA_IDLE_MS)) break;
                if (strstr(line, "CONNECT")) { gotHdr = true; break; }
                if (strstr(line, "ERROR"))   break;
            }
            if (!gotHdr) {
                Serial.printf("\n[AWS OTA] WARN: QFREAD header failed at %ld: %s\n", got, line);
                continue;
            }

            char*  sp     = strchr(line, ' ');
            size_t toRead = sp ? (size_t)atoi(sp + 1) : want;
            if (toRead == 0 || toRead > want) toRead = want;

            r = _otaReadFixed(buf, toRead, OTA_IDLE_MS);
            if (r != toRead) {
                Serial.printf("\n[AWS OTA] WARN: short read %u/%u at %ld\n",
                              (unsigned)r, (unsigned)toRead, got);
                continue;
            }

            _otaWaitForPattern("OK", 3000);   // consume the QFREAD trailer
            ok = true;
        }

        if (!ok) { Serial.printf("\n[AWS OTA] ERROR: gave up at %ld\n", got);
                   _otaFail("qfread", handle); return; }

        // The first bytes must carry the ESP32 image magic. Anything else means
        // we downloaded an error page, not firmware.
        if (firstBytes) {
            firstBytes = false;
            Serial.printf("[DEBUG] First 4 bytes: %02X %02X %02X %02X\n",
                          buf[0], buf[1], buf[2], buf[3]);
            if (buf[0] != 0xE9) {
                Serial.printf("[AWS OTA] ERROR: not an ESP32 image (magic 0x%02X)\n", buf[0]);
                _otaFail("bad_magic", handle); return;
            }
        }

        if (Update.write(buf, r) != r) {
            Serial.printf("\n[AWS OTA] ERROR: Update.write failed: %s\n", Update.errorString());
            _otaFail("flash_write", handle); return;
        }
        got += (long)r;
        healthNoteOtaActivity();   // download is progress, not a stall

        int pct = (int)((got * 100) / len);
        if (pct >= nextMark) {
            Serial.printf("[AWS OTA] %ld/%ld (%d%%)\n", got, len, pct);
            nextMark = pct - (pct % 10) + 10;
        }
    }
    Serial.println("[AWS OTA] Download complete.");

    // Step 9: close + delete the staged file, finalize flash.
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFCLOSE=%d", handle);
    _otaATRaw(cmdBuf, nullptr, 0, 3000);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFDEL=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmdBuf, nullptr, 0, 3000);

    Serial.println("[AWS OTA] Step 9: Finalizing...");
    if (!Update.end(true) || !Update.isFinished()) {
        Serial.printf("[AWS OTA] ERROR: finalize failed: %s\n", Update.errorString());
        _otaFail("finalize"); return;
    }

    // Only now is the flash real, so only now is SUCCEEDED persisted. It is
    // published after the NEW image boots and reconnects (otaBootReport).
    _otaPersistSuccess();

    // Put the incoming image on probation: if it cannot reach AWS within a few
    // boots, healthBegin() makes this bank bootable again.
    healthMarkFreshImage();

    Serial.println("===== AWS OTA FLASH SUCCESSFUL — Rebooting into new bank =====");
    delay(1000);
    ESP.restart();
    while (true) {}
}

// ======================= Post-boot + deferred hooks =======================

/**
 * Call after MQTT connects. Confirms image health and, if this boot completed a
 * pending OTA job, publishes SUCCEEDED and clears the persisted marker.
 */
inline void otaBootReport() {
    otaConfirmHealthy();

    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return;
    if (!p.isKey("pend") || !p.getBool("pend", false)) { p.end(); return; }

    if (p.isKey("job")) p.getString("job", _jobId, sizeof(_jobId));
    p.putBool("pend", false);
    p.remove("job");
    if (_jobId[0]) p.putString("done", _jobId);   // never re-run this job
    p.end();

    if (_jobId[0]) {
        _otaReportStatus("SUCCEEDED", "fw_version", FW_VERSION);
        _jobId[0] = '\0';
    }
}

/**
 * Call from the main loop right after a successful MQTT (re)connect, to flush a
 * pending FAILED status that could not be sent while MQTT was down.
 */
inline void otaFlushDeferredReport() {
    if (!_otaFailReport || !mqttConnected) return;
    _otaReportStatus("FAILED", "reason", _otaFailReason);
    _otaFailReport = false;
}
