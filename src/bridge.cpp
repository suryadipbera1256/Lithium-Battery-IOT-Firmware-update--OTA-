#include <Arduino.h>

#define EC200U_RX_PIN 16
#define EC200U_TX_PIN 17

HardwareSerial SerialAT(1);

void setup() {
  Serial.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, EC200U_RX_PIN, EC200U_TX_PIN);
  delay(2000);
  Serial.println("\n=== ESP32 AT BRIDGE READY ===");
  Serial.println("Now run the factory_flash.py script from terminal.");
}

void loop() {
  // PC থেকে আসা ডেটা মোডেমে পাঠানো
  while (Serial.available()) {
    SerialAT.write(Serial.read());
  }
  // মোডেম থেকে আসা ডেটা PC-তে পাঠানো
  while (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
}

/*
Connected to modem...
Sending: AT
Response: ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x17 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1184
load:0x40078000,len:13232
load:0x40080400,len:3028
entry 0x400805e4


--- Uploading rootCA.pem (1187 bytes) ---
+QFUPL: 1187,2d19

OK

Successfully uploaded rootCA.pem to UFS.

--- Uploading cert.pem (1220 bytes) ---
+QFUPL: 1220,231a

OK

Successfully uploaded cert.pem to UFS.

--- Uploading privkey.pem (1675 bytes) ---
+QFUPL: 1675,3b75

OK

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---

--- Uploading privkey.pem (1675 bytes) ---
+QFUPL: 1675,3b75

OK

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---
--- Uploading privkey.pem (1675 bytes) ---
+QFUPL: 1675,3b75

OK

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---

OK

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---
OK

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---

Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---
Successfully uploaded privkey.pem to UFS.

--- Verifying Uploaded Files in UFS ---

--- Verifying Uploaded Files in UFS ---
--- Verifying Uploaded Files in UFS ---
Sending: AT+QFLST
Response:
+QFLST: "UFS:boot",15004
+QFLST: "UFS:firm",286000
Sending: AT+QFLST
Response:
+QFLST: "UFS:boot",15004
+QFLST: "UFS:firm",286000
+QFLST: "UFS:gnss_data",4644
+QFLST: "UFS:gnss_loca",47
+QFLST: "UFS:boot",15004
+QFLST: "UFS:firm",286000
+QFLST: "UFS:gnss_data",4644
+QFLST: "UFS:gnss_loca",47
+QFLST: "UFS:gnss_data",4644
+QFLST: "UFS:gnss_loca",47
+QFLST: "UFS:gnss_time",21
+QFLST: "UFS:rootCA.pem",1187
+QFLST: "UFS:cert.pem",1220
+QFLST: "UFS:privkey.pem",1675
+QFLST: "UFS:cert.pem",1220
+QFLST: "UFS:privkey.pem",1675
+QFLST: "UFS:privkey.pem",1675

OK

Factory flashing process completed successfully.
*/