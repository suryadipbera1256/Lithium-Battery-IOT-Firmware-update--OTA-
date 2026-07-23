#pragma once
#include <Arduino.h>
#include <Adafruit_BME680.h>
#include "config.h"

/**
 * @brief Initializes all onboard sensors (BME680, Gas Sensors).
 * @param bme Reference to the Adafruit_BME680 instance.
 * @return true if BME680 is successfully initialized.
 */
bool initSensors(Adafruit_BME680& bme);

/**
 * @brief Reads raw analog data from MQ series gas sensors.
 * @param mq2Out Pointer to store MQ2 raw value.
 * @param mq8Out Pointer to store MQ8 raw value.
 */
void readGasSensors(int* mq2Out, int* mq8Out);

/**
 * @brief Parses raw AT command response for GPS data without dynamic memory allocation.
 * 
 * @param rawNmeaBuffer Null-terminated char array containing the AT response.
 * @param outGpsData Pointer to the GPSData struct to populate.
 * @return true if a valid GPS fix was parsed.
 */
bool parseGPSResponse(char* rawNmeaBuffer, GPSData* outGpsData);