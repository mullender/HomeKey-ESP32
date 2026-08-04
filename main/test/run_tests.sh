#!/usr/bin/env bash
# Host-side tests for the application-side helpers that need to be tested
# outside the ESP-IDF target build. Currently:
#   - the WIFIDATA blob shape classifier that drives the Improv boot decision
#   - the pure NFC reader-type range validator used by WebServerManager's
#     captive-portal config HTTP handler

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

echo "==> Compiling with $CXX"
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$main/include" \
  "$main/homespan_wifidata_check.cpp" "$here/test_homespan_wifidata_check.cpp" \
  -o "$BUILD/test_homespan_wifidata_check"

"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$main/include" \
  "$here/test_nfc_reader_type.cpp" \
  -o "$BUILD/test_nfc_reader_type"

echo "==> Running"
"$BUILD/test_homespan_wifidata_check"
"$BUILD/test_nfc_reader_type"
