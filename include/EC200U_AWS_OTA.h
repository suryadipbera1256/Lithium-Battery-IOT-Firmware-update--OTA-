#pragma once
/* =========================================================
 * EC200U_AWS_OTA.h (ZERO-ALLOCATION EDITION - BULLETPROOF)
 * AWS IoT Jobs (Cloud-Push) OTA for ESP32 + EC200U-CN
 * Features: O(1) Memory, WDT Safe, Chunked Flash Write
 * ========================================================= */

#include <Arduino.h>
#include <Update.h>

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
static char _otaUrl[1536] = {0};     
static char _jobId[64] = {0};

inline bool otaPending() { return _otaPending; }

// --- Zero-Allocation Helper Functions ---

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
        delay(1); // WDT safe yield (Replaced esp_task_wdt_reset)
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
        delay(1); 
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

// Parses MQTT Downlink for AWS Jobs
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
    
    Serial.println("[AWS OTA] Step 1: Checking Network Stability...");
    if (!_isNetworkStable()) {
        Serial.println("[AWS OTA] ERROR: Network signal too weak for OTA. Aborting safely.");
        return;
    }

    Serial.println("[AWS OTA] Step 2: Safe MQTT Disconnect...");
    _otaATRaw("AT+QMTDISC=0", nullptr, 0, 3000);
    _otaATRaw("AT+QMTCLOSE=0", nullptr, 0, 5000);
    delay(500);

    Serial.println("[AWS OTA] Step 3: Configuring HTTP & SSL Engine...");
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
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"seclevel\",%d,0", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);

    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"sni\",%d,1", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);
    
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QSSLCFG=\"ignorertctime\",%d,1", OTA_SSL_CTX_ID);
    _otaATRaw(cmdBuf, nullptr, 0);

    Serial.println("[AWS OTA] Step 4: Submitting S3 Pre-signed URL...");
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPURL=%u,30", strlen(_otaUrl));
    SerialAT.println(cmdBuf);
    
    if (!_otaWaitForPattern("CONNECT", 5000)) { _otaFail(); return; }
    
    SerialAT.print(_otaUrl);
    if (!_otaWaitForPattern("OK", 5000)) { _otaFail(); return; }

    Serial.println("[AWS OTA] Step 5: Requesting Firmware File (GET)...");
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPGET=%d", OTA_GET_RSPTIME);
    SerialAT.println(cmdBuf);
    
    char getResp[128] = {0};
    unsigned long startWait = millis();
    size_t idx = 0;
    while (millis() - startWait < (uint32_t)OTA_GET_RSPTIME * 1000UL) {
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
        if (p && strchr(p, '\n')) { 
            break; 
        }
    }

    Serial.print("[DEBUG] Raw GET Response: ");
    Serial.println(getResp);

    long len = _otaParseGetLen(getResp);
    if (len <= 0) { 
        Serial.println("[AWS OTA] ERROR: Failed to get file length from server.");
        _otaFail(); return; 
    }
    Serial.printf("[AWS OTA] Firmware Size: %ld Bytes. Preparing Flash...\n", len);

    Serial.println("[AWS OTA] Step 6: Initializing ESP32 OTA Dual-Bank...");
    if (!Update.begin((size_t)len, U_FLASH)) {
        Serial.println("[AWS OTA] ERROR: Not enough space in flash partition!");
        _otaFail(); return;
    }

    Serial.println("[AWS OTA] Step 7: Downloading and Writing Chunks...");
    while (SerialAT.available()) SerialAT.read();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QHTTPREAD=%d", OTA_READ_WAITTIME);
    SerialAT.println(cmdBuf);
    
    if (!_otaWaitForPattern("CONNECT", 10000)) { Update.abort(); _otaFail(); return; }
    
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        if (SerialAT.available() && SerialAT.read() == '\n') break;
        delay(1);
    }

    uint8_t buf[OTA_CHUNK]; 
    long got = 0;                  
    unsigned long lastData = millis();
    long last_dot = 0;             
    
    // ---- BULLETPROOF CHUNK DOWNLOADING ----
    while (got < len) {
        int to_read = len - got;
        if (to_read > OTA_CHUNK) to_read = OTA_CHUNK;
        
        int chunk_got = 0;
        
        // Grab exactly one full chunk (or the remaining bytes)
        while (chunk_got < to_read) {
            int avail = SerialAT.available();
            
            if (avail > 0) {
                int chunk_left = to_read - chunk_got;
                int bytes_to_grab = (avail < chunk_left) ? avail : chunk_left;
                
                // Fast hardware-level read directly into the buffer
                int r = SerialAT.readBytes(buf + chunk_got, bytes_to_grab);
                if (r > 0) {
                    chunk_got += r;
                    lastData = millis();
                }
            } else {
                // Timeout check
                if (millis() - lastData > (uint32_t)OTA_READ_WAITTIME * 1000UL) {
                    Serial.println("\n[AWS OTA] ERROR: Download timeout.");
                    Update.abort(); _otaFail(); return;
                }
                delay(1); // Safely feed WDT while waiting for modem data
            }
        }
        
        // Write the perfectly sized chunk to flash memory
        if (Update.write(buf, chunk_got) != (size_t)chunk_got) {
            Serial.printf("\n[AWS OTA] ERROR: Flash write failed at %ld bytes.\n", got);
            Update.abort(); _otaFail(); return;
        }
        
        got += chunk_got; 
        
        // Print progress dot every 10KB
        if (got - last_dot >= 10240) {
            Serial.print(".");
            last_dot = got;
        }
    }
    Serial.println("\n[AWS OTA] Download Complete!");

    _otaWaitForPattern("+QHTTPREAD:", 10000);

    Serial.println("[AWS OTA] Step 8: Finalizing Update...");
    if (!Update.end(true) || !Update.isFinished()) {
        Serial.println("[AWS OTA] ERROR: Flash finalization failed.");
        _otaFail(); return;
    }
    
    Serial.println("===== AWS OTA FLASH SUCCESSFUL — Rebooting Device =====");
    delay(1000);
    ESP.restart();
    while (true) {} 
}