// Bridges the Improv Serial component to this project's HomeSpan-based
// persistence, in-process handoff to the main setup task, and Arduino
// Serial transport. Kept out of the component so improv/ has zero
// application-specific dependencies.

#include "improv_boot_adapter.hpp"

#include "sdkconfig.h"

#if CONFIG_ENABLE_IMPROV_SERIAL

#include <Arduino.h>
#include <HomeSpan.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "homespan_wifidata_check.hpp"
#include "improv_serial.hpp"

#include <algorithm>
#include <cstring>

namespace {

constexpr const char *TAG = "improv_adapter";

// Cache the boot-time decision. Read once, in improv_should_own_serial(),
// and never re-evaluated -- a transient AP outage after boot must NOT
// silently reopen credential replacement.
bool g_decided = false;
bool g_owns    = false;
TaskHandle_t g_task = nullptr;

// Main-setup TaskHandle registered by improv_start_after_homespan_begin
// BEFORE the Improv task is created, so a completion notify cannot race
// ahead of the handle being visible. Task notifications are sticky, so
// a notify that arrives before ulTaskNotifyTake is entered is preserved.
TaskHandle_t g_main_task_to_notify = nullptr;

// Read HomeSpan's persisted Wi-Fi credentials to decide whether Improv should
// own Serial for this boot.
//
// Policy: fail closed to console ownership on any unexpected condition. Only
// two positive cases enable Improv:
//   1. Key genuinely missing (ESP_ERR_NVS_NOT_FOUND) -- never provisioned.
//   2. Blob successfully read at exactly the expected size and ssid[0] == 0.
//
// Any nvs_open/read error, wrong size, unterminated ssid, or bad status
// keeps the existing console + captive-portal path and is logged loudly.
// The previous approach (query with required=1) was wrong: NVS returns
// INVALID_LENGTH without copying anything on size mismatch, so provisioned
// devices would silently look unprovisioned every boot.
bool improv_should_activate() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("WIFI", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace has never been written. Genuinely unprovisioned.
        ESP_LOGI(TAG, "boot classification: no HomeSpan WIFI namespace -> Improv owns Serial");
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot classification: nvs_open(\"WIFI\") failed: %s -> Console (fail-closed)",
                 esp_err_to_name(err));
        return false;
    }

    // Query size first, then read the whole blob. HomeSpan writes 98 bytes.
    size_t required = 0;
    err = nvs_get_blob(handle, "WIFIDATA", nullptr, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGI(TAG, "boot classification: no WIFIDATA key -> Improv owns Serial");
        return true;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "boot classification: nvs_get_blob size query failed: %s -> Console (fail-closed)",
                 esp_err_to_name(err));
        return false;
    }
    if (required != homespan::kWifiDataBlobSize) {
        nvs_close(handle);
        ESP_LOGE(TAG, "boot classification: WIFIDATA blob is %u bytes, expected %u -> Console (fail-closed)",
                 static_cast<unsigned>(required),
                 static_cast<unsigned>(homespan::kWifiDataBlobSize));
        return false;
    }

    uint8_t blob[homespan::kWifiDataBlobSize] = {};
    size_t read = sizeof(blob);
    err = nvs_get_blob(handle, "WIFIDATA", blob, &read);
    nvs_close(handle);
    if (err != ESP_OK || read != sizeof(blob)) {
        ESP_LOGE(TAG, "boot classification: nvs_get_blob read failed: %s (read=%u) -> Console (fail-closed)",
                 esp_err_to_name(err), static_cast<unsigned>(read));
        return false;
    }

    switch (homespan::classifyWifiDataBlob(blob, read)) {
        case homespan::WifiDataStatus::Unprovisioned:
            ESP_LOGI(TAG, "boot classification: WIFIDATA present but ssid empty -> Improv owns Serial");
            return true;
        case homespan::WifiDataStatus::Provisioned:
            ESP_LOGI(TAG, "boot classification: WIFIDATA valid -> Console owns Serial (normal boot)");
            return false;
        case homespan::WifiDataStatus::Malformed:
        default:
            ESP_LOGE(TAG, "boot classification: WIFIDATA malformed -> Console (fail-closed)");
            return false;
    }
}

