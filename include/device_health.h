#pragma once
/* =========================================================
 * device_health.h — field survivability
 *
 * Three independent safety nets, cheapest first:
 *
 *  1. HANG WATCHDOG. A separate high-priority task watches a heartbeat that
 *     the main loop stamps. If the loop stops stamping it, the task reboots
 *     the device. Deliberately NOT esp_task_wdt: the Arduino core already
 *     initialises the TWDT at 5 s and esp_task_wdt_init() cannot lower that
 *     once initialised, so subscribing the loop task would reboot on any AT
 *     command taking over 5 s. An independent task gives real hang recovery
 *     with no dependency on the core's TWDT configuration.
 *
 *  2. CLOUD SUPERVISOR. If nothing has reached AWS for a long time the modem
 *     is reset (AT+CFUN=1,1); if that does not help, the device restarts.
 *     Runs in loop context because it needs the AT bus.
 *
 *  3. BOOT-HEALTH ROLLBACK. A freshly OTA'd image is on probation: each boot
 *     increments a counter, and the first successful AWS connection clears it.
 *     If it never connects across several boots, the previous bank is made
 *     bootable again. This replaces the bootloader's PENDING_VERIFY rollback,
 *     which does not engage with the stock precompiled Arduino bootloader.
 * ========================================================= */

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

extern HardwareSerial SerialAT;
extern bool           mqttConnected;

#ifndef HEALTH_HANG_MS
#define HEALTH_HANG_MS          180000UL   // loop silent this long -> reboot
#endif
#ifndef HEALTH_MODEM_RESET_MS
#define HEALTH_MODEM_RESET_MS   600000UL   // 10 min no cloud -> reset modem
#endif
#ifndef HEALTH_REBOOT_MS
#define HEALTH_REBOOT_MS        1200000UL  // 20 min no cloud -> reboot
#endif
#ifndef HEALTH_MAX_BAD_BOOTS
#define HEALTH_MAX_BAD_BOOTS    3          // probation boots before reverting
#endif
#define HEALTH_NVS_NS           "health"

static volatile unsigned long _hHeartbeat      = 0;
static volatile bool          _hArmed          = false;
static unsigned long          _hLastCloudOk    = 0;
static bool                   _hModemResetDone = false;
static uint32_t               _hReconnects     = 0;
static bool                   _hOnProbation    = false;

// ---- diagnostics accessors (published in telemetry) ----

inline const char* healthResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:                return "unknown";
  }
}

inline uint32_t healthUptimeS()    { return (uint32_t)(millis() / 1000UL); }
inline uint32_t healthReconnects() { return _hReconnects; }
inline bool     healthProbation()  { return _hOnProbation; }
inline void     healthCountReconnect() { _hReconnects++; }

/* Largest allocatable block vs total free IS the fragmentation metric. If free
 * stays flat while largest drifts down, the heap is fragmenting. */
inline uint32_t healthHeapFree()    { return ESP.getFreeHeap(); }
inline uint32_t healthHeapLargest() { return ESP.getMaxAllocHeap(); }

// Stamp the heartbeat. Safe to call from anywhere, including inside the OTA
// download loop, which owns the CPU for minutes at a time.
inline void healthAlive() { _hHeartbeat = millis(); }

// ---- 1. hang watchdog ----

static void _healthWatchdogTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Stays disarmed until loop() runs once. setup() can legitimately block for
    // over a minute (network registration, CAN probing, MQTT handshake) and must
    // never be mistaken for a hang.
    if (!_hArmed) continue;
    unsigned long hb = _hHeartbeat;
    if (hb != 0 && (millis() - hb) > HEALTH_HANG_MS) {
      Serial.println("\n[HEALTH] Main loop stalled — rebooting.");
      Serial.flush();
      delay(50);
      esp_restart();
    }
  }
}

// ---- 3. boot-health rollback ----

