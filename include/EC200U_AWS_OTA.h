#pragma once
/* =========================================================
 * EC200U_AWS_OTA.h 
 * AWS IoT Jobs (Cloud-Push) OTA for ESP32 + EC200U-CN
 * Handles JSON parsing and binary firmware streaming via HTTPS
 * ========================================================= */

#include <Arduino.h>
#include <Update.h>

// External variables declared in main.cpp
extern HardwareSerial SerialAT;
extern bool           mqttConnected;

// OTA Tuning Parameters
#ifndef OTA_HTTP_CONTEXT_ID
#define OTA_HTTP_CONTEXT_ID   1      
#endif
#ifndef OTA_SSL_CTX_ID
#define OTA_SSL_CTX_ID        1      
#endif
#ifndef OTA_GET_RSPTIME
#define OTA_GET_RSPTIME       120    
#endif
#ifndef OTA_READ_WAITTIME
#define OTA_READ_WAITTIME     60     
#endif
#ifndef OTA_CHUNK
#define OTA_CHUNK             2048   
#endif

static bool   _otaPending = false;
static String _otaUrl;
static String _jobId;

// Returns true if an OTA job is in the queue
inline bool otaPending() { return _otaPending; }

// Helper function to send AT commands and read response within a timeout
static String _otaAT(const String& cmd, uint32_t timeoutMs = 5000) {
  while (SerialAT.available()) SerialAT.read(); // Flush buffer       
  SerialAT.println(cmd);
  
  String resp;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
    // Break early if standard responses are found
    if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
      delay(30);
      while (SerialAT.available()) resp += (char)SerialAT.read();
      break;
    }
  }
  return resp;
}

// Wait for a specific pattern in the serial buffer
static String _otaWaitFor(const String& pattern, uint32_t timeoutMs) {
  String buf;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (SerialAT.available()) { char c = SerialAT.read(); buf += c; }
    if (buf.indexOf(pattern) >= 0) return buf;
  }
  return buf; 
}

// Parse AWS IoT Jobs JSON payload to extract the Presigned S3 HTTPS URL
static String _otaExtractUrl(const String& s) {
  int i = s.indexOf("https://");
  if (i < 0) return "";
  int j = i;
  while (j < (int)s.length()) {
    char c = s[j];
    // Break at standard JSON delimiters
    if (c == '"' || c == '\\' || c == ' ' || c == '\r' || c == '\n' || c == '}' || c == ',') break;
    j++;
  }
  return s.substring(i, j);
}

// Extract Job ID from AWS Payload for tracking
static String _otaExtractJobId(const String& s) {
  int i = s.indexOf("\"jobId\":\"");
  if (i < 0) return "";
  i += 9;
  int j = s.indexOf("\"", i);
  if (j < 0) return "";
  return s.substring(i, j);
}

// Check if incoming MQTT message is from AWS Jobs Notification Topic
inline bool otaCheckDownlink(const String& urcLine) {
  if (_otaPending) return true; // Ignore if already queued                      
  if (urcLine.indexOf("+QMTRECV:") < 0) return false;

  // Verify the topic contains the AWS Jobs identifier
  bool isAwsJob = (urcLine.indexOf("jobs/notify-next") >= 0);
  if (!isAwsJob) return false;

  String url = _otaExtractUrl(urcLine);
  String jobId = _otaExtractJobId(urcLine);
  
  // Abort if invalid payload
  if (url.length() == 0 || jobId.length() == 0) return false;

  _otaUrl     = url;
  _jobId      = jobId;
  _otaPending = true;
  
  Serial.println("[AWS OTA] Firmware Update Requested. Job ID: " + _jobId);
  return true;
}

// Parse content length from HTTP GET response
static long _otaParseGetLen(const String& s) {
  int p = s.indexOf("+QHTTPGET:");
  if (p < 0) return -1;
  int c1 = s.indexOf(',', p);          if (c1 < 0) return -1;
  int c2 = s.indexOf(',', c1 + 1);     if (c2 < 0) return -1;   
  int err  = s.substring(p + 10, c1).toInt();
  int code = s.substring(c1 + 1, c2).toInt();
  if (err != 0 || code != 200) return -1; // Abort if not HTTP 200 OK
  return s.substring(c2 + 1).toInt();
}

// Graceful failure handler
static void _otaFail() {
  if (Update.isRunning()) Update.abort();
  Serial.println("[AWS OTA] FAILED — Discarding update and reconnecting MQTT.");
  mqttConnected = false;               
}

