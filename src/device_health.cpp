/* =========================================================
 * device_health.cpp — field survivability (STATE + IMPLEMENTATION)
 *
 * Sole owner of the health state. See device_health.h for the architecture and
 * for why this state is NOT in the header.
 * ========================================================= */

#include "device_health.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

// ---- state: file-local to this translation unit, by design ----

static volatile unsigned long _hHeartbeat      = 0;
static volatile bool          _hArmed          = false;
static unsigned long          _hLastCloudOk    = 0;
static bool                   _hModemResetDone = false;
static uint32_t               _hReconnects     = 0;
static bool                   _hOnProbation    = false;

// ---- diagnostics accessors ----

const char* healthResetReason() {
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

uint32_t healthUptimeS()       { return (uint32_t)(millis() / 1000UL); }
uint32_t healthReconnects()    { return _hReconnects; }
bool     healthProbation()     { return _hOnProbation; }
void     healthCountReconnect(){ _hReconnects++; }

uint32_t healthHeapFree()    { return ESP.getFreeHeap(); }
uint32_t healthHeapLargest() { return ESP.getMaxAllocHeap(); }

void healthAlive() { _hHeartbeat = millis(); }

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

void healthBegin() {
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

void healthOnCloudSuccess() {
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

void healthMarkFreshImage() {
  Preferences p;
  if (p.begin(HEALTH_NVS_NS, false)) {
    p.putBool("fresh", true);
    p.putInt("bootc", 0);
    p.end();
  }
}

void healthTick() {
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

void healthNoteOtaActivity() {
  healthAlive();
  _hLastCloudOk = millis();
}
