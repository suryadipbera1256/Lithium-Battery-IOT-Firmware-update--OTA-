#include "QuectelEC200U.h"

QuectelEC200U::QuectelEC200U(HardwareSerial& serial, uint32_t baud, int rx, int tx) 
    : _serial(serial), _baud(baud), _rxPin(rx), _txPin(tx) {}

String QuectelEC200U::sendCommand(const String& cmd, uint32_t timeoutMs) {
    while (_serial.available()) _serial.read();
    _serial.println(cmd);
    
    String response = "";
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (_serial.available()) {
            response += (char)_serial.read();
        }
        if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) {
            delay(50);
            break;
        }
    }
    return response;
}

bool QuectelEC200U::begin() {
    _serial.begin(_baud, SERIAL_8N1, _rxPin, _txPin);
    delay(1000);
    
    // Check if modem is responsive
    for(int i = 0; i < 5; i++) {
        String resp = sendCommand("AT");
        if(resp.indexOf("OK") != -1) return true;
        delay(1000);
    }
    return false;
}

bool QuectelEC200U::waitForNetwork(uint32_t timeoutMs) {
    String resp = sendCommand("AT+CREG?", timeoutMs);
    if (resp.indexOf("+CREG: 0,1") != -1 || resp.indexOf("+CREG: 0,5") != -1) {
        return true;
    }
    return false;
}

bool QuectelEC200U::attachData(const char* apn) {
    String cmd = String("AT+QICSGP=1,1,\"") + apn + "\"";
    String resp = sendCommand(cmd, 3000);
    return (resp.indexOf("OK") != -1);
}