// Main blocking function to execute OTA download and flash
inline void otaRun() {
  if (!_otaPending) return;
  _otaPending = false;

  Serial.println("\n===== AWS OTA START =====");
  
  // 1. Disconnect MQTT to avoid serial data corruption during download
  _otaAT("AT+QMTDISC=0", 3000);
  _otaAT("AT+QMTCLOSE=0", 12000);
  delay(300);

  // 2. Configure HTTP context parameters
  _otaAT("AT+QHTTPCFG=\"contextid\","      + String(OTA_HTTP_CONTEXT_ID));
  _otaAT("AT+QHTTPCFG=\"requestheader\",0");
  _otaAT("AT+QHTTPCFG=\"responseheader\",0"); // Body-only response               
  _otaAT("AT+QHTTPCFG=\"sslctxid\","       + String(OTA_SSL_CTX_ID));
  
  // 3. Apply AWS Enterprise mTLS configuration for HTTPS Download
  _otaAT("AT+QSSLCFG=\"sslversion\","      + String(OTA_SSL_CTX_ID) + ",4");      
  _otaAT("AT+QSSLCFG=\"ciphersuite\","     + String(OTA_SSL_CTX_ID) + ",0xFFFF");
  _otaAT("AT+QSSLCFG=\"seclevel\","        + String(OTA_SSL_CTX_ID) + ",2");      
  _otaAT("AT+QSSLCFG=\"cacert\","          + String(OTA_SSL_CTX_ID) + ",\"UFS:rootCA.pem\"");
  _otaAT("AT+QSSLCFG=\"clientcert\","      + String(OTA_SSL_CTX_ID) + ",\"UFS:cert.pem\"");
  _otaAT("AT+QSSLCFG=\"clientkey\","       + String(OTA_SSL_CTX_ID) + ",\"UFS:privkey.pem\"");
  _otaAT("AT+QSSLCFG=\"ignorelocaltime\"," + String(OTA_SSL_CTX_ID) + ",1");

  // 4. Send the Presigned S3 URL to the modem
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QHTTPURL=" + String(_otaUrl.length()) + ",30");
  if (_otaWaitFor("CONNECT", 5000).indexOf("CONNECT") < 0) {
    _otaFail(); return;
  }
  SerialAT.print(_otaUrl);
  _otaWaitFor("OK", 5000);

  // 5. Trigger HTTP GET Request
  _otaAT("AT+QHTTPGET=" + String(OTA_GET_RSPTIME), 3000);
  String g   = _otaWaitFor("+QHTTPGET:", (uint32_t)OTA_GET_RSPTIME * 1000UL);
  long   len = _otaParseGetLen(g);
  
  if (len <= 0) { _otaFail(); return; }

  // 6. Initialize ESP32 OTA Update Partition
  if (!Update.begin((size_t)len)) {
    _otaFail(); return;
  }

  // 7. Stream bytes from Modem UART directly to ESP32 Flash Memory
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QHTTPREAD=" + String(OTA_READ_WAITTIME));
  
  if (_otaWaitFor("CONNECT", 10000).indexOf("CONNECT") < 0) {
     Update.abort(); _otaFail(); return;
  }
  
  // Consume trailing newline after CONNECT
  {
    unsigned long t0 = millis(); bool nl = false;
    while (millis() - t0 < 3000 && !nl) {
      while (SerialAT.available()) { if (SerialAT.read() == '\n') { nl = true; break; } }
    }
  }

  static uint8_t buf[OTA_CHUNK];
  long          got      = 0;
  unsigned long lastData = millis();
  
  // Read stream chunk by chunk
  while (got < len) {
    int avail = SerialAT.available();
    if (avail <= 0) {
      // Timeout check
      if (millis() - lastData > (uint32_t)OTA_READ_WAITTIME * 1000UL) {
        Update.abort(); _otaFail(); return;
      }
      continue;
    }
    
    long want = len - got;
    int  n    = (avail < (int)sizeof(buf)) ? avail : (int)sizeof(buf);
    if (n > want) n = (int)want;
    
    int r = SerialAT.readBytes(buf, n);
    if (r > 0) {
      // Write buffer to flash
      if (Update.write(buf, r) != (size_t)r) {
        Update.abort(); _otaFail(); return;
      }
      got     += r;
      lastData = millis();
    }
  }

  _otaWaitFor("+QHTTPREAD:", 10000);

  // 8. Finalize Update and Reboot
  if (!Update.end(true) || !Update.isFinished()) {
    _otaFail(); return;
  }
  
  Serial.println("===== AWS OTA FLASH SUCCESSFUL — Rebooting Device =====");
  delay(500);
  ESP.restart();
  while (true) {} // Halt execution until reset completes
}