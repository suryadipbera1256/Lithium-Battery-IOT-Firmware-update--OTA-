#include "bms_can_parser.h"
#include <string.h>

/* A silent bus otherwise costs 17 requests x 150 ms = 2.55 s of blocking inside
 * a 5 s cycle — a 51% duty cycle that starves the loop, the AT bus and (from
 * Phase G) the SMS channel. Abort the sweep once the bus looks dead, but only
 * on CONSECUTIVE misses so isolated noise never truncates a healthy read. */
#define BMS_MAX_CONSEC_FAIL 4

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

/* Sweep-scoped wrapper around requestFrame(). Counts successes so the caller
 * can tell a real read from a dead bus, and short-circuits the remaining
 * requests once BMS_MAX_CONSEC_FAIL consecutive frames have gone unanswered. */
static bool _bmsAsk(uint16_t id, uint8_t* buf, uint8_t* okFrames, uint8_t* consecFail) {
  if (*consecFail >= BMS_MAX_CONSEC_FAIL) return false;
  if (requestFrame(id, buf)) {
    (*okFrames)++;
    *consecFail = 0;
    return true;
  }
  (*consecFail)++;
  return false;
}

/* TWAI_MODE_NORMAL requires an acknowledger. With the harness unplugged every
 * frame goes unacknowledged, the transmit error counter climbs, and the
 * controller walks error-active -> error-passive (TEC 128) -> BUS_OFF (TEC 255).
 * Once bus-off the driver stops transmitting permanently: a pack that dies after
 * a successful lock would never be read again, and telemetry would keep
 * publishing the last values with a healthy-looking cloud path. Neither the
 * watchdog nor the cloud supervisor can see this, so recovery is handled here.
 *
 * Recovery is asynchronous — the driver lands in STOPPED and needs an explicit
 * restart, so both states are handled and the sweep is skipped for one cycle. */
static bool _canBusHealthy() {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return false;

  if (st.state == TWAI_STATE_BUS_OFF) {
    Serial.println("[CAN] BUS_OFF detected — initiating recovery.");
    twai_initiate_recovery();
    return false;
  }
  if (st.state == TWAI_STATE_STOPPED) {
    Serial.println("[CAN] Driver stopped after recovery — restarting.");
    twai_start();
    return false;
  }
  return st.state == TWAI_STATE_RUNNING;
}

void readBMS() {
  if (!baudRateLocked) return;
  if (!_canBusHealthy()) return;

  uint8_t buf[8];
  uint8_t okFrames   = 0;
  uint8_t consecFail = 0;

  // 0x100: Voltage, Current, Residual Capacity
  if (_bmsAsk(0x100, buf, &okFrames, &consecFail)) {
    bmsData.packVoltage = ((buf[0]<<8)|buf[1]) / 100.0;
    bmsData.packCurrent = (int16_t)((buf[2]<<8)|buf[3]) / 100.0;
    bmsData.residualCapacity = ((buf[4]<<8)|buf[5]) / 10.0;
  }

  // 0x101: Full Capacity, Cycles, SOC
  if (_bmsAsk(0x101, buf, &okFrames, &consecFail)) {
    bmsData.fullCapacity = ((buf[0]<<8)|buf[1]) / 10.0;
    bmsData.cycles = (int16_t)((buf[2]<<8)|buf[3]);
    bmsData.soc = (buf[4]<<8)|buf[5];
  }

  // 0x102: Balance State & Protection Flags
  if (_bmsAsk(0x102, buf, &okFrames, &consecFail)) {
    bmsData.balanceState = (buf[0]<<8)|buf[1];
    bmsData.protectionFlags = (buf[4]<<8)|buf[5];
  }

  // 0x103: MOSFET States
  if (_bmsAsk(0x103, buf, &okFrames, &consecFail)) {
    uint16_t mos = (buf[0]<<8)|buf[1];
    bmsData.chMOSFET_Act = (mos & 0x01) != 0; // Bit 0
    bmsData.dchMOSFET_Act = (mos & 0x02) != 0; // Bit 1
  }

  // 0x104: Hardware Configuration
  if (_bmsAsk(0x104, buf, &okFrames, &consecFail)) {
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
      if (!_bmsAsk(frame == 0 ? 0x105 : 0x106, buf, &okFrames, &consecFail)) continue;
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

  /* 0x107 to 0x110: Individual Cell Voltages.
   *
   * Parsed into locals and committed only on success. Writing straight into
   * bmsData meant a failed sweep left minCellV at its 999.0 sentinel and
   * cellCount at 0, and those were published as live battery readings. Holding
   * the previous values instead lets the age_s / valid staleness marker do its
   * job — the consumer sees the last real reading, correctly labelled stale. */
  {
    float   cells[32];
    uint8_t cellCount = 0;
    float   minV = 999.0, maxV = 0.0, totalV = 0.0;

    for (uint16_t id = 0x107; id <= 0x110; id++) {
      if (!_bmsAsk(id, buf, &okFrames, &consecFail)) continue;
      for (int i = 0; i < 3; i++) {
        if (bmsData.batteryStrings > 0 && cellCount >= bmsData.batteryStrings) break;
        if (cellCount >= 32) break; // Maximum array bound check

        uint16_t cellVoltage = (buf[i*2] << 8) | buf[i*2+1];
        if (cellVoltage > 0 && cellVoltage <= 4500) {
          float v = cellVoltage / 1000.0;
          cells[cellCount++] = v;
          totalV += v;
          if (v < minV) minV = v;
          if (v > maxV) maxV = v;
        }
      }
    }

    if (cellCount > 0) {
      memcpy(bmsData.cellV, cells, (size_t)cellCount * sizeof(float));
      bmsData.cellCount  = cellCount;
      bmsData.minCellV   = minV;
      bmsData.maxCellV   = maxV;
      bmsData.avgCellV   = totalV / cellCount;
      bmsData.cellDeltaV = maxV - minV;
    }
  }

  /* Stamped ONLY when the sweep actually parsed something. Stamping
   * unconditionally made age_s report 0 and valid report 1 the instant the CAN
   * harness was pulled, presenting frozen voltages as live — the exact failure
   * the staleness marker exists to catch. */
  if (okFrames > 0) bmsData.lastUpdate = millis();
}
