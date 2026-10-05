// Device-only OTA backend (ESP-IDF): implements eyes::OtaBackend with
// esp_https_ota into the inactive slot and app_update rollback. NOT compiled on
// the host (not in the Makefile's CORE_SRCS); the sim/tests use a fake.
//
// Rollback: the inactive slot is written and set as boot; on next boot the new
// image runs its self-check and calls confirm_running_image() -> the bootloader
// keeps it. If it crashes/hangs before confirming, esp_ota's rollback restores
// the previous slot. Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
//
// The release channel is a JSON manifest at LILGUY_OTA_MANIFEST_URL:
//   {"version":"1.1.0","url":"https://.../podbot-1.1.0.bin","sha256":"..."}
// Signed images (CONFIG_SECURE_SIGNED_ON_UPDATE) are verified by the bootloader.

#include "eyes/os/ota.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

namespace eyes {
namespace {

constexpr const char *kTag = "ota";

// Belt-and-suspenders integrity check: after the image is written, hash the
// update partition and compare to the manifest's sha256. The AUTHENTICITY
// boundary remains Secure Boot V2 (CONFIG_SECURE_SIGNED_ON_UPDATE, verified by
// the bootloader) — this only catches a corrupted/truncated download that still
// parsed as a valid image. An empty want (no sha256 in the manifest) skips it.
bool image_sha256_matches(const std::string &want_hex, int image_len)
{
    if (want_hex.empty()) {
        return true;  // nothing to check against; Secure Boot still applies
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr || image_len <= 0) {
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    std::uint8_t buf[1024];
    int off = 0;
    while (off < image_len) {
        const int n = std::min(static_cast<int>(sizeof(buf)), image_len - off);
        if (esp_partition_read(part, static_cast<std::size_t>(off), buf,
                               static_cast<std::size_t>(n)) != ESP_OK ||
            mbedtls_sha256_update(&ctx, buf, static_cast<std::size_t>(n)) != 0) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        off += n;
    }
    std::uint8_t digest[32];
    const bool ok = mbedtls_sha256_finish(&ctx, digest) == 0;
    mbedtls_sha256_free(&ctx);
    if (!ok) {
        return false;
    }
    static const char *hex = "0123456789abcdef";
    std::string got;
    got.reserve(64);
    for (const std::uint8_t byte : digest) {
        got += hex[byte >> 4];
        got += hex[byte & 0x0F];
    }
    std::string want = want_hex;
    std::transform(want.begin(), want.end(), want.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return got == want;
}

// The manifest URL is a build-time define so the release channel is fixed per
// firmware; override in Kconfig if needed.
#ifndef LILGUY_OTA_MANIFEST_URL
#define LILGUY_OTA_MANIFEST_URL "https://github.com/bry0306/lil-circle/releases/latest/download/manifest.json"
#endif

// Tiny field pluck from the flat manifest JSON (no JSON lib in this unit).
std::string json_field(const std::string &blob, const char *key)
{
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto at = blob.find(needle);
    if (at == std::string::npos) return "";
    const auto start = at + needle.size();
    const auto end = blob.find('"', start);
    return end == std::string::npos ? "" : blob.substr(start, end - start);
}

class EspOtaBackend final : public OtaBackend {
public:
    const char *current_version() const override
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        return desc ? desc->version : "unknown";
    }

    void begin_check() override
    {
        // The check is a short blocking HTTP GET of the manifest; run it on a
        // one-shot task so the caller (LVGL loop) never stalls.
        checking_.store(true);
        result_.clear();
        xTaskCreate(&EspOtaBackend::check_task, "ota_check", 6144, this, 4, nullptr);
    }

    std::string check_result() override
    {
        if (checking_.load()) return "";
        return result_.empty() ? "none" : result_;
    }

    void begin_download(const std::string &version) override
    {
        (void)version;
        downloading_.store(true);
        percent_.store(0);
        failed_.store(false);
        xTaskCreate(&EspOtaBackend::download_task, "ota_dl", 8192, this, 4, nullptr);
    }

    int download_percent() override { return failed_.load() ? -1 : percent_.load(); }
    bool download_done() const override { return !downloading_.load() && !failed_.load(); }

    void apply_and_reboot() override
    {
        // esp_https_ota already set the boot partition; just restart.
        ESP_LOGI(kTag, "rebooting into new image");
        esp_restart();
    }

    // The manifest may flag a release mandatory: {"version":...,"required":true}.
    // The controller then never defers it (it still waits for idle + power).
    bool required_update() const override { return required_.load(); }

    void confirm_running_image() override
    {
        // Called once the freshly booted image is proven healthy.
        esp_ota_img_states_t state;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(kTag, "running image confirmed valid");
        }
    }

private:
    static void check_task(void *arg)
    {
        auto *self = static_cast<EspOtaBackend *>(arg);
        std::string body;
        esp_http_client_config_t cfg = {};
        cfg.url = LILGUY_OTA_MANIFEST_URL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = 8000;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            char buf[512];
            int r;
            while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                body.append(buf, static_cast<std::size_t>(r));
            }
        }
        esp_http_client_cleanup(client);
        const std::string version = json_field(body, "version");
        self->manifest_url_ = json_field(body, "url");
        self->sha256_ = json_field(body, "sha256");  // optional integrity check
        // Boolean field, so match the literal rather than json_field (strings only).
        self->required_.store(body.find("\"required\":true") != std::string::npos);
        self->result_ = version;
        self->checking_.store(false);
        vTaskDelete(nullptr);
    }

