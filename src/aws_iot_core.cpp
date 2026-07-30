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
        loc->latitude, loc->longitude, loc->source, gps->speed, gps->satellites,
        mq2, mq8, 
        temp, hum, press, gasRes,
        devInfo->signal_strength, devInfo->imei, totalOdo
    );

    // Check for buffer truncation (security/stability check)
    if (written < 0 || (size_t)written >= bufferSize) {
        Serial.println("[ERROR] Telemetry payload truncated! Buffer size too small.");
        return false; 
    }

    return true;
}

void publishToAWS(HardwareSerial& serialAT, const char* topic, const char* payload) {
    if (topic == nullptr || payload == nullptr) return;

    // 1. Build the AT Command dynamically without using String class
    char cmdBuf[128];
    int len = snprintf(cmdBuf, sizeof(cmdBuf), "AT+QMTPUBEX=0,1,1,0,\"%s\",%u", topic, strlen(payload));
    
    if (len < 0 || (size_t)len >= sizeof(cmdBuf)) {
        Serial.println("[ERROR] MQTT Command truncated!");
        return;
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
        return;
    }

    // 4. Dispatch the raw binary stream directly (Zero-Allocation)
    serialAT.print(payload);
    
    // Give modem time to process
    unsigned long waitTime = millis();
    while(millis() - waitTime < 500) { 
        if(serialAT.available()) serialAT.read(); 
    }
}