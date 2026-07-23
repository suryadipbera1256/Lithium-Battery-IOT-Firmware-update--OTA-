#include "bms_can_parser.h"

bool parseBMSFrame(uint32_t frameId, const uint8_t* frameData, BMSData* outData) {
    // Robust edge-case check to prevent null pointer dereferencing
    if (frameData == nullptr || outData == nullptr) {
        return false;
    }

    // O(1) parsing using direct bitwise shifts. No temporary objects created.
    switch (frameId) {
        case 0x90: // Example ID: Voltage, Current, SOC
            outData->packVoltage = ((frameData[0] << 8) | frameData[1]) * 0.1f;
            outData->packCurrent = ((frameData[2] << 8) | frameData[3]) * 0.1f - 30000.0f;
            outData->soc = frameData[4];
            break;
            
        case 0x91: // Example ID: Max/Min Cell Voltages
            outData->maxCellV = ((frameData[0] << 8) | frameData[1]) * 0.001f;
            outData->minCellV = ((frameData[2] << 8) | frameData[3]) * 0.001f;
            outData->cellDeltaV = outData->maxCellV - outData->minCellV;
            break;

        case 0x92: // Example ID: Temperatures
            outData->temp[0] = (frameData[0] - 40.0f);
            outData->temp[1] = (frameData[1] - 40.0f);
            break;
            
        default:
            return false; // Unknown frame ID
    }
    
    outData->lastUpdate = millis();
    return true;
}