    static void download_task(void *arg)
    {
        auto *self = static_cast<EspOtaBackend *>(arg);
        esp_http_client_config_t http = {};
        http.url = self->manifest_url_.c_str();
        http.crt_bundle_attach = esp_crt_bundle_attach;
        http.timeout_ms = 15000;
        esp_https_ota_config_t ota_cfg = {};
        ota_cfg.http_config = &http;

        esp_https_ota_handle_t handle = nullptr;
        if (self->manifest_url_.empty() || esp_https_ota_begin(&ota_cfg, &handle) != ESP_OK) {
            self->failed_.store(true);
            self->downloading_.store(false);
            vTaskDelete(nullptr);
            return;
        }
        const int total = esp_https_ota_get_image_size(handle);
        esp_err_t err;
        while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            const int done = esp_https_ota_get_image_len_read(handle);
            self->percent_.store(total > 0 ? (done * 100) / total : 0);
        }
        // Bytes written so far — captured before finish() frees the handle.
        const int image_len = esp_https_ota_get_image_len_read(handle);
        const bool ok = err == ESP_OK && esp_https_ota_is_complete_data_received(handle);
        if (ok && esp_https_ota_finish(handle) == ESP_OK) {
            if (image_sha256_matches(self->sha256_, image_len)) {
                self->percent_.store(100);
            } else {
                // A corrupt image that still parsed: undo the boot switch so the
                // device stays on the current slot, and fail the attempt.
                ESP_LOGE(kTag, "image sha256 mismatch — reverting boot partition");
                esp_ota_set_boot_partition(esp_ota_get_running_partition());
                self->failed_.store(true);
            }
        } else {
            esp_https_ota_abort(handle);
            self->failed_.store(true);
        }
        self->downloading_.store(false);
        vTaskDelete(nullptr);
    }

    std::atomic<bool> checking_{false};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> required_{false};
    std::atomic<int> percent_{0};
    std::string result_;
    std::string manifest_url_;
    std::string sha256_;  // expected image hash from the manifest (optional)
};

}  // namespace

// The device wires this into its Services handle at boot.
OtaBackend &esp_ota_backend()
{
    static EspOtaBackend backend;
    return backend;
}

}  // namespace eyes
