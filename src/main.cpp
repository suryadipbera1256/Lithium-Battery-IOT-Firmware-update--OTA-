/* =========================================================
 * ESP32 + EC200U-CN + BMS (AWS IOT CORE - ENTERPRISE V1.0.0)
 * ========================================================= */

#include <Arduino.h>
#include <QuectelEC200U.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "driver/twai.h"
#include "config.h"           // Hardware and AWS configuration
#include "EC200U_AWS_OTA.h"   // AWS OTA Handlers

// ============ GLOBALS ============
BMSData bmsData = {0};
GPSData gpsData = {0};
DeviceInfo deviceInfo = {"", "", "", "", 0, 0, 0, 0};
LocationData currentLocation = {0, 0, "NONE", 0};
String v8RawHex = ""; 

float totalOdometer = 0.0;
float tripOdometer = 0.0;

bool chMOS = false; 
bool dchMOS = false; 
bool baudRateLocked = false;
uint32_t currentBaud = 0;
bool mqttConnected = false;
bool gpsEnabled = false;

// Global Instances
HardwareSerial SerialAT(1);
QuectelEC200U modem(SerialAT, 115200, EC200U_RX_PIN, EC200U_TX_PIN);
Adafruit_BME680 bme;

unsigned long lastUpload = 0;
unsigned long lastBMSRead = 0;
unsigned long lastGPSRead = 0;
unsigned long lastDeviceInfoRead = 0;

// ============ HELPER FUNCTIONS ============

// Calculates 16-bit CRC for Modbus/CAN validation
uint16_t calc_crc16(uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)*data++;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ CRC_16_POLY;
      else crc >>= 1;
    }
  }
  return crc;
}

// Extracts a substring based on a specific character separator
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// Sends an AT command to the modem and waits for a response
String sendATCommand(const String &cmd, uint32_t timeoutMs = 5000) {
  while (SerialAT.available()) SerialAT.read(); // Clear buffer
  SerialAT.println(cmd);
  
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      resp += (char)SerialAT.read();
    }
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1 || resp.indexOf("+CME ERROR") != -1) {
      delay(50);
      while (SerialAT.available()) resp += (char)SerialAT.read();
      break; // Exit loop if terminator is found
    }
  }
  return resp;
}

// Extracts a string between two delimiters
String extractBetween(const String &response, const String &start_delim, const String &end_delim) {
  int start = response.indexOf(start_delim);
  if (start == -1) return "";
  start += start_delim.length();
  int end = response.indexOf(end_delim, start);
  if (end == -1) end = response.length();
  String result = response.substring(start, end);
  result.trim();
  return result;
}

// ============ SENSOR & DEVICE INFORMATION ============

// Fetches Cellular Information (IMEI, Operator, Signal Strength)
void collectDeviceInfo() {
  String imeiResp = sendATCommand("AT+CGSN", 3000);
  String parsedImei = "";
  for (int i = 0; i < imeiResp.length(); i++) {
    if (isDigit(imeiResp[i])) parsedImei += imeiResp[i];
  }
  if (parsedImei.length() >= 15) deviceInfo.imei = parsedImei.substring(0, 15);
  
  String signalResp = sendATCommand("AT+CSQ", 3000);
  String rssi = extractBetween(signalResp, "+CSQ: ", ",");
  if (rssi.length() > 0) {
    int rssi_int = rssi.toInt();
    deviceInfo.signal_strength = (rssi_int == 99) ? 0 : (-113 + (2 * rssi_int));
  }
  
  String qengResp = sendATCommand("AT+QENG=\"servingcell\"", 3000);
  String payload = extractBetween(qengResp, "+QENG: ", "\n");
  payload.trim();
  
  if (payload.length() > 0) {
    String rat = getValue(payload, ',', 2);
    rat.replace("\"", "");
    
    if (rat == "LTE") {
      deviceInfo.data_mode = 4;
      String mcc = getValue(payload, ',', 4);
      String mnc = getValue(payload, ',', 5);
      deviceInfo.operator_code = mcc + mnc;
    }
  }
}

