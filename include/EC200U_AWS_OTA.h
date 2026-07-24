#pragma once
/* =========================================================
 * EC200U_AWS_OTA.h  (COOPERATIVE, ROLLBACK-SAFE EDITION)
 * AWS IoT Jobs (Cloud-Push) OTA for ESP32 + EC200U-CN
 *
 * Model: SINGLE-BUS, IN-LOOP, COOPERATIVE. otaRun() owns the modem AT bus
 *        exclusively while it runs — NO concurrent FreeRTOS task (the shared
 *        SerialAT bus makes multi-core modem access unsafe).
 *
 * Download: QHTTPREADFILE → modem UFS, then QFREAD fixed chunks → Update.write.
 *           This removes real-time UART timing from the cache-disabled flash
 *           window (far more robust than raw QHTTPREAD streaming).
 *
 * Job reporting is SEQUENCED around the mandatory MQTT teardown:
 *   • IN_PROGRESS  — published BEFORE QMTDISC (MQTT still alive).
 *   • SUCCEEDED    — jobId persisted to NVS; reported AFTER the new image
 *                    boots, self-validates, and reconnects (otaBootReport()).
 *                    This is the correct AWS Jobs semantic for rollback.
 *   • FAILED       — reported after MQTT reconnects on the failure path
 *                    (device stays on the current known-good bank).
 *
 * ROLLBACK NOTE: dual-bank slots come from partitions_ota.csv. otaConfirmHealthy()
 * cancels the pending-verify state once the new image is healthy. AUTOMATIC
 * rollback on a crash-loop additionally requires the bootloader to be built
 * with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (Arduino-as-IDF-component). With
 * the stock precompiled Arduino core that flag is off, so otaConfirmHealthy()
 * is a safe no-op there and rollback is manual/re-push — the partition table
 * still gives you two independent banks either way.
 * ========================================================= */

#include <Arduino.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include "aws_iot_core.h"   // publishToAWS(), THING_NAME, FW_VERSION

// External globals from main
extern HardwareSerial SerialAT;
extern bool           mqttConnected;

// OTA Tuning Parameters
#define OTA_HTTP_CONTEXT_ID   1
#define OTA_SSL_CTX_ID        1
#define OTA_GET_RSPTIME       120
#define OTA_READFILE_TIMEOUT  80          // seconds for QHTTPREADFILE
#define OTA_CHUNK             2048
#define MIN_OTA_RSSI          10          // Minimum CSQ signal required for OTA
#define OTA_UFS_FILE          "UFS:as_fw.bin"
#define OTA_NVS_NS            "ota"

// Static Buffers for O(1) Memory
static bool _otaPending   = false;
static char _otaUrl[1536] = {0};
static char _jobId[64]    = {0};

// Deferred FAILED-report state (published after MQTT reconnects)
static bool _otaFailReport     = false;
static char _otaFailReason[48] = {0};

inline bool otaPending() { return _otaPending; }

// ======================= Low-level AT helpers =======================

static void _otaATRaw(const char* cmd, char* outBuffer = nullptr, size_t bufferSize = 0, uint32_t timeoutMs = 5000) {
    char tempBuf[128] = {0};
    char* targetBuf = (outBuffer != nullptr && bufferSize > 0) ? outBuffer : tempBuf;
    size_t targetSize = (outBuffer != nullptr && bufferSize > 0) ? bufferSize : sizeof(tempBuf);

    memset(targetBuf, 0, targetSize);
    while (SerialAT.available()) SerialAT.read(); // flush

    SerialAT.println(cmd);
    unsigned long start = millis();
    size_t idx = 0;

    while (millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        while (SerialAT.available()) {
            if (idx >= targetSize - 1) {
                size_t shift = targetSize / 2;
                memmove(targetBuf, targetBuf + shift, targetSize - shift);
                idx -= shift;
                memset(targetBuf + idx, 0, targetSize - idx);
            }
            targetBuf[idx++] = (char)SerialAT.read();
        }
        if (strstr(targetBuf, "OK\r\n") || strstr(targetBuf, "ERROR\r\n")) {
            delay(50);
            return;
        }
    }
}

static bool _otaWaitForPattern(const char* pattern, uint32_t timeoutMs) {
    unsigned long start = millis();
    char buf[128] = {0};
    size_t idx = 0;

    while (millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        while (SerialAT.available()) {
            if (idx >= sizeof(buf) - 1) {
                size_t shift = sizeof(buf) / 2;
                memmove(buf, buf + shift, sizeof(buf) - shift);
                idx -= shift;
                memset(buf + idx, 0, sizeof(buf) - idx);
            }
            buf[idx++] = (char)SerialAT.read();
            buf[idx] = '\0';
            if (strstr(buf, pattern)) return true;
        }
    }
    return false;
}

