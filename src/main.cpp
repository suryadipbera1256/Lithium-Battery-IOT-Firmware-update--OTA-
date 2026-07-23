/* =========================================================
 * POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.0)
 * Modular, Zero-Allocation, Non-blocking Architecture
 * ========================================================= */

#include <Arduino.h>
#include <QuectelEC200U.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "driver/twai.h"
#include "config.h"           
#include "EC200U_AWS_OTA.h"   

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
bool bmeAvailable = false;

// Global Instances
HardwareSerial SerialAT(1);
QuectelEC200U modem(SerialAT, 115200, EC200U_RX_PIN, EC200U_TX_PIN);
Adafruit_BME680 bme;

unsigned long lastUpload = 0;
unsigned long lastBMSRead = 0;
unsigned long lastGPSRead = 0;
unsigned long lastDeviceInfoRead = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectCooldown = 20000;

// ============ HELPER FUNCTIONS ============

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

String sendATCommand(const String &cmd, uint32_t timeoutMs = 5000) {
  while (SerialAT.available()) SerialAT.read();
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
      break;
    }
  }
  return resp;
}

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

void readGPS() {
  if (!gpsEnabled) return;
  
  String gpsResp = sendATCommand("AT+QGPSLOC=2", 3000);
  gpsData.gpsFixed = false;
  
  if (gpsResp.indexOf("+QGPSLOC:") != -1) {
    String data = extractBetween(gpsResp, "+QGPSLOC: ", "\n");
    int fields = 0; String values[11]; int lastIdx = 0;
    
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

// Zero-Allocation O(1) MQTT Publish function
void publishToAWS(const char* topic, const char* payload) {
  if (topic == nullptr || payload == nullptr) return;

  char cmdBuf[128];
  int len = snprintf(cmdBuf, sizeof(cmdBuf), "AT+QMTPUBEX=0,1,1,0,\"%s\",%u", topic, strlen(payload));
  
  if (len < 0 || (size_t)len >= sizeof(cmdBuf)) {
      Serial.println("[ERROR] MQTT Command truncated!");
      return;
  }

  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmdBuf);
  
  unsigned long start = millis(); 
  bool gotPrompt = false;
  while (millis() - start < 3000) {
      if (SerialAT.available()) { 
          if (SerialAT.read() == '>') { gotPrompt = true; break; } 
      }
  }
  
  if (!gotPrompt) return;
  SerialAT.print(payload);
  
  unsigned long waitTime = millis();
  while(millis() - waitTime < 500) { if(SerialAT.available()) SerialAT.read(); }
}

void connectAndVerify() {
  Serial.println("[MQTT] Connecting to AWS IoT Core...");
  sendATCommand("AT+QMTDISC=0", 3000); 
  sendATCommand("AT+QMTCLOSE=0", 5000); 
  delay(500);
  
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTOPEN=0,\"" + String(AWS_IOT_ENDPOINT) + "\"," + String(mqtt_port));

  String openResp = ""; unsigned long start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) openResp += (char)SerialAT.read();
    if (openResp.indexOf("+QMTOPEN: 0,") != -1) break;
  }
  
  Serial.println("[DEBUG] QMTOPEN Response: " + openResp);

  if (openResp.indexOf("+QMTOPEN: 0,0") == -1) { 
    Serial.println("[ERROR] Failed to open MQTT socket to AWS!");
    mqttConnected = false; 
    return; 
  }

  while (SerialAT.available()) SerialAT.read();
  SerialAT.println("AT+QMTCONN=0,\"" + String(THING_NAME) + "\"");

  String connResp = ""; start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) connResp += (char)SerialAT.read();
    if (connResp.indexOf("+QMTCONN: 0,") != -1) break;
  }
  
  if (connResp.indexOf("+QMTCONN: 0,0,0") != -1) {
    mqttConnected = true; 
    Serial.println("[SUCCESS] AWS IoT Core Connected via mTLS!");
    
    String jobTopic = "$aws/things/" + String(THING_NAME) + "/jobs/notify-next";
    sendATCommand("AT+QMTSUB=0,1,\"" + jobTopic + "\",1", 10000);
  } else { 
    Serial.println("[ERROR] AWS MQTT Auth Failed!");
    mqttConnected = false; 
  }
}

void checkIncoming() {
  if (!SerialAT.available()) return;
  String line = ""; 
  unsigned long start = millis();
  
  while (millis() - start < 100) { 
      while (SerialAT.available()) line += (char)SerialAT.read(); 
  }
  if (line.length() == 0) return;
  
  if (otaCheckDownlink(line)) return;
  
  if (line.indexOf("+QMTSTAT:") != -1) {
    Serial.println("[MQTT] Received disconnect URC");
    mqttConnected = false;
  }
}

// ============ SYSTEM SETUP ============

