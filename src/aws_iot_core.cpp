#include "aws_iot_core.h"

bool buildTelemetryPayload(char* buffer, size_t bufferSize, 
                           const GPSData* gps, const LocationData* loc, const DeviceInfo* devInfo,
                           int mq2, int mq8, float temp, float hum, float press, float gasRes, 
                           float totalOdo) {
    
    // Edge-case check for null pointers
    if (buffer == nullptr || gps == nullptr || loc == nullptr || devInfo == nullptr) {
        return false; 
    }

    // snprintf safely formats the JSON string directly into the static buffer.
    // It returns the number of characters that would have been written if the buffer was large enough.
    int written = snprintf(buffer, bufferSize,
        "{"
        "\"thing_name\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"location\":{\"latitude\":%.6f,\"longitude\":%.6f,\"source\":\"%s\",\"speed_kmh\":%.2f,\"satellites\":%d},"
        "\"sensors\":{\"mq2_gas_raw\":%d,\"mq8_gas_raw\":%d,\"bme680\":{\"temp_c\":%.2f,\"humidity_pct\":%.2f,\"pressure_hpa\":%.2f,\"gas_res_kohm\":%.2f}},"
        "\"telemetry\":{\"rssi\":%d,\"imei\":\"%s\",\"total_odometer\":%.2f}"
        "}",
        THING_NAME, 
        FW_VERSION,
        loc->latitude, loc->longitude, loc->source.c_str(), gps->speed, gps->satellites,
        mq2, mq8, 
        temp, hum, press, gasRes,
        devInfo->signal_strength, devInfo->imei.c_str(), totalOdo
    );

    // Check for buffer truncation (security/stability check)
    if (written < 0 || (size_t)written >= bufferSize) {
        Serial.println("[ERROR] Telemetry payload truncated! Buffer size too small.");
        return false; 
    }

    return true;
}

bool publishToAWS(HardwareSerial& serialAT, const char* topic, const char* payload) {
    if (topic == nullptr || payload == nullptr) return false;

    // 1. Build the AT Command dynamically without using String class
    char cmdBuf[128];
    int len = snprintf(cmdBuf, sizeof(cmdBuf), "AT+QMTPUBEX=0,1,1,0,\"%s\",%u", topic, strlen(payload));

    if (len < 0 || (size_t)len >= sizeof(cmdBuf)) {
        Serial.println("[ERROR] MQTT Command truncated!");
        return false;
    }

    // Flush RX buffer
    while (serialAT.available()) serialAT.read();
    
    // 2. Send the AT command
    serialAT.println(cmdBuf);
    
    // 3. Wait for the '>' prompt indicating the modem is ready for the payload
    unsigned long start = millis(); 
    bool gotPrompt = false;
    while (millis() - start < 3000) {
        if (serialAT.available()) { 
            if (serialAT.read() == '>') { 
                gotPrompt = true; 
                break; 
            } 
        }
    }
    
    if (!gotPrompt) {
        Serial.println("[ERROR] Modem timeout waiting for '>' prompt.");
        return false;
    }

    // 4. Dispatch the raw binary stream directly (Zero-Allocation)
    serialAT.print(payload);

    /* 5. Confirm the broker took it. "+QMTPUBEX: <client>,<msgid>,<result>"
     *    with result 0 means delivered; anything else (or nothing at all) means
     *    the payload is gone. Scanned in a small rolling buffer so no String
     *    allocation happens on the hot publish path. */
    char ack[96] = {0};
    size_t idx = 0;
    bool   ok  = false;
    unsigned long waitTime = millis();
    while (millis() - waitTime < 3000) {
        while (serialAT.available()) {
            if (idx >= sizeof(ack) - 1) {
                size_t shift = sizeof(ack) / 2;
                memmove(ack, ack + shift, sizeof(ack) - shift);
                idx -= shift;
                memset(ack + idx, 0, sizeof(ack) - idx);
            }
            ack[idx++] = (char)serialAT.read();
            ack[idx]   = '\0';
        }
        char* p = strstr(ack, "+QMTPUBEX:");
        if (p) {
            int client = -1, msgid = -1, result = -1;
            if (sscanf(p, "+QMTPUBEX: %d,%d,%d", &client, &msgid, &result) == 3) {
                ok = (result == 0);
                if (!ok) Serial.printf("[ERROR] Publish rejected, result=%d\n", result);
                break;
            }
        }
        if (strstr(ack, "ERROR")) break;
        delay(1);
    }

    if (!ok && !strstr(ack, "+QMTPUBEX:")) {
        Serial.println("[ERROR] No publish confirmation from modem.");
    }
    return ok;
}