// Fetches Location Coordinates (Prefers GPS, fallbacks to LBS)
void readGPS() {
  if (!gpsEnabled) return;
  
  String gpsResp = sendATCommand("AT+QGPSLOC=2", 3000);
  gpsData.gpsFixed = false;
  
  // Parse GPS if fixed
  if (gpsResp.indexOf("+QGPSLOC:") != -1) {
    String data = extractBetween(gpsResp, "+QGPSLOC: ", "\n");
    int fields = 0; String values[11]; int lastIdx = 0;
    
    // Manual CSV Split
    for (int i = 0; i <= data.length(); i++) {
      if (data[i] == ',' || i == data.length()) {
        if (fields < 11) {
          values[fields] = data.substring(lastIdx, i);
          values[fields].trim();
          lastIdx = i + 1;
          fields++;
        }
      }
    }
    
    if (fields >= 11) {
      gpsData.latitude = values[1].toFloat();
      gpsData.longitude = values[2].toFloat();
      gpsData.fix = values[5].toInt();
      gpsData.speed = values[7].toFloat();
      gpsData.satellites = values[10].toInt();
      gpsData.gpsFixed = (gpsData.satellites > 0 && gpsData.fix >= 2);
      
      if (gpsData.gpsFixed) {
        currentLocation.latitude = gpsData.latitude;
        currentLocation.longitude = gpsData.longitude;
        currentLocation.source = "GPS";
        
        // Calculate Odometer if moving
        if (currentLocation.timestamp > 0) {
           float timeDiffHours = (millis() - currentLocation.timestamp) / 3600000.0;
           if (gpsData.speed > 2.0) { 
              float dist = gpsData.speed * timeDiffHours;
              totalOdometer += dist;
              tripOdometer += dist;
           }
        }
        currentLocation.timestamp = millis();
      }
    }
  }
  
  // Fallback to LBS (Cell Tower Location) if GPS is lost
  if (!gpsData.gpsFixed) {
    String lbsResp = sendATCommand("AT+QCELLLOC=1", 5000);
    if (lbsResp.indexOf("+QCELLLOC:") != -1) {
      String locData = extractBetween(lbsResp, "+QCELLLOC: ", "\n");
      int commaIdx = locData.indexOf(",");
      if (commaIdx != -1) {
        currentLocation.longitude = locData.substring(0, commaIdx).toFloat();
        currentLocation.latitude = locData.substring(commaIdx + 1).toFloat();
        currentLocation.source = "LBS (approx)";
        currentLocation.timestamp = millis(); 
      }
    }
  }
}

// ============ AWS MQTT HANDLING ============

// Publishes payload to AWS IoT Topic securely
void mqttPublish(const String &topic, const String &payload) {
  String cmd = "AT+QMTPUBEX=0,1,1,0,\"" + topic + "\"," + String(payload.length());
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);
  
  // Wait for modem '>' prompt before sending payload
  unsigned long start = millis(); bool gotPrompt = false;
  while (millis() - start < 3000) {
    if (SerialAT.available()) { if (SerialAT.read() == '>') { gotPrompt = true; break; } }
  }
  if (!gotPrompt) return;

  SerialAT.print(payload); // Dispatch actual data
  
  // Wait to clear buffers
  unsigned long waitTime = millis();
  while(millis() - waitTime < 500) { if(SerialAT.available()) SerialAT.read(); }
}

// Connects to AWS IoT Core using mTLS Certificates
void connectAndVerify() {
  sendATCommand("AT+QMTDISC=0", 3000); 
  sendATCommand("AT+QMTCLOSE=0", 5000); 
  delay(500);
  
  // 1. Open Socket to AWS Endpoint
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTOPEN=0,\"" + String(AWS_IOT_ENDPOINT) + "\"," + String(mqtt_port));

  String openResp = ""; unsigned long start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) openResp += (char)SerialAT.read();
    if (openResp.indexOf("+QMTOPEN: 0,") != -1) break;
  }
  if (openResp.indexOf("+QMTOPEN: 0,0") == -1) { mqttConnected = false; return; } // Socket failed

  // 2. Establish MQTT Connection (Client ID required, User/Pass ignored in mTLS)
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTCONN=0,\"" + String(THING_NAME) + "\"");

  String connResp = ""; start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) connResp += (char)SerialAT.read();
    if (connResp.indexOf("+QMTCONN: 0,") != -1) break;
  }
  
  if (connResp.indexOf("+QMTCONN: 0,0,0") != -1) {
    mqttConnected = true; 
    
    // 3. Subscribe to AWS Jobs notification topic to listen for OTA updates
    String jobTopic = "$aws/things/" + String(THING_NAME) + "/jobs/notify-next";
    sendATCommand("AT+QMTSUB=0,1,\"" + jobTopic + "\",1", 10000);
  } else { 
    mqttConnected = false; 
  }
}

// Scans serial lines for incoming MQTT messages
void checkIncoming() {
  if (!SerialAT.available()) return;
  String line = ""; 
  unsigned long start = millis();
  
  // Buffer the incoming URC (Unsolicited Result Code)
  while (millis() - start < 100) { 
      while (SerialAT.available()) line += (char)SerialAT.read(); 
  }
  if (line.length() == 0) return;
  
  // Forward to OTA Library to check if it's an update job
  if (otaCheckDownlink(line)) return;
  
  // Mark disconnected if modem reports disconnect
  if (line.indexOf("+QMTSTAT:") != -1) mqttConnected = false;
}

