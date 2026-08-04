// Installer/generic defaults regression. Compiled twice:
//   - without a flag: the generic firmware defaults must hold.
//   - with -DCONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS=1: the AtomS3 Lite
//     Pages-installer overrides in main/include/defaults.h must apply
//     (matches sdkconfig.defaults.installer.atoms3 on target).
// All assertions are static_assert: a drift in either configuration
// fails compilation, so the test binary never links.

#include "defaults.h"

#if defined(CONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS)
static_assert(NFC_READER_TYPE   == 2,   "installer NFC_READER_TYPE must be 2 (ST25R3916)");
static_assert(NFC_ACTIVE_PRESET == 5,   "installer NFC_ACTIVE_PRESET must index the AtomS3 Lite preset (5)");
static_assert(NFC_NEOPIXEL_PIN  == 35,  "installer NFC_NEOPIXEL_PIN must be GPIO 35 (AtomS3 Lite internal RGB LED)");
static_assert(NEOPIXEL_TYPE     == 5,   "installer NEOPIXEL_TYPE must be 5 (GRB) -- unchanged from generic, must not drift");
static_assert(HS_STATUS_LED     == 255, "installer HS_STATUS_LED must stay 255 -- HomeSpan status LED would collide with Pixel on GPIO 35");
#else
static_assert(NFC_READER_TYPE   == 0,   "generic NFC_READER_TYPE must be 0 (PN532)");
static_assert(NFC_ACTIVE_PRESET == 255, "generic NFC_ACTIVE_PRESET must be 255 (custom)");
static_assert(NFC_NEOPIXEL_PIN  == 255, "generic NFC_NEOPIXEL_PIN must be 255 (no LED)");
static_assert(NEOPIXEL_TYPE     == 5,   "generic NEOPIXEL_TYPE must be 5 (GRB)");
static_assert(HS_STATUS_LED     == 255, "generic HS_STATUS_LED must be 255 (no HomeSpan status LED)");
#endif

int main() { return 0; }
