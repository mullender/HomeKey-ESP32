#!/usr/bin/env bash
# Host-side tests for the Improv Serial parser and framer.
#
# Deliberately no CMake, no test framework, no dependencies. This has to run
# from a plain shell on Linux and macOS with whatever C++ compiler is around.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
comp="$(cd "$here/.." && pwd)"

# Prefer clang++ (macOS default), fall back to g++.
CXX="${CXX:-$(command -v clang++ || command -v g++)}"
if [ -z "$CXX" ]; then
  echo "No C++ compiler found." >&2
  exit 1
fi

BUILD="$here/build"
mkdir -p "$BUILD"

echo "==> Compiling test_protocol"
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$comp/include" \
  "$comp/improv_protocol.cpp" "$here/test_protocol.cpp" \
  -o "$BUILD/test_protocol"

echo "==> Compiling test_state_machine"
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$comp/include" \
  "$comp/improv_protocol.cpp" "$comp/improv_state_machine.cpp" \
  "$here/test_state_machine.cpp" \
  -o "$BUILD/test_state_machine"

echo "==> Compiling test_wire_compat"
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O0 -g \
  -I"$comp/include" \
  "$comp/improv_protocol.cpp" "$here/test_wire_compat.cpp" \
  -o "$BUILD/test_wire_compat"

echo "==> Running test_protocol"
"$BUILD/test_protocol"

echo
echo "==> Running test_state_machine"
"$BUILD/test_state_machine"

echo
echo "==> Running test_wire_compat"
"$BUILD/test_wire_compat"
