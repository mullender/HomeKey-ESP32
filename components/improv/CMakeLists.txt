# Improv Serial component.
#
# improv_protocol.cpp is pure C++ and host-buildable (see test/run_tests.sh).
# improv_serial.cpp is the ESP-side state machine, but byte I/O and Wi-Fi are
# caller-supplied callbacks -- so this component has no Arduino / HomeSpan /
# esp_wifi dependency and stays reusable and off-target testable.
#
# Both sources are registered unconditionally; improv_serial.cpp guards its
# body with #if CONFIG_ENABLE_IMPROV_SERIAL. Conditional idf_component_register
# calls are unreliable in ESP-IDF's bootstrap phase because CONFIG_* values
# are not yet applied when the register call is evaluated.

idf_component_register(
    SRCS improv_protocol.cpp improv_state_machine.cpp improv_serial.cpp
    INCLUDE_DIRS include
    PRIV_REQUIRES esp_timer log
)
