#!/usr/bin/env bash
# Host-side tests for the application-side helpers that need to be tested
# outside the ESP-IDF target build. Currently:
#   - the WIFIDATA blob shape classifier that drives the Improv boot decision
#   - the pure NFC reader-type range validator used by WebServerManager's
#     captive-portal config HTTP handler
#   - installer/generic defaults in main/include/defaults.h, asserted via
#     static_assert so a drift in the CONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS
#     override or in the generic values fires at compile time

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
main="$(cd "$here/.." && pwd)"

CXX="${CXX:-$(command -v clang++ || command -v g++)}"
if [ -z "$CXX" ]; then
  echo "No C++ compiler found." >&2
  exit 1
fi

BUILD="$here/build"
mkdir -p "$BUILD"

# Stubs directory is put FIRST on the include path so main/include/defaults.h
# resolves its unconditional `#include "sdkconfig.h"` to the empty stub in
# main/test/stubs. Same include semantics as the on-target build (which
# resolves sdkconfig.h out of the ESP-IDF build tree); nothing in defaults.h
# had to change to make it host-compilable.
STUBS="$here/stubs"

echo "==> Compiling with $CXX"
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$main/include" \
  "$main/homespan_wifidata_check.cpp" "$here/test_homespan_wifidata_check.cpp" \
  -o "$BUILD/test_homespan_wifidata_check"

"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$main/include" \
  "$here/test_nfc_reader_type.cpp" \
  -o "$BUILD/test_nfc_reader_type"

# One test source, compiled twice: generic (no flag) and installer
# (-DCONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS=1, matching what
# sdkconfig.defaults.installer.atoms3 flips on target). All assertions are
# static_assert -- if either compile fires, the binary never links.
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$STUBS" -I"$main/include" \
  "$here/test_installer_defaults.cpp" \
  -o "$BUILD/test_installer_defaults_generic"

"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$STUBS" -I"$main/include" \
  -DCONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS=1 \
  "$here/test_installer_defaults.cpp" \
  -o "$BUILD/test_installer_defaults_installer"

echo "==> Running"
"$BUILD/test_homespan_wifidata_check"
"$BUILD/test_nfc_reader_type"
"$BUILD/test_installer_defaults_generic" && echo "  ok  generic defaults static_asserts hold"
"$BUILD/test_installer_defaults_installer" && echo "  ok  installer defaults static_asserts hold"
