#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Rolling in-RAM log ring, installed before anything else in setup().
 *
 * The WebSocket log sink can only carry what is logged after WiFi and the HTTP
 * server are up, several seconds into boot. Everything before that -- most
 * importantly the reset reason, which is logged within the first ~50 ms and is
 * the only record of *why* a device restarted -- is unobservable on a board with
 * no accessible serial port. This ring captures from the first log line and can
 * be read back over HTTP once the network exists.
 *
 * Rolling rather than boot-only: the reset reason answers "why did it restart",
 * but the lines immediately before a fault are what identify the code path, and
 * those are lost if capture stops once boot completes.
 *
 * Disabled by default. The buffer costs its full size in heap for the lifetime
 * of the device, so it is opt-in and sized by the user.
 */
namespace bootlog {

/**
 * Reads the configured size straight from NVS and, if non-zero, allocates the
 * ring and installs the log hook.
 *
 * Deliberately does not go through ConfigManager: the whole point is to be
 * running before ConfigManager deserialises its blob, since that happens after
 * the reset reason has already been logged. Reads a small mirror key written by
 * ConfigManager::saveConfig() instead.
 *
 * Safe to call unconditionally; a failure to allocate is silent and harmless.
 */
void initFromNvs();

/** True when a ring is allocated and capturing. */
bool enabled();

/** Configured size in bytes, or 0 when disabled. */
size_t capacity();

/** Bytes currently held (<= capacity()). */
size_t size();

/** Log lines dropped because the ring lock was contended. */
uint32_t dropped();

/** Bytes held in the frozen boot prologue. */
size_t prologueSize();

/** Bytes held in the rolling window. */
size_t windowSize();

/**
 * Copy a slice of the frozen prologue, oldest byte first.
 *
 * Chunked deliberately. Returning the whole buffer as a std::string required a
 * contiguous allocation the size of the ring, which at 32 KB failed against a
 * fragmented heap and aborted the device -- reading the diagnostic buffer
 * crashed the thing it was meant to diagnose.
 *
 * @return bytes copied, 0 at or past the end
 */
size_t readPrologue(size_t offset, char *out, size_t max);

/** As readPrologue(), over the rolling window. */
size_t readWindow(size_t offset, char *out, size_t max);

/** Discards buffered content, keeping the ring allocated and capturing. */
void clear();

/** NVS key mirroring misc_config_t::bootLogKb so it can be read early. */
inline constexpr const char* kNvsKey = "bootlog_kb";

}  // namespace bootlog
