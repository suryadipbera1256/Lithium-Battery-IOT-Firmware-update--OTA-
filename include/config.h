#pragma once
#include <Arduino.h>

/* =========================================================
 * AWS IOT CORE CONFIGURATION
 * (Values are dynamically injected from .env at compile-time)
 * ========================================================= */
#ifndef AWS_IOT_ENDPOINT
#define AWS_IOT_ENDPOINT    "default-endpoint-ats.iot.ap-south-1.amazonaws.com"
#endif

#ifndef THING_NAME
#define THING_NAME          "DEFAULT_THING"
#endif

#ifndef FW_VERSION
#define FW_VERSION          "1.0.0"
#endif

/* =========================================================
 * HARDWARE PIN DEFINITIONS
 * ========================================================= */
#define EC200U_RX_PIN 16
#define EC200U_TX_PIN 17
#define LED_PIN       2
#define CAN_TX        GPIO_NUM_5
#define CAN_RX        GPIO_NUM_4

/* =========================================================
 * SENSOR PIN DEFINITIONS
 * ========================================================= */
#define MQ2_PIN       25 // Hydrocarbon / Smoke Sensor
#define MQ8_PIN       26 // Hydrogen Gas Sensor

/* =========================================================
 * SYSTEM CONSTANTS & INTERVALS
 * ========================================================= */
#define CRC_16_POLY   0xA001
const String apn = "airteliot.com";
const int mqtt_port = 8883; // Standard port for AWS mTLS

const unsigned long uploadInterval = 30000;     // AWS Publish interval (30s)
const unsigned long bmsReadInterval = 5000;     // CAN Bus read interval (5s)
const unsigned long gpsReadInterval = 10000;    // GPS coordinates fetch interval (10s)
const unsigned long deviceInfoInterval = 60000; // Network info fetch interval (60s)

/* =========================================================
 * DATA STRUCTURES
 * ========================================================= */

// Structure to hold all Battery Management System parameters
struct BMSData {
  float packVoltage; float packCurrent; float residualCapacity; float fullCapacity;
  int cycles; int soc; uint16_t balanceState; uint16_t protectionFlags;
  bool chMOSFET_Cmd; bool chMOSFET_Act; bool dchMOSFET_Cmd; bool dchMOSFET_Act;
  uint16_t productionDate; uint16_t softwareVersion;
  uint8_t batteryStrings; uint8_t ntcCount; float temp[6];
  float cellV[32]; uint8_t cellCount; float minCellV; float maxCellV; float avgCellV; float cellDeltaV;
  unsigned long lastUpdate;
};

// Structure to hold GPS/GNSS parameters
struct GPSData {
  float latitude; float longitude; float speed; float altitude;
  int satellites; int fix; float hdop; bool gpsFixed; unsigned long lastUpdate;
};

// Structure to hold Cellular Network and Device Identity info
struct DeviceInfo {
  char imei[16];
  char ccid[24];
  char operator_name[32];
  char operator_code[10]; 
  int signal_strength; 
  int data_mode; 
  long area_code; 
  long cell_id;         
};

// Structure to hold finalized Location Data (GPS fallback to LBS)
struct LocationData {
  float latitude; 
  float longitude; 
  char source[16]; 
  unsigned long timestamp;
};