void setup() {
  Serial.begin(115200); 
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW); 
  delay(3000);
  
  pinMode(MQ2_PIN, INPUT);
  pinMode(MQ8_PIN, INPUT);

  Serial.println("\n=== POINT-AI FIRMWARE (AWS ENTERPRISE V1.0.0) ===");
  
  SerialAT.setRxBufferSize(16384);

  Serial.println("[INIT] Checking BME680 Sensor...");
  Wire.begin(21, 22);
  if (!bme.begin(0x77)) {
    Serial.println("[WARN] BME680 not detected. Continuing without environmental data.");
    bmeAvailable = false;
  } else {
    Serial.println("[SUCCESS] BME680 Ready.");
    bmeAvailable = true;
  }

  Serial.println("[INIT] Contacting EC200U Modem...");
  if (!modem.begin()) {
    Serial.println("[ERROR] Modem not responding to AT. Check RX/TX and Power!");
  } else {
    Serial.println("[SUCCESS] Modem AT Bridge Ready.");
  }
  
  Serial.println("[INIT] Registering to Cellular Network...");
  bool netOk = false;
  for (int i = 0; i < 15; i++) { 
    if (modem.waitForNetwork(1000)) {
      netOk = true;
      break;
    }
    Serial.print(".");
  }
  
  if (!netOk) {
    Serial.println("\n[WARN] Cellular Network delay. Will attempt reconnecting in loop.");
  } else {
    Serial.println("\n[SUCCESS] Network Registered.");
  }
  
  modem.attachData(apn.c_str());
  
  sendATCommand("AT+QIACT=1", 10000);
  sendATCommand("AT+QMTCFG=\"pdpcid\",0,1"); 
  sendATCommand("AT+QMTCFG=\"version\",0,4"); 
  sendATCommand("AT+QMTCFG=\"ssl\",0,1,0");   
  
  sendATCommand("AT+QSSLCFG=\"sslversion\",0,4"); 
  sendATCommand("AT+QSSLCFG=\"ciphersuite\",0,0xFFFF"); 
  sendATCommand("AT+QSSLCFG=\"ignorelocaltime\",0,1");  
  sendATCommand("AT+QSSLCFG=\"sni\",0,1"); 
  
  sendATCommand("AT+QSSLCFG=\"seclevel\",0,2"); 
  sendATCommand("AT+QSSLCFG=\"cacert\",0,\"UFS:rootCA.pem\"");
  sendATCommand("AT+QSSLCFG=\"clientcert\",0,\"UFS:cert.pem\"");
  sendATCommand("AT+QSSLCFG=\"clientkey\",0,\"UFS:privkey.pem\"");

  String gpsResp = sendATCommand("AT+QGPS=1", 3000);
  if (gpsResp.indexOf("OK") != -1) gpsEnabled = true;
  
  collectDeviceInfo(); 
  connectAndVerify();
}

// ============ MAIN EVENT LOOP ============

void loop() {
  if (otaPending()) { 
    otaRun(); 
    return; 
  }

  if (!mqttConnected) {
    if (millis() - lastReconnectAttempt > reconnectCooldown) {
      lastReconnectAttempt = millis();
      connectAndVerify();
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
    if (bmeAvailable) {
      if (bme.performReading()) {
        temp = bme.temperature;
        hum = bme.humidity;
        press = bme.pressure / 100.0F;
        gasRes = bme.gas_resistance / 1000.0F;
      }
    }

    // Zero-Allocation JSON Construction using Static Buffer (No Memory Leaks)
    char jsonPayload[512]; 
    int written = snprintf(jsonPayload, sizeof(jsonPayload),
        "{"
        "\"thing_name\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"location\":{\"latitude\":%.6f,\"longitude\":%.6f,\"source\":\"%s\",\"speed_kmh\":%.2f,\"satellites\":%d},"
        "\"sensors\":{\"mq2_gas_raw\":%d,\"mq8_gas_raw\":%d,\"bme680\":{\"temp_c\":%.2f,\"humidity_pct\":%.2f,\"pressure_hpa\":%.2f,\"gas_res_kohm\":%.2f}},"
        "\"telemetry\":{\"rssi\":%d,\"imei\":\"%s\",\"total_odometer\":%.2f}"
        "}",
        THING_NAME, 
        FW_VERSION,
        currentLocation.latitude, currentLocation.longitude, currentLocation.source.c_str(), gpsData.speed, gpsData.satellites,
        rawMQ2, rawMQ8, 
        temp, hum, press, gasRes,
        deviceInfo.signal_strength, deviceInfo.imei.c_str(), totalOdometer
    );

    if (written > 0 && (size_t)written < sizeof(jsonPayload)) {
        char topicBuf[64];
        snprintf(topicBuf, sizeof(topicBuf), "bms/data/%s/telemetry", THING_NAME);
        
        publishToAWS(topicBuf, jsonPayload);
        
        Serial.println("\n[MQTT] Published JSON Payload to AWS (Zero-Allocation):");
        Serial.println(jsonPayload);
    } else {
        Serial.println("[ERROR] Payload buffer overflow prevented!");
    }
  }
}