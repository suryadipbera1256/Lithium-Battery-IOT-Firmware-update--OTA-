#include <Arduino.h>

#define EC200U_RX_PIN 16
#define EC200U_TX_PIN 17

HardwareSerial SerialAT(1);

void setup() {
  Serial.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, EC200U_RX_PIN, EC200U_TX_PIN);
  // No print statements here to keep the UART stream 100% clean for Python
}

void loop() {
  while (Serial.available()) {
    SerialAT.write(Serial.read());
  }
  while (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
}