// Read one CRLF-terminated line into out. Returns length (0 on timeout).
static size_t _otaReadLine(char* out, size_t cap, uint32_t timeoutMs) {
    size_t n = 0;
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (SerialAT.available()) {
            char c = (char)SerialAT.read();
            if (c == '\n') {
                while (n && (out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
                out[n] = '\0';
                if (n) return n;              // skip empty lines
            } else if (n < cap - 1) {
                out[n++] = c;
            }
        }
        esp_task_wdt_reset();
        delay(1);
    }
    out[n] = '\0';
    return n;
}

// Read exactly len bytes. Returns bytes actually read.
static size_t _otaReadFixed(uint8_t* buf, size_t len, uint32_t timeoutMs) {
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
            if (millis() - last > timeoutMs) break;
            esp_task_wdt_reset();
            delay(1);
        }
    }
    return got;
}

// ======================= Job status reporting =======================
// Uses the existing zero-allocation publishToAWS() as the decoupled MQTT sink.

static void _otaReportStatus(const char* status, const char* detailKey, const char* detailVal) {
    if (_jobId[0] == '\0') return;
    char topic[128];
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
}

static void _otaPersistJob() {
    Preferences p;
    if (p.begin(OTA_NVS_NS, false)) {
        p.putString("job", _jobId);
        p.putBool("pend", true);
        p.end();
    }
}

static void _otaClearPersisted() {
    Preferences p;
    if (p.begin(OTA_NVS_NS, false)) {
        p.putBool("pend", false);
        p.remove("job");
        p.end();
    }
}

// ======================= Network / rollback =======================

static bool _isNetworkStable() {
    char csqResp[64];
    _otaATRaw("AT+CSQ", csqResp, sizeof(csqResp), 3000);
    char* csqPtr = strstr(csqResp, "+CSQ: ");
    if (csqPtr) {
        int rssi = atoi(csqPtr + 6);
        if (rssi >= MIN_OTA_RSSI && rssi != 99) return true;
    }
    return false;
}

/**
 * Cancel the pending-verify state once the running image is healthy.
 * Safe no-op if the image is not in PENDING_VERIFY (e.g. rollback disabled
 * in the bootloader, or a normal non-OTA boot).
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

inline bool otaCheckDownlink(const char* urcLine) {
    if (_otaPending) return true;

    if (!strstr(urcLine, "+QMTRECV:") || !strstr(urcLine, "jobs/notify-next")) {
        return false;
    }

    // Extract Presigned URL safely
    const char* urlStart = strstr(urcLine, "https://");
    if (!urlStart) return false;

    size_t urlLen = 0;
    while (urlStart[urlLen] != '"' && urlStart[urlLen] != '\\' && urlLen < sizeof(_otaUrl) - 1) {
        _otaUrl[urlLen] = urlStart[urlLen];
        urlLen++;
    }
    _otaUrl[urlLen] = '\0';

    // Extract Job ID
    const char* jobStart = strstr(urcLine, "\"jobId\":\"");
    if (jobStart) {
        jobStart += 9;
        size_t jLen = 0;
        while (jobStart[jLen] != '"' && jLen < sizeof(_jobId) - 1) {
            _jobId[jLen] = jobStart[jLen];
            jLen++;
        }
        _jobId[jLen] = '\0';
    }

    if (strlen(_otaUrl) > 10) {
        _otaPending = true;
        Serial.printf("\n[AWS OTA] Update Requested. Job ID: %s\n", _jobId);
        return true;
    }
    return false;
}

static long _otaParseGetLen(const char* httpGetResp) {
    const char* p = strstr(httpGetResp, "+QHTTPGET:");
    if (!p) return -1;

    int err, code;
    long len = -1;
    if (sscanf(p, "+QHTTPGET: %d,%d,%ld", &err, &code, &len) >= 2) {
        if (err == 0 && code == 200) {
            return len;
        } else {
            Serial.printf("\n[AWS OTA] ERROR: S3 Server Rejected! Modem Err: %d, HTTP Code: %d\n", err, code);
            return -1;
        }
    }
    return -1;
}

// Failure handler: abort flash, stash reason, force MQTT reconnect so the
// FAILED status can be published (see otaFlushDeferredReport()).
static void _otaFail(const char* reason) {
    if (Update.isRunning()) Update.abort();
    Serial.printf("[AWS OTA] FAILED (%s) — staying on current bank.\n", reason);
    strncpy(_otaFailReason, reason, sizeof(_otaFailReason) - 1);
    _otaFailReason[sizeof(_otaFailReason) - 1] = '\0';
    _otaFailReport = true;
    _otaClearPersisted();
    mqttConnected = false;   // main loop will reconnect, then flush the report
}

// ======================= Main cooperative OTA runner =======================

inline void otaRun() {
    if (!_otaPending) return;
    _otaPending = false;

    Serial.println("\n===== AWS OTA START =====");

    // Step 0: report IN_PROGRESS while MQTT is STILL connected, and persist
    // the jobId so SUCCEEDED can be reported after the new image boots.
    _otaReportStatus("IN_PROGRESS", nullptr, nullptr);
    _otaPersistJob();

    Serial.println("[AWS OTA] Step 1: Checking Network Stability...");
    if (!_isNetworkStable()) {
        _otaFail("weak_signal");
        return;
    }

    Serial.println("[AWS OTA] Step 2: Safe MQTT Disconnect...");
    _otaATRaw("AT+QMTDISC=0", nullptr, 0, 3000);
    _otaATRaw("AT+QMTCLOSE=0", nullptr, 0, 5000);
    delay(500);

    Serial.println("[AWS OTA] Step 3: Configuring HTTP & SSL Engine...");
    char cmdBuf[96];
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPCFG=\"contextid\",%d", OTA_HTTP_CONTEXT_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    _otaATRaw("AT+QHTTPCFG=\"requestheader\",0", nullptr, 0);
    _otaATRaw("AT+QHTTPCFG=\"responseheader\",0", nullptr, 0);

    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPCFG=\"sslctxid\",%d", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"sslversion\",%d,4", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"ciphersuite\",%d,0xFFFF", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"seclevel\",%d,0", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"sni\",%d,1", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"ignorertctime\",%d,1", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);

    Serial.println("[AWS OTA] Step 4: Submitting S3 Pre-signed URL...");
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPURL=%u,30", (unsigned)strlen(_otaUrl));
    SerialAT.println(cmdBuf);
    if (!_otaWaitForPattern("CONNECT", 5000)) { _otaFail("qhttpurl_connect"); return; }
    SerialAT.print(_otaUrl);
    if (!_otaWaitForPattern("OK", 5000)) { _otaFail("qhttpurl_ok"); return; }

    Serial.println("[AWS OTA] Step 5: Requesting Firmware File (GET)...");
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPGET=%d", OTA_GET_RSPTIME);
    SerialAT.println(cmdBuf);

    char getResp[128] = {0};
    unsigned long startWait = millis();
    size_t idx = 0;
    while (millis() - startWait < (uint32_t)OTA_GET_RSPTIME * 1000UL) {
        esp_task_wdt_reset();
        delay(1);
        while (SerialAT.available()) {
            if (idx >= sizeof(getResp) - 1) {
                size_t shift = sizeof(getResp) / 2;
                memmove(getResp, getResp + shift, sizeof(getResp) - shift);
                idx -= shift;
                memset(getResp + idx, 0, sizeof(getResp) - idx);
            }
            getResp[idx++] = (char)SerialAT.read();
            getResp[idx] = '\0';
        }
        char* p = strstr(getResp, "+QHTTPGET:");
        if (p && strchr(p, '\n')) break;
    }
    Serial.print("[DEBUG] Raw GET Response: ");
    Serial.println(getResp);

    long len = _otaParseGetLen(getResp);
    if (len <= 0) { _otaFail("get_len"); return; }
    Serial.printf("[AWS OTA] Firmware Size: %ld Bytes.\n", len);

    // Step 6: Stage the HTTP body into modem UFS (decouples UART timing).
    Serial.println("[AWS OTA] Step 6: Staging firmware to modem UFS...");
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFDEL=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmdBuf, nullptr, 0, 3000);   // ignore error if not present
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPREADFILE=\"%s\",%d", OTA_UFS_FILE, OTA_READFILE_TIMEOUT);
    SerialAT.println(cmdBuf);
    if (!_otaWaitForPattern("+QHTTPREADFILE: 0", (uint32_t)OTA_READFILE_TIMEOUT * 1000UL)) {
        _otaFail("readfile");
        return;
    }

    Serial.println("[AWS OTA] Step 7: Initializing ESP32 dual-bank OTA...");
    if (!Update.begin((size_t)len, U_FLASH)) {
        Serial.printf("[AWS OTA] ERROR: Update.begin failed: %s\n", Update.errorString());
        _otaFail("update_begin");
        return;
    }

    // Step 8: Open UFS file and read fixed chunks into flash.
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFOPEN=\"%s\",0", OTA_UFS_FILE);
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cmdBuf);
    int handle = -1;
    char line[80];
    unsigned long hStart = millis();
    while (millis() - hStart < 5000) {
        if (_otaReadLine(line, sizeof(line), 5000)) {
            char* p = strstr(line, "+QFOPEN:");
            if (p) handle = atoi(p + 8);
            if (strstr(line, "OK")) break;
            if (strstr(line, "ERROR")) { handle = -1; break; }
        }
    }
    if (handle < 0) { _otaFail("qfopen"); return; }

    Serial.println("[AWS OTA] Step 8: Downloading & writing chunks...");
    static uint8_t buf[OTA_CHUNK];
    long got = 0, lastDot = 0;

    while (got < len) {
        size_t want = ((len - got) > OTA_CHUNK) ? OTA_CHUNK : (size_t)(len - got);

        while (SerialAT.available()) SerialAT.read();
        SerialAT.printf("AT+QFREAD=%d,%u\r\n", handle, (unsigned)want);

        // Expect "CONNECT <len>" header, then <len> raw bytes, then OK.
        if (!_otaReadLine(line, sizeof(line), 5000) || !strstr(line, "CONNECT")) {
            SerialAT.printf("AT+QFCLOSE=%d\r\n", handle);
            _otaFail("qfread_hdr");
            return;
        }
        char* sp = strchr(line, ' ');
        size_t toRead = sp ? (size_t)atoi(sp + 1) : want;
        if (toRead == 0 || toRead > want) toRead = want;

        size_t r = _otaReadFixed(buf, toRead, 8000);
        if (r != toRead) {
            SerialAT.printf("AT+QFCLOSE=%d\r\n", handle);
            _otaFail("qfread_short");
            return;
        }
        if (Update.write(buf, r) != r) {
            SerialAT.printf("AT+QFCLOSE=%d\r\n", handle);
            _otaFail("flash_write");
            return;
        }
        got += (long)r;

        _otaWaitForPattern("OK", 2000);   // consume QFREAD trailer
        esp_task_wdt_reset();

        if (got - lastDot >= 10240) { Serial.print("."); lastDot = got; }
    }
    Serial.println("\n[AWS OTA] Download complete.");

    // Step 9: Close + delete staged file, finalize flash.
    SerialAT.printf("AT+QFCLOSE=%d\r\n", handle);
    _otaWaitForPattern("OK", 3000);
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QFDEL=\"%s\"", OTA_UFS_FILE);
    _otaATRaw(cmdBuf, nullptr, 0, 3000);

    Serial.println("[AWS OTA] Step 9: Finalizing...");
    if (!Update.end(true) || !Update.isFinished()) {
        Serial.printf("[AWS OTA] ERROR: finalize failed: %s\n", Update.errorString());
        _otaFail("finalize");
        return;
    }

    // SUCCEEDED is intentionally NOT reported here — it is published after the
    // NEW image boots and self-validates (otaBootReport). Reboot into new bank.
    Serial.println("===== AWS OTA FLASH SUCCESSFUL — Rebooting into new bank =====");
    delay(1000);
    ESP.restart();
    while (true) {}
}

// ======================= Post-boot + deferred hooks =======================

/**
 * Call ONCE from setup() AFTER connectivity is confirmed (MQTT connected).
 * Confirms image health and, if this boot completed a pending OTA job,
 * publishes SUCCEEDED and clears the persisted marker.
 */
inline void otaBootReport() {
    otaConfirmHealthy();

    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return;
    bool pend = p.getBool("pend", false);
    if (pend) {
        p.getString("job", _jobId, sizeof(_jobId));
        if (_jobId[0]) {
            _otaReportStatus("SUCCEEDED", "fw_version", FW_VERSION);
        }
        p.putBool("pend", false);
        p.remove("job");
    }
    p.end();
}

/**
 * Call from the main loop right after a successful MQTT (re)connect, to flush
 * any pending FAILED status that couldn't be sent while MQTT was down.
 */
inline void otaFlushDeferredReport() {
    if (!_otaFailReport) return;
    if (!mqttConnected) return;
    _otaReportStatus("FAILED", "reason", _otaFailReason);
    _otaFailReport = false;
}
