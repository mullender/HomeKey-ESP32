#pragma once

// Free-function facade for the Improv Serial state machine. main.cpp uses
// begin()/loop() rather than instantiating a StateMachine directly, so the
// facade owns a singleton with the on-target default clock. Off-target
// tests bypass this file entirely and drive the StateMachine directly.

#include "improv_state_machine.hpp"

namespace improv {

/**
 * Install as the sole Serial RX consumer for this boot.
 *
 * cfg.now_us may be left null; the facade fills in esp_timer_get_time.
 * cfg.io and cfg.wifi callbacks are the caller's responsibility.
 *
 * Sends an initial CurrentState(Authorized) packet so a client that attaches
 * immediately knows we are ready to accept WIFI_SETTINGS.
 */
void begin(const SerialConfig &cfg);

/**
 * Drain any bytes waiting on the transport and progress the state machine.
 * Non-blocking. Safe to call unconditionally from loop().
 */
void loop();

}  // namespace improv
