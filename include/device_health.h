#pragma once
/* =========================================================
 * device_health.h — field survivability (INTERFACE)
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
 *
 * STATE OWNERSHIP: every mutable field lives in device_health.cpp, and this
 * header exposes declarations only. It previously carried its state in
 * file-scope statics, which meant a second translation unit including it got an
 * independent copy — the watchdog task would then watch a heartbeat that the
 * main loop never stamped, and reboot a perfectly healthy device every 180 s.
 * Any number of translation units may now include this safely.
 * ========================================================= */

#include <Arduino.h>

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

// ---- diagnostics accessors (published in telemetry) ----

const char* healthResetReason();
uint32_t    healthUptimeS();
uint32_t    healthReconnects();
bool        healthProbation();
void        healthCountReconnect();

/* Largest allocatable block vs total free IS the fragmentation metric. If free
 * stays flat while largest drifts down, the heap is fragmenting. */
uint32_t healthHeapFree();
uint32_t healthHeapLargest();

/* Stamp the heartbeat. Safe to call from anywhere, including inside the OTA
 * download loop, which owns the CPU for minutes at a time. */
void healthAlive();

/* Call first in setup(). Starts the watchdog task (disarmed until loop() runs
 * once) and evaluates boot-health probation. */
void healthBegin();

/* Call on every successful AWS IoT connection. Ends probation. */
void healthOnCloudSuccess();

/* Call from the OTA path right after a successful flash, before rebooting, so
 * the incoming image starts on probation. */
void healthMarkFreshImage();

/* Call at the top of loop(), every iteration. */
void healthTick();

/* OTA takes minutes and deliberately drops MQTT. Call around it so neither
 * supervisor mistakes a download for a hang. */
void healthNoteOtaActivity();
