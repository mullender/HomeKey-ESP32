#pragma once

// Application-side adapter for Improv Serial.
//
// The Improv component itself deliberately knows nothing about HomeSpan or
// how this project persists Wi-Fi credentials. This file bridges the two:
//   - decides whether Improv should own Serial for this boot, from
//     HomeSpan's own "WIFI" / "WIFIDATA" NVS blob (the same criterion
//     HomeSpan itself uses to decide "provisioned" vs "needs config")
//   - once HomeSpan has begun, wires the Improv on_provisioned callback to
//     homeSpan.setWifiCredentials() and signals the main setup task via
//     xTaskNotifyGive so it can continue deferred hardware init in-process
//     (no reboot -- see improv_wait_for_provisioning below).
//
// The decision is made once, at boot, and cached. A transient AP outage
// after boot does NOT reopen credential replacement.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True iff Improv Serial should own Serial for this boot.
 *
 * Returns false when CONFIG_ENABLE_IMPROV_SERIAL is off, or when HomeSpan's
 * WIFIDATA blob is present with a non-empty ssid. Otherwise true.
 */
bool improv_should_own_serial(void);

/**
 * Install the Improv provisioning callback. Must be called AFTER
 * homeSpan.begin() so setWifiCredentials() takes effect through HomeSpan's
 * public API and the standard NVS write path.
 *
 * No-op when improv_should_own_serial() returned false.
 */
void improv_start_after_homespan_begin(void);

/**
 * Block the calling task until the Improv adapter signals a successful
 * provisioning (portMAX_DELAY, no busy-loop). Must be paired with a
 * prior improv_start_after_homespan_begin() from the same task, which
 * registers the notify target before the Improv task can complete.
 */
void improv_wait_for_provisioning(void);

#ifdef __cplusplus
}
#endif