// ============ SYSTEM SETUP ============

void setup() {
  Serial.begin(115200); 
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW); 
  delay(3000);
  
  // Set MQ Gas Sensors as Analog Inputs
  pinMode(MQ2_PIN, INPUT);
  pinMode(MQ8_PIN, INPUT);

  Serial.println("\n=== POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.0) ===");
  
  // Expand Serial1 Buffer for smooth OTA downloading
  SerialAT.setRxBufferSize(8192);

  Wire.begin(21, 22); // I2C Pins for BME680
  bme.begin(0x77);

  // Initialize Cellular Modem
  if (!modem.begin()) while (1) delay(1000);
  
  // Wait for Network Registration
  bool netOk = false;
  for (int i = 0; i < 60 && !netOk; i++) { netOk = modem.waitForNetwork(1000); delay(1000); }
  if (!netOk) while (1) delay(1000);
  
  // Attach Data APN
  modem.attachData(apn.c_str());
  
  /* ----------------------------------------------------
   * AWS ENTERPRISE mTLS CONFIGURATION (MANDATORY)
   * ---------------------------------------------------- */
  sendATCommand("AT+QIACT=1", 10000); // Activate PDP Context
  sendATCommand("AT+QMTCFG=\"pdpcid\",0,1"); 
  sendATCommand("AT+QMTCFG=\"version\",0,4"); // MQTT V3.1.1
  sendATCommand("AT+QMTCFG=\"ssl\",0,1,0");   // Enable SSL for MQTT Context 0
  
  // SSL Configuration block
  sendATCommand("AT+QSSLCFG=\"sslversion\",0,4"); // Enforce TLS 1.2
  sendATCommand("AT+QSSLCFG=\"ciphersuite\",0,0xFFFF"); // Allow all ciphers
  sendATCommand("AT+QSSLCFG=\"ignorelocaltime\",0,1");  // Ignore time validation issues
  sendATCommand("AT+QSSLCFG=\"sni\",0,1"); // Server Name Indication (Required by AWS)
  
  // Attach Certificates from UFS (User Flash Storage in Modem)
  sendATCommand("AT+QSSLCFG=\"seclevel\",0,2"); // Mutual Authentication Level 2
  sendATCommand("AT+QSSLCFG=\"cacert\",0,\"UFS:rootCA.pem\"");
  sendATCommand("AT+QSSLCFG=\"clientcert\",0,\"UFS:cert.pem\"");
  sendATCommand("AT+QSSLCFG=\"clientkey\",0,\"UFS:privkey.pem\"");

  // Enable GNSS
  String gpsResp = sendATCommand("AT+QGPS=1", 3000);
  if (gpsResp.indexOf("OK") != -1) gpsEnabled = true;
  
  // Fetch Identity and connect to AWS
  collectDeviceInfo(); 
  connectAndVerify();
}

// ============ MAIN EVENT LOOP ============

unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectCooldown = 20000;

void loop() {
  // 1. Check if OTA firmware update is queued (Blocks execution if true)
  if (otaPending()) { otaRun(); return; }

  // 2. Maintain MQTT Connection
  if (!mqttConnected) {
    if (millis() - lastReconnectAttempt > reconnectCooldown) {
      lastReconnectAttempt = millis();
      connectAndVerify();
    }
    return;
  }

  // 3. Scan for incoming jobs/messages
  checkIncoming();

  // 4. Time-based Data Fetching
  if (millis() - lastGPSRead >= gpsReadInterval) { lastGPSRead = millis(); readGPS(); }
  if (millis() - lastDeviceInfoRead >= deviceInfoInterval) { lastDeviceInfoRead = millis(); collectDeviceInfo(); }

  // 5. AWS Data Publish Cycle
  if (millis() - lastUpload >= uploadInterval) {
    lastUpload = millis();
    
    // Base topic for AWS Rules Engine Routing
    String baseTopic = "bms/data/" + String(THING_NAME);

    // Format Telemetry Data Payload
    String locText = "=== Location ===\nLat: " + String(currentLocation.latitude, 6) + " | Lon: " + String(currentLocation.longitude, 6);
    
    // Publish to AWS IoT Core securely
    mqttPublish(baseTopic + "/telemetry", locText);

    Serial.println("[MQTT] Published Telemetry Data to AWS IoT Core");
  }
}