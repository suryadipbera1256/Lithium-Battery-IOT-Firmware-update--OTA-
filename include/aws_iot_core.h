#pragma once
#include <Arduino.h>
#include "config.h"

/**
 * @brief Generates a complete JSON telemetry payload using a static buffer (O(1) Space Allocation).
 * Prevents heap fragmentation by avoiding dynamic String concatenation.
 * 
 * @param buffer Pointer to the character array where the JSON string will be stored.
 * @param bufferSize Maximum size of the buffer (prevents buffer overflow).
 * @param gps Pointer to the GPS data structure.
 * @param loc Pointer to the Location data structure.
 * @param devInfo Pointer to the Device Info structure.
 * @param mq2 Raw MQ2 sensor value.
 * @param mq8 Raw MQ8 sensor value.
 * @param temp Temperature in Celsius.
 * @param hum Humidity percentage.
 * @param press Pressure in hPa.
 * @param gasRes Gas resistance in kOhms.
 * @param totalOdo Total odometer reading.
 * @return true if the payload was successfully formatted without truncation, false otherwise.
 */
bool buildTelemetryPayload(char* buffer, size_t bufferSize, 
                           const GPSData* gps, const LocationData* loc, const DeviceInfo* devInfo,
                           int mq2, int mq8, float temp, float hum, float press, float gasRes, 
                           float totalOdo);

/**
 * @brief Publishes a JSON payload to a specific AWS IoT MQTT topic using Zero-Allocation AT commands.
 * 
 * @param serialAT Reference to the HardwareSerial connected to the EC200U modem.
 * @param topic The MQTT topic to publish to (C-string).
 * @param payload The null-terminated JSON payload string.
 */
void publishToAWS(HardwareSerial& serialAT, const char* topic, const char* payload);