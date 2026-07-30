#include "QuectelEC200U.h"

QuectelEC200U::QuectelEC200U(HardwareSerial& serial, uint32_t baud, int rx, int tx) 
    : _serial(serial), _baud(baud), _rxPin(rx), _txPin(tx) {}

// Zero-Allocation implementation
void QuectelEC200U::sendCommandRaw(const char* cmd, char* outBuffer, size_t bufferSize, uint32_t timeoutMs) {
    if (outBuffer == nullptr || bufferSize == 0) return;

    while (_serial.available()) _serial.read();
    _serial.println(cmd);
    
    memset(outBuffer, 0, bufferSize);
    size_t idx = 0;
    unsigned long start = millis();
    
    while (millis() - start < timeoutMs) {
        while (_serial.available() && idx < bufferSize - 1) {
            outBuffer[idx++] = (char)_serial.read();
        }
        if (strstr(outBuffer, "OK\r\n") != nullptr || strstr(outBuffer, "ERROR\r\n") != nullptr) {
            delay(50); // Allow final bytes to land
            while (_serial.available() && idx < bufferSize - 1) {
                outBuffer[idx++] = (char)_serial.read();
            }
            break;
        }
    }
}

bool QuectelEC200U::begin() {
    _serial.begin(_baud, SERIAL_8N1, _rxPin, _txPin);
    delay(1000);
    
    char respBuf[64];
    for(int i = 0; i < 5; i++) {
        sendCommandRaw("AT", respBuf, sizeof(respBuf), 2000);
        if(strstr(respBuf, "OK") != nullptr) return true;
        delay(1000);
    }
    return false;
}

bool QuectelEC200U::waitForNetwork(uint32_t timeoutMs) {
    char respBuf[128];
    sendCommandRaw("AT+CREG?", respBuf, sizeof(respBuf), timeoutMs);
    if (strstr(respBuf, "+CREG: 0,1") != nullptr || strstr(respBuf, "+CREG: 0,5") != nullptr) {
        return true;
    }
    return false;
}

bool QuectelEC200U::attachData(const char* apn) {
    char cmdBuf[64];
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+QICSGP=1,1,\"%s\"", apn);
    
    char respBuf[128];
    sendCommandRaw(cmdBuf, respBuf, sizeof(respBuf), 3000);
    return (strstr(respBuf, "OK") != nullptr);
}