static void _healthRevertBank() {
  const esp_partition_t* prev = esp_ota_get_next_update_partition(NULL);
  if (!prev) { Serial.println("[HEALTH] No alternate bank to revert to."); return; }

  esp_err_t e = esp_ota_set_boot_partition(prev);
  if (e != ESP_OK) {
    Serial.printf("[HEALTH] Revert refused: %s (other bank not a valid image)\n",
                  esp_err_to_name(e));
    return;
  }
  Serial.printf("[HEALTH] Reverting to %s and rebooting.\n", prev->label);
  Serial.flush();
  delay(200);
  esp_restart();
}

/* Call first in setup(). */
inline void healthBegin() {
  healthAlive();
  _hLastCloudOk = millis();

  xTaskCreatePinnedToCore(_healthWatchdogTask, "health_wdt", 2048, NULL, 2, NULL, 1);

  Preferences p;
  if (!p.begin(HEALTH_NVS_NS, false)) return;

  _hOnProbation = p.isKey("fresh") && p.getBool("fresh", false);
  if (!_hOnProbation) { p.end(); return; }

  int bootc = (p.isKey("bootc") ? p.getInt("bootc", 0) : 0) + 1;
  p.putInt("bootc", bootc);
  bool alreadyReverted = p.isKey("rvt") && p.getBool("rvt", false);

  Serial.printf("[HEALTH] New image on probation, boot %d/%d\n",
                bootc, HEALTH_MAX_BAD_BOOTS);

  if (bootc > HEALTH_MAX_BAD_BOOTS && !alreadyReverted) {
    // Never revert twice: if the old bank is broken too, ping-ponging banks is
    // worse than staying here and retrying the network forever.
    p.putBool("rvt", true);
    p.putBool("fresh", false);
    p.putInt("bootc", 0);
    p.end();
    Serial.println("[HEALTH] Image failed to reach AWS across several boots.");
    _healthRevertBank();
    return;
  }
  p.end();
}

/* Call on every successful AWS IoT connection. Ends probation. */
inline void healthOnCloudSuccess() {
  _hLastCloudOk    = millis();
  _hModemResetDone = false;
  healthAlive();

  if (!_hOnProbation) return;
  _hOnProbation = false;

  Preferences p;
  if (p.begin(HEALTH_NVS_NS, false)) {
    p.putBool("fresh", false);
    p.putInt("bootc", 0);
    p.putBool("rvt", false);       // re-arm for the next OTA
    p.end();
  }

  // Harmless no-op unless the bootloader actually staged PENDING_VERIFY.
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }
  Serial.println("[HEALTH] Image confirmed healthy — probation cleared.");
}

/* Call from the OTA path right after a successful flash, before rebooting, so
 * the incoming image starts on probation. */
inline void healthMarkFreshImage() {
  Preferences p;
  if (p.begin(HEALTH_NVS_NS, false)) {
    p.putBool("fresh", true);
    p.putInt("bootc", 0);
    p.end();
  }
}

/* Call at the top of loop(), every iteration. */
inline void healthTick() {
  healthAlive();
  if (!_hArmed) {
    _hArmed = true;
    _hLastCloudOk = millis();   // don't count setup time as cloud silence
    Serial.println("[HEALTH] Hang watchdog armed.");
  }

  unsigned long silent = millis() - _hLastCloudOk;

  if (silent > HEALTH_REBOOT_MS) {
    Serial.println("\n[HEALTH] No cloud contact after a modem reset — rebooting.");
    Serial.flush();
    delay(50);
    esp_restart();
  }

  if (silent > HEALTH_MODEM_RESET_MS && !_hModemResetDone) {
    _hModemResetDone = true;
    Serial.println("\n[HEALTH] No cloud contact — resetting modem (AT+CFUN=1,1).");
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println("AT+CFUN=1,1");
    mqttConnected = false;
    delay(5000);          // modem reboots; the reconnect path takes it from here
  }
}

/* OTA takes minutes and deliberately drops MQTT. Call around it so neither
 * supervisor mistakes a download for a hang. */
inline void healthNoteOtaActivity() {
  healthAlive();
  _hLastCloudOk = millis();
}
