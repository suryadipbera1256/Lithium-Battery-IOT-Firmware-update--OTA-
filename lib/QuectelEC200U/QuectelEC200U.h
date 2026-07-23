#pragma once
#include <Arduino.h>

class QuectelEC200U {
private:
    HardwareSerial& _serial;
    uint32_t _baud;
    int _rxPin;
    int _txPin;
    
public:
    QuectelEC200U(HardwareSerial& serial, uint32_t baud, int rx, int tx);
    bool begin();
    bool waitForNetwork(uint32_t timeoutMs = 10000);
    bool attachData(const char* apn);
    
    // O(1) Memory Safe Command Function
    void sendCommandRaw(const char* cmd, char* outBuffer, size_t bufferSize, uint32_t timeoutMs = 2000);
};