// Device-only BLE Wi-Fi provisioning (ESP-IDF): implements
// eyes::ProvisioningBackend so a phone hands the device Wi-Fi credentials over
// BLE — no keyboard on a round screen. NOT compiled on the host (not in the
// Makefile's CORE_SRCS); the sim uses a fake.
//
// Uses ESP-IDF's wifi_provisioning manager with the BLE scheme (protocomm over
// GATT), secured with Security 1 (X25519 + AES-CTR, PoP). The companion app
// (Espressif's "ESP BLE Provisioning", or our own) scans for the service name,
// exchanges the proof-of-possession, and sends SSID + password. Credentials
// persist in NVS, so this only runs on first boot / after a reset.

#include "eyes/os/ota.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

namespace eyes {
namespace {

constexpr const char *kTag = "prov";
// Proof-of-possession: the phone must present this to pair. Ship a per-device
// value (e.g. derived from the MAC) in production; a constant here is the
// bring-up default.
#ifndef LILGUY_PROV_POP
#define LILGUY_PROV_POP "podbot-setup"
#endif

class EspProvisioning final : public ProvisioningBackend {
public:
    bool is_provisioned() const override
    {
        bool provisioned = false;
        // wifi_prov_mgr must be initialised before this query; the device does
        // that once at boot in app_main before constructing Services.
        wifi_prov_mgr_is_provisioned(&provisioned);
        return provisioned;
    }

    bool start(const char *service_name) override
    {
        // Security 1 with a PoP; BLE transport. The 128-bit service UUID is the
        // standard Espressif provisioning UUID used by the reference apps.
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
        const char *pop = LILGUY_PROV_POP;
        const esp_err_t err = wifi_prov_mgr_start_provisioning(
            security, pop, service_name, /*service_key=*/nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "start_provisioning failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(kTag, "BLE provisioning started as '%s'", service_name);
        return true;
    }

    void stop() override { wifi_prov_mgr_stop_provisioning(); }

    bool connected() const override
    {
        wifi_ap_record_t ap;
        return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    }
};

}  // namespace

// Initialise the provisioning manager (call once at boot before Services).
void esp_provisioning_init()
{
    wifi_prov_mgr_config_t config = {};
    config.scheme = wifi_prov_scheme_ble;
    config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
}

ProvisioningBackend &esp_provisioning_backend()
{
    static EspProvisioning backend;
    return backend;
}

}  // namespace eyes
