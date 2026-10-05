#include "eyes/preferences.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "nvs.h"
#include "nvs_flash.h"

#include "eyes/catalog.hpp"

namespace eyes {
namespace {

constexpr char kNamespace[] = "lilguy";
constexpr std::uint32_t kCalibrationMagic = 0x45594543U;
constexpr std::uint8_t kCatalogVersion = 2U;

struct CalibrationBlob {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    float neutral_accel_x;
    float neutral_accel_y;
    float neutral_accel_z;
    float gyro_bias_x;
    float gyro_bias_y;
    float gyro_bias_z;
};

bool calibration_is_plausible(const Calibration &calibration)
{
    const bool finite = std::isfinite(calibration.neutral_accel_x) &&
                        std::isfinite(calibration.neutral_accel_y) &&
                        std::isfinite(calibration.neutral_accel_z) &&
                        std::isfinite(calibration.gyro_bias_x) &&
                        std::isfinite(calibration.gyro_bias_y) &&
                        std::isfinite(calibration.gyro_bias_z);
    if (!calibration.valid || !finite) {
        return false;
    }
    const float gravity = std::sqrt(calibration.neutral_accel_x * calibration.neutral_accel_x +
                                    calibration.neutral_accel_y * calibration.neutral_accel_y +
                                    calibration.neutral_accel_z * calibration.neutral_accel_z);
    const float gyro = std::sqrt(calibration.gyro_bias_x * calibration.gyro_bias_x +
                                 calibration.gyro_bias_y * calibration.gyro_bias_y +
                                 calibration.gyro_bias_z * calibration.gyro_bias_z);
    return gravity >= 7.0F && gravity <= 13.0F && gyro <= 2.0F;
}

}  // namespace

esp_err_t Preferences::init()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }
    return result;
}

Selection Preferences::load_selection(Selection fallback) const
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return wrap_selection(fallback);
    }
    std::uint8_t catalog_version = 0U;
    if (nvs_get_u8(handle, "catalog_ver", &catalog_version) != ESP_OK ||
        catalog_version != kCatalogVersion) {
        nvs_close(handle);
        return wrap_selection(fallback);
    }
    std::int32_t shape = fallback.shape;
    std::int32_t palette = fallback.palette;
    (void)nvs_get_i32(handle, "shape", &shape);
    (void)nvs_get_i32(handle, "palette", &palette);
    nvs_close(handle);
    return wrap_selection({static_cast<int>(shape), static_cast<int>(palette)});
}

esp_err_t Preferences::save_selection(Selection selection) const
{
    selection = wrap_selection(selection);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_i32(handle, "shape", selection.shape);
    if (result == ESP_OK) {
        result = nvs_set_i32(handle, "palette", selection.palette);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "catalog_ver", kCatalogVersion);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

Calibration Preferences::load_calibration() const
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return {};
    }
    CalibrationBlob blob{};
    std::size_t size = sizeof(blob);
    const esp_err_t result = nvs_get_blob(handle, "imu_bias", &blob, &size);
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(blob) || blob.magic != kCalibrationMagic ||
        blob.version != 2U || blob.size != sizeof(blob)) {
        return {};
    }
    const Calibration calibration{
        blob.neutral_accel_x, blob.neutral_accel_y, blob.neutral_accel_z,
        blob.gyro_bias_x,     blob.gyro_bias_y,     blob.gyro_bias_z,
        true,
    };
    return calibration_is_plausible(calibration) ? calibration : Calibration{};
}

namespace {

constexpr std::uint32_t kSettingsMagic = 0x4C475345U;  // "ESGL"

// On-flash layout. Growth contract: append new fields only, bump nothing —
// load_settings pre-fills defaults and accepts any stored size between the
// header and the current struct, so an old (shorter) blob leaves the new
// tail fields at their defaults.
struct SettingsBlob {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint8_t imu_level;
    std::uint8_t reserved[3];
    float orientation;
    std::uint8_t face_mode;  // appended field; older blobs leave the default
    std::uint8_t reserved2[3];
    std::uint8_t capsule_invert;  // appended field; older blobs leave the default
    std::uint8_t reserved3[3];
};

constexpr std::size_t kSettingsHeaderSize = 8U;  // magic + version + size

}  // namespace

Settings Preferences::load_settings() const
{
    const Settings defaults{};
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return defaults;
    }
    SettingsBlob blob{};
    blob.imu_level = defaults.imu_level;
    blob.orientation = defaults.orientation;
    blob.face_mode = defaults.face_mode;
    blob.capsule_invert = defaults.capsule_invert;
    std::size_t size = sizeof(blob);
    const esp_err_t result = nvs_get_blob(handle, "settings", &blob, &size);
    nvs_close(handle);
    if (result != ESP_OK || blob.magic != kSettingsMagic || blob.version != 1U ||
        size < kSettingsHeaderSize || size > sizeof(blob) || blob.size != size) {
        return defaults;
    }
    Settings settings{};
    settings.imu_level = blob.imu_level <= 2U ? blob.imu_level : defaults.imu_level;
    settings.orientation = std::isfinite(blob.orientation) &&
                                   std::fabs(blob.orientation) <= 4.0F
                               ? blob.orientation
                               : defaults.orientation;
    settings.face_mode = blob.face_mode <= 1U ? blob.face_mode : defaults.face_mode;
    settings.capsule_invert =
        blob.capsule_invert <= 1U ? blob.capsule_invert : defaults.capsule_invert;
    return settings;
}

esp_err_t Preferences::save_settings(const Settings &settings) const
{
    SettingsBlob blob{};
    blob.magic = kSettingsMagic;
    blob.version = 1U;
    blob.size = static_cast<std::uint16_t>(sizeof(blob));
    blob.imu_level = settings.imu_level;
    blob.orientation = settings.orientation;
    blob.face_mode = settings.face_mode;
    blob.capsule_invert = settings.capsule_invert;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, "settings", &blob, sizeof(blob));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

DeviceIdentity Preferences::load_identity() const
{
    DeviceIdentity identity{};
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return identity;
    }
    std::size_t size = sizeof(identity.name);
    if (nvs_get_str(handle, "id_name", identity.name, &size) != ESP_OK) {
        identity.name[0] = '\0';
    }
    size = sizeof(identity.wake_word);
    char wake[DeviceIdentity::kWakeMax];
    if (nvs_get_str(handle, "id_wake", wake, &size) == ESP_OK && wake[0] != '\0') {
        std::snprintf(identity.wake_word, sizeof(identity.wake_word), "%s", wake);
    }
    std::uint8_t stage = 0U;
    if (nvs_get_u8(handle, "id_stage", &stage) == ESP_OK &&
        stage <= static_cast<std::uint8_t>(OnboardingStage::done)) {
        identity.stage = static_cast<OnboardingStage>(stage);
    }
    nvs_close(handle);
    return identity;
}

esp_err_t Preferences::save_identity(const DeviceIdentity &identity) const
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, "id_name", identity.name);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "id_wake", identity.wake_word);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "id_stage", static_cast<std::uint8_t>(identity.stage));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t Preferences::save_calibration(const Calibration &calibration) const
{
    if (!calibration_is_plausible(calibration)) {
        return ESP_ERR_INVALID_ARG;
    }
    const CalibrationBlob blob{
        kCalibrationMagic,
        2U,
        static_cast<std::uint16_t>(sizeof(CalibrationBlob)),
        calibration.neutral_accel_x,
        calibration.neutral_accel_y,
        calibration.neutral_accel_z,
        calibration.gyro_bias_x,
        calibration.gyro_bias_y,
        calibration.gyro_bias_z,
    };
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, "imu_bias", &blob, sizeof(blob));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

}  // namespace eyes
