// Thin ESP-side singleton facade over improv::StateMachine, so main.cpp
// keeps a couple of free functions to include rather than a class handle.
// The clock defaults to esp_timer_get_time on target; tests instantiate the
// state machine directly with their own clock (see test_state_machine.cpp).

#include "sdkconfig.h"

#if CONFIG_ENABLE_IMPROV_SERIAL

#include "improv_serial.hpp"
#include "improv_state_machine.hpp"

#include <esp_timer.h>

namespace improv {
namespace {
StateMachine g_sm;
}  // namespace

void begin(const SerialConfig &cfg) {
    SerialConfig with_clock = cfg;
    if (!with_clock.now_us) {
        with_clock.now_us = []() -> int64_t { return esp_timer_get_time(); };
    }
    g_sm.begin(with_clock);
}

void loop() {
    g_sm.loop();
}

}  // namespace improv

#endif  // CONFIG_ENABLE_IMPROV_SERIAL