// Improv persist callback: write via HomeSpan's public API and then verify
// by reading the blob back through NVS.
//
// HomeSpan::setWifiCredentials returns Span& (chainable) and has no error
// signal (see components/HomeSpan/upstream/src/HomeSpan.cpp:1494). Any
// nvs_set_blob or nvs_commit failure inside it is silent. The read-back is
// what lets this adapter honour the Improv contract that the client must
// never see a success signal for credentials that were not actually stored.
//
// Runs on the Improv service task, so must be self-contained -- no reliance
// on the main task's context.
bool persist_credentials(const std::string &ssid, const std::string &password) {
    ESP_LOGI(TAG, "persist: writing credentials via homeSpan.setWifiCredentials (ssid=%u bytes)",
             static_cast<unsigned>(ssid.size()));
    homeSpan.setWifiCredentials(ssid.c_str(), password.c_str());

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("WIFI", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist verify: nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t required = 0;
    err = nvs_get_blob(handle, "WIFIDATA", nullptr, &required);
    if (err != ESP_OK || required != homespan::kWifiDataBlobSize) {
        nvs_close(handle);
        ESP_LOGE(TAG, "persist verify: bad blob size after persist (%s, %u bytes)",
                 esp_err_to_name(err), static_cast<unsigned>(required));
        return false;
    }
    uint8_t blob[homespan::kWifiDataBlobSize] = {};
    size_t read = sizeof(blob);
    err = nvs_get_blob(handle, "WIFIDATA", blob, &read);
    nvs_close(handle);
    if (err != ESP_OK || read != sizeof(blob)) {
        ESP_LOGE(TAG, "persist verify: read-back failed: %s", esp_err_to_name(err));
        return false;
    }
    // Compare the persisted ssid byte-for-byte with what we asked to write.
    // A mismatch means HomeSpan silently truncated or the write did not
    // land -- either way, do not signal success.
    const size_t cmp = std::min(ssid.size(), homespan::kWifiDataSsidMax);
    if (std::memcmp(blob, ssid.data(), cmp) != 0 ||
        (cmp < homespan::kWifiDataSsidMax && blob[cmp] != 0)) {
        ESP_LOGE(TAG, "persist verify: persisted ssid does not match input");
        return false;
    }
    ESP_LOGI(TAG, "persist verify: OK");
    return true;
}

// Improv completion callback (runs on the Improv service task). No
// reboot: signal the main setup task so it can continue deferred
// hardware init in the same boot.
void complete_and_signal() {
    ESP_LOGI(TAG, "provisioning complete; signalling main task");
    Serial.flush();
    xTaskNotifyGive(g_main_task_to_notify);
}

// Dedicated Improv service task. Only started on the factory-provisioning
// branch (setup() early-returns before hardware init in that case). Arduino
// loop() does not begin until setup() returns, and even after it returns
// factory-mode leaves nothing else driving Serial, so pumping the state
// machine from a dedicated task -- rather than piggybacking on loop() --
// keeps a single, well-defined Serial reader and guarantees the browser
// sees Authorized within a bounded time from port open.
//
// Priority: tskIDLE_PRIORITY + 1. Above the idle task so it always makes
// forward progress, below every application task (WiFi driver, HomeSpan)
// so it never starves them. Wake interval 20 ms -- fast enough that a full
// WIFI_SETTINGS frame (~80 bytes) is drained in one wake, slow enough that
// CPU cost is negligible.
[[noreturn]] void improv_task(void *) {
    ESP_LOGI(TAG, "Improv task started: priority=%u, stack_free=%u bytes",
             static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    for (;;) {
        improv::loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

extern "C" bool improv_should_own_serial(void) {
    if (!g_decided) {
        g_owns = improv_should_activate();
        g_decided = true;
    }
    return g_owns;
}

extern "C" void improv_start_after_homespan_begin(void) {
    if (!improv_should_own_serial()) return;
    if (g_task != nullptr) {
        ESP_LOGW(TAG, "improv_start called twice; ignoring");
        return;
    }

    // Fields returned in GET_DEVICE_INFO. Version comes from HomeSpan's
    // sketchVersion (set at setup time). The device name uses the MAC-based
    // suffix HomeSpan itself uses, so what the flasher shows matches what the
    // Home app will see once the device is provisioned.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char device_name[24];
    snprintf(device_name, sizeof(device_name), "HomeKey-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    improv::SerialConfig cfg{
        .firmware_name    = "HomeKey-ESP32",
        .firmware_version = homeSpan.getSketchVersion() ? homeSpan.getSketchVersion() : "unknown",
        .chip_family      = "ESP32-S3",
        .device_name      = device_name,
        .io = {
            .read_byte   = []() -> int { return Serial.available() ? Serial.read() : -1; },
            .write_bytes = [](const uint8_t *data, size_t len) {
                Serial.write(data, len);
                Serial.flush();
            },
        },
        .wifi = {
            .begin        = [](const std::string &ssid, const std::string &password) {
                WiFi.begin(ssid.c_str(), password.c_str());
            },
            .is_connected = []() { return WiFi.status() == WL_CONNECTED; },
            .failed_hard  = []() {
                auto s = WiFi.status();
                return s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL;
            },
            .device_url   = []() -> std::string {
                auto ip = WiFi.localIP();
                char buf[32];
                snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
                return std::string(buf);
            },
            .disconnect   = []() {
                // false, false = STA-only disconnect, keep the WiFi radio on.
                // Passing wifioff=true would tear the shared radio down and
                // kill any AP HomeSpan is running for the captive-portal
                // fallback, which must survive a failed Improv attempt.
                WiFi.disconnect(false, false);
            },
        },
        .persist  = persist_credentials,
        .complete = complete_and_signal,
    };
    improv::begin(cfg);

    // Register the handoff target BEFORE the Improv task can run and
    // reach complete_and_signal. This closes a race where a fast
    // completion could xTaskNotifyGive(nullptr) if we captured the
    // handle inside improv_wait_for_provisioning() instead.
    g_main_task_to_notify = xTaskGetCurrentTaskHandle();

    // 4096 stack: bounded parser buffer + two ~100 byte strings + WiFi/
    // NVS callbacks + ESP_LOG formatting. Startup log line below reports
    // the actual free stack for confirmation.
    BaseType_t rc = xTaskCreate(&improv_task, "improv", 4096, nullptr,
                                tskIDLE_PRIORITY + 1, &g_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(improv) FAILED: rc=%d -- provisioning will not work",
                 static_cast<int>(rc));
        g_task = nullptr;
        return;
    }
    ESP_LOGI(TAG, "xTaskCreate(improv) ok: handle=%p", static_cast<void *>(g_task));
}

extern "C" void improv_wait_for_provisioning(void) {
    // The Improv task's completion callback notifies g_main_task_to_notify,
    // which improv_start_after_homespan_begin already set to this task.
    ESP_LOGI(TAG, "waiting for Improv provisioning handoff");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "handoff received");
}

#else  // !CONFIG_ENABLE_IMPROV_SERIAL

extern "C" bool improv_should_own_serial(void) { return false; }
extern "C" void improv_start_after_homespan_begin(void) {}
extern "C" void improv_wait_for_provisioning(void) {}

#endif
