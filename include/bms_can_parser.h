#pragma once
#include <Arduino.h>
#include "config.h"
#include "driver/twai.h"

// External globals defined in main.cpp
extern BMSData bmsData;
extern bool baudRateLocked;
extern uint32_t currentBaud;

/**
 * @brief Initializes the TWAI (CAN) driver at a specific baud rate.
 */
bool startCAN(uint32_t baud);

/**
 * @brief Requests a data frame from the BMS using ID and 0x5A payload, and validates CRC.
 */
bool requestFrame(uint16_t id, uint8_t* buf);

/**
 * @brief Sequentially requests and parses all BMS data frames (0x100 to 0x110).
 */
void readBMS();

/**
 * @brief Calculates standard Modbus CRC-16.
 */
uint16_t calc_crc16(uint8_t* data, uint8_t len);