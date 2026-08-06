#include "bms_can_parser.h"
#include <string.h>

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

// ============ CAN / TWAI PROTOCOL ============
bool startCAN(uint32_t baud) {
  twai_driver_uninstall(); 
  delay(100);
  
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config;
  
  if (baud == 500000) {
    t_config = TWAI_TIMING_CONFIG_500KBITS(); 
  } else {
    t_config = TWAI_TIMING_CONFIG_250KBITS();
  }
  
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    if (twai_start() == ESP_OK) { 
      currentBaud = baud; 
      return true; 
    }
  }
  return false;
}

bool requestFrame(uint16_t id, uint8_t* buf) {
  // Sending the 0x5A request payload required by the BMS protocol
  twai_message_t tx;
  tx.identifier = id;
  tx.extd = 0;
  tx.rtr = 0;
  tx.data_length_code = 1;
  tx.data[0] = 0x5A;

  if (twai_transmit(&tx, pdMS_TO_TICKS(10)) != ESP_OK) return false;
  
  unsigned long t0 = millis();
  while (millis() - t0 < 150) {
    twai_message_t rx;
    if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) {
      // Validate response ID and minimum length for CRC payload
      if (rx.identifier == id && rx.data_length_code >= 4) {
        int payloadLen = rx.data_length_code - 2;
        uint16_t crcReceived = (rx.data[payloadLen] << 8) | rx.data[payloadLen + 1];
        
        if (crcReceived == calc_crc16(rx.data, payloadLen)) {
          memcpy(buf, rx.data, 8);
          return true;
        }
      }
    }
  }
  return false;
}

void readBMS() {
  if (!baudRateLocked) return;
  
  uint8_t buf[8];
  
  // 0x100: Voltage, Current, Residual Capacity
  if (requestFrame(0x100, buf)) {
    bmsData.packVoltage = ((buf[0]<<8)|buf[1]) / 100.0;
    bmsData.packCurrent = (int16_t)((buf[2]<<8)|buf[3]) / 100.0;
    bmsData.residualCapacity = ((buf[4]<<8)|buf[5]) / 10.0;
  }
  
  // 0x101: Full Capacity, Cycles, SOC
  if (requestFrame(0x101, buf)) {
    bmsData.fullCapacity = ((buf[0]<<8)|buf[1]) / 10.0;
    bmsData.cycles = (int16_t)((buf[2]<<8)|buf[3]);
    bmsData.soc = (buf[4]<<8)|buf[5];
  }
  
  // 0x102: Balance State & Protection Flags
  if (requestFrame(0x102, buf)) {
    bmsData.balanceState = (buf[0]<<8)|buf[1];
    bmsData.protectionFlags = (buf[4]<<8)|buf[5];
  }
  
  // 0x103: MOSFET States
  if (requestFrame(0x103, buf)) {
    uint16_t mos = (buf[0]<<8)|buf[1];
    bmsData.chMOSFET_Act = (mos & 0x01) != 0; // Bit 0
    bmsData.dchMOSFET_Act = (mos & 0x02) != 0; // Bit 1
  }
  
  // 0x104: Hardware Configuration
  if (requestFrame(0x104, buf)) {
    bmsData.batteryStrings = buf[0];
    bmsData.ntcCount = buf[1];
  }
  
  // 0x105 & 0x106: Temperatures (NTC)
  //
  // Only ntcCount sensors are populated; the remaining slots carry frame padding
  // that decodes to nonsense (a 6th slot was publishing ~3476 C). Gate on the
  // reported sensor count AND on a plausible range, mirroring what the cell
  // voltage parser below already does.
  {
    const uint8_t ntc = (bmsData.ntcCount > 0 && bmsData.ntcCount <= 6)
                        ? bmsData.ntcCount : 6;
    for (uint8_t i = ntc; i < 6; i++) bmsData.temp[i] = 0.0;

    for (int frame = 0; frame < 2; frame++) {
      if (!requestFrame(frame == 0 ? 0x105 : 0x106, buf)) continue;
      for (int i = 0; i < 3; i++) {
        uint8_t slot = (uint8_t)(frame * 3 + i);
        if (slot >= ntc) break;

        uint16_t raw = (buf[i*2] << 8) | buf[i*2+1];
        if (raw == 0) continue;
        float c = (raw - 2731) / 10.0;
        if (c >= -40.0 && c <= 125.0) bmsData.temp[slot] = c;
      }
    }
  }
  
  // 0x107 to 0x110: Individual Cell Voltages
  bmsData.cellCount = 0; 
  bmsData.minCellV = 999.0; 
  bmsData.maxCellV = 0.0; 
  float totalCellV = 0.0;
  
  for (uint16_t id = 0x107; id <= 0x110; id++) {
    if (requestFrame(id, buf)) {
      for (int i = 0; i < 3; i++) {
        if (bmsData.batteryStrings > 0 && bmsData.cellCount >= bmsData.batteryStrings) break;
        if (bmsData.cellCount >= 32) break; // Maximum array bound check
        
        uint16_t cellVoltage = (buf[i*2] << 8) | buf[i*2+1];
        if (cellVoltage > 0 && cellVoltage <= 4500) { 
          float v = cellVoltage / 1000.0;
          bmsData.cellV[bmsData.cellCount] = v;
          totalCellV += v;
          if (v < bmsData.minCellV) bmsData.minCellV = v;
          if (v > bmsData.maxCellV) bmsData.maxCellV = v;
          bmsData.cellCount++;
        }
      }
    }
  }
  
  if (bmsData.cellCount > 0) { 
    bmsData.avgCellV = totalCellV / bmsData.cellCount; 
    bmsData.cellDeltaV = bmsData.maxCellV - bmsData.minCellV; 
  }
  bmsData.lastUpdate = millis();
}