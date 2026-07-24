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
#include "EC200U_AWS_OTA.h"
#include "aws_iot_core.h"
#include "bms_can_parser.h"

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

HardwareSerial SerialAT(1);
QuectelEC200U modem(SerialAT, 115200, EC200U_RX_PIN, EC200U_TX_PIN);
Adafruit_BME680 bme;

unsigned long lastUpload = 0;
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

void readGPS() {
    if (!gpsEnabled) return;
    
    char gpsResp[256];
    sendATCommandRaw("AT+QGPSLOC=2", gpsResp, sizeof(gpsResp), 3000);
    gpsData.gpsFixed = false;
    
    char* locStart = strstr(gpsResp, "+QGPSLOC: ");
    if (locStart) {
        locStart += 10; 
        char* savePtr;
        char* token = strtok_r(locStart, ",\r\n", &savePtr);
        int field = 0;
        
        while (token && field < 11) {
            switch (field) {
                case 1: gpsData.latitude = atof(token); break;
                case 2: gpsData.longitude = atof(token); break;
                case 5: gpsData.fix = atoi(token); break;
                case 7: gpsData.speed = atof(token); break;
                case 10: gpsData.satellites = atoi(token); break;
            }
            token = strtok_r(nullptr, ",\r\n", &savePtr);
            field++;
        }
        
        gpsData.gpsFixed = (gpsData.satellites > 0 && gpsData.fix >= 2);
        if (gpsData.gpsFixed) {
            currentLocation.latitude = gpsData.latitude;
            currentLocation.longitude = gpsData.longitude;
            strncpy(currentLocation.source, "GPS", sizeof(currentLocation.source) - 1);
            
            unsigned long now = millis();
            if (currentLocation.timestamp > 0 && now >= currentLocation.timestamp) {
                float timeDiffHours = (now - currentLocation.timestamp) / 3600000.0f;
                if (gpsData.speed > 2.0f) { 
                    float dist = gpsData.speed * timeDiffHours;
                    totalOdometer += dist; 
                }
            }
            currentLocation.timestamp = now;
        }
    }
    
    if (!gpsData.gpsFixed) {
        char lbsResp[128];
        sendATCommandRaw("AT+QCELLLOC=1", lbsResp, sizeof(lbsResp), 5000);
        char* cellStart = strstr(lbsResp, "+QCELLLOC: ");
        if (cellStart) {
            cellStart += 11;
            char* commaPtr = strchr(cellStart, ',');
            if (commaPtr) {
                *commaPtr = '\0'; 
                currentLocation.longitude = atof(cellStart);
                currentLocation.latitude = atof(commaPtr + 1);
                strncpy(currentLocation.source, "LBS", sizeof(currentLocation.source) - 1);
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
    
    if (otaCheckDownlink(buf)) return;
    
    if (strstr(buf, "+QMTSTAT:")) {
        mqttConnected = false;
    }
}

// ============ SETUP ============

void setup() {
    Serial.begin(115200);
    
    // ⚠️ CRITICAL FIX: MUST be called BEFORE SerialAT.begin() or modem.begin()
    SerialAT.setRxBufferSize(8192); 
    
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
    pinMode(MQ2_PIN, INPUT); pinMode(MQ8_PIN, INPUT);

    Serial.println("\n=== POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.0) ===");
    
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

    char gpsResp[64];
    sendATCommandRaw("AT+QGPS=1", gpsResp, sizeof(gpsResp), 3000);
    if (strstr(gpsResp, "OK")) gpsEnabled = true;
    
    collectDeviceInfo();
    connectAndVerify();

    // Post-boot: confirm image health + report SUCCEEDED for a completed job.
    if (mqttConnected) otaBootReport();
}

// ============ MAIN LOOP ============

void loop() {
    if (otaPending()) { otaRun(); return; }

    if (!mqttConnected) {
        if (millis() - lastReconnectAttempt > reconnectCooldown) {
            lastReconnectAttempt = millis();
            connectAndVerify();
            if (mqttConnected) {
                otaFlushDeferredReport();  // publish FAILED if an OTA just failed
                otaBootReport();           // confirm health + SUCCEEDED if boot was post-OTA
            }
        }
        return;
    }

    checkIncoming();

    if (millis() - lastGPSRead >= gpsReadInterval) { 
        lastGPSRead = millis(); 
        readGPS(); 
    }
    
    if (millis() - lastDeviceInfoRead >= deviceInfoInterval) { 
        lastDeviceInfoRead = millis(); 
        collectDeviceInfo(); 
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

        char jsonPayload[512];
        if (buildTelemetryPayload(jsonPayload, sizeof(jsonPayload), &gpsData, &currentLocation, &deviceInfo, 
                                  rawMQ2, rawMQ8, temp, hum, press, gasRes, totalOdometer)) {
            
            char topicBuf[64];
            snprintf(topicBuf, sizeof(topicBuf), "bms/data/%s/telemetry", THING_NAME);
            
            publishToAWS(SerialAT, topicBuf, jsonPayload);
            Serial.println("\n[MQTT] Published JSON Payload successfully.");
        }
    }
}