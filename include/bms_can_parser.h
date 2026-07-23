#pragma once
#include <Arduino.h>
#include "config.h"

/**
 * @brief Parses raw CAN frames into a strongly-typed BMSData struct in O(1) time and space.
 * 
 * @param frameId The standard or extended CAN message identifier.
 * @param frameData Pointer to the 8-byte payload buffer.
 * @param outData Pointer to the BMSData struct to populate.
 * @return true if the frame was successfully parsed, false if data was invalid.
 */
bool parseBMSFrame(uint32_t frameId, const uint8_t* frameData, BMSData* outData);