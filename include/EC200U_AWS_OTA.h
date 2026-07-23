#pragma once
/* =========================================================
 * EC200U_AWS_OTA.h (ZERO-ALLOCATION EDITION)
 * AWS IoT Jobs (Cloud-Push) OTA for ESP32 + EC200U-CN
 * Features: O(1) Memory, Network Pre-check, Safe Rollback
 * ========================================================= */

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

// External globals from main
extern HardwareSerial SerialAT;
extern bool           mqttConnected;

// OTA Tuning Parameters
#define OTA_HTTP_CONTEXT_ID   1      
#define OTA_SSL_CTX_ID        1      
#define OTA_GET_RSPTIME       120    
#define OTA_READ_WAITTIME     60     
#define OTA_CHUNK             2048   
#define MIN_OTA_RSSI          10     // Minimum CSQ signal required for OTA

// Static Buffers for O(1) Memory
static bool _otaPending = false;
static char _otaUrl[256] = {0};
static char _jobId[64] = {0};

inline bool otaPending() { return _otaPending; }

// --- Zero-Allocation Helper Functions ---

static void _otaATRaw(const char* cmd, char* outBuffer, size_t bufferSize, uint32_t timeoutMs = 5000) {
    char tempBuf[128] = {0};
    char* targetBuf = outBuffer ? outBuffer : tempBuf;
    size_t targetSize = outBuffer ? bufferSize : sizeof(tempBuf);

    memset(targetBuf, 0, targetSize);
    while (SerialAT.available()) SerialAT.read();
    
    SerialAT.println(cmd);
    unsigned long start = millis();
    size_t idx = 0;
    
    while (millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        while (SerialAT.available() && idx < targetSize - 1) {
            targetBuf[idx++] = (char)SerialAT.read();
        }
        if (strstr(targetBuf, "OK\r\n") || strstr(targetBuf, "ERROR\r\n")) {
            delay(50);
            break;
        }
    }
}
static bool _otaWaitForPattern(const char* pattern, uint32_t timeoutMs) {
    unsigned long start = millis();
    char buf[128] = {0};
    size_t idx = 0;
    
    while (millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        while (SerialAT.available() && idx < sizeof(buf) - 1) {
            buf[idx++] = (char)SerialAT.read();
            buf[idx] = '\0';
            if (strstr(buf, pattern)) return true;
        }
    }
    return false; 
}

// Check network stability before downloading
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

// Parses MQTT Downlink for AWS Jobs using Pointers
inline bool otaCheckDownlink(const char* urcLine) {
    if (_otaPending) return true; 
    
    if (!strstr(urcLine, "+QMTRECV:") || !strstr(urcLine, "jobs/notify-next")) {
        return false;
    }

    // Extract Presigned URL
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
    if (sscanf(p, "+QHTTPGET: %d,%d,%ld", &err, &code, &len) == 3) {
        if (err == 0 && code == 200) return len;
    }
    return -1;
}

static void _otaFail() {
    if (Update.isRunning()) Update.abort();
    Serial.println("[AWS OTA] FAILED — Discarding update. ESP32 will auto-rollback if needed.");
    mqttConnected = false;               
}

// --- Main Blocking OTA Runner ---

inline void otaRun() {
    if (!_otaPending) return;
    _otaPending = false;

    Serial.println("\n===== AWS OTA START =====");
    
    // PRE-CHECK: Network Quality
    if (!_isNetworkStable()) {
        Serial.println("[AWS OTA] ERROR: Network signal too weak for OTA. Aborting safely.");
        return;
    }

    // 1. Safe MQTT Disconnect
    _otaATRaw("AT+QMTDISC=0", nullptr, 0, 3000);
    _otaATRaw("AT+QMTCLOSE=0", nullptr, 0, 5000);
    delay(300);

    // 2. HTTP/SSL Configuration
    char cmdBuf[64];
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
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"seclevel\",%d,2", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"cacert\",%d,\"UFS:rootCA.pem\"", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"clientcert\",%d,\"UFS:cert.pem\"", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"clientkey\",%d,\"UFS:privkey.pem\"", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);

    // 3. Send Presigned URL
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPURL=%u,30", strlen(_otaUrl));
    SerialAT.println(cmdBuf);
    
    if (!_otaWaitForPattern("CONNECT", 5000)) { _otaFail(); return; }
    
    SerialAT.print(_otaUrl);
    if (!_otaWaitForPattern("OK", 5000)) { _otaFail(); return; }

    // 4. HTTP GET Request
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPGET=%d", OTA_GET_RSPTIME);
    SerialAT.println(cmdBuf);
    
    char getResp[128] = {0};
    unsigned long startWait = millis();
    size_t idx = 0;
    while (millis() - startWait < (uint32_t)OTA_GET_RSPTIME * 1000UL) {
        esp_task_wdt_reset();
        while (SerialAT.available() && idx < sizeof(getResp) - 1) {
            getResp[idx++] = (char)SerialAT.read();
        }
        if (strstr(getResp, "+QHTTPGET:")) break;
    }

    long len = _otaParseGetLen(getResp);
    if (len <= 0) { _otaFail(); return; }

    // 5. Initialize ESP32 OTA (Dual-Bank Safe)
    if (!Update.begin((size_t)len, U_FLASH)) {
        _otaFail(); return;
    }

    // 6. Stream Binary Data (O(1) Memory Chunking)
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPREAD=%d", OTA_READ_WAITTIME);
    SerialAT.println(cmdBuf);
    
    if (!_otaWaitForPattern("CONNECT", 10000)) { Update.abort(); _otaFail(); return; }
    
    // Clear trailing newline
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        if (SerialAT.available() && SerialAT.read() == '\n') break;
    }

    static uint8_t buf[OTA_CHUNK];
    long got = 0;
    unsigned long lastData = millis();
    
    while (got < len) {
        esp_task_wdt_reset();
        int avail = SerialAT.available();
        
        if (avail <= 0) {
            if (millis() - lastData > (uint32_t)OTA_READ_WAITTIME * 1000UL) {
                Update.abort(); _otaFail(); return;
            }
            continue;
        }
        
        long want = len - got;
        int n = (avail < (int)sizeof(buf)) ? avail : (int)sizeof(buf);
        if (n > want) n = (int)want;
        
        int r = SerialAT.readBytes(buf, n);
        if (r > 0) {
            if (Update.write(buf, r) != (size_t)r) {
                Update.abort(); _otaFail(); return;
            }
            got += r;
            lastData = millis();
        }
    }

    _otaWaitForPattern("+QHTTPREAD:", 10000);

    // 7. Finalize & Reboot (Rollback enabled)
    if (!Update.end(true) || !Update.isFinished()) {
        _otaFail(); return;
    }
    
    Serial.println("===== AWS OTA FLASH SUCCESSFUL — Rebooting Device =====");
    delay(1000);
    ESP.restart();
    while (true) {} 
}