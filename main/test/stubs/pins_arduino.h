// Minimal stub of Arduino's pins_arduino.h so main/include/defaults.h can
// be included by host tests. defaults.h consumes SS/MISO/MOSI/SCK to build
// the generic NFC SPI pin defaults; those specific values are never
// asserted on -- the installer-defaults tests only check macros the
// installer variant actually overrides. Values here are arbitrary but
// plausible for an ESP32-class board so a curious reader isn't misled.
#pragma once

#ifndef SS
#define SS 5
#endif
#ifndef MISO
#define MISO 19
#endif
#ifndef MOSI
#define MOSI 23
#endif
#ifndef SCK
#define SCK 18
#endif
