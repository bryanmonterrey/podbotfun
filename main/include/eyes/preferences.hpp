#pragma once

#include <cstdint>

#include "esp_err.h"

#include "eyes/os/onboarding.hpp"
#include "eyes/types.hpp"

namespace eyes {

// Settings lives in eyes/types.hpp: the options menu and the host simulator
// both need it without pulling in the NVS API.
class Preferences {
  public:
    esp_err_t init();
    Selection load_selection(Selection fallback) const;
    esp_err_t save_selection(Selection selection) const;
    Calibration load_calibration() const;
    esp_err_t save_calibration(const Calibration &calibration) const;
    Settings load_settings() const;
    esp_err_t save_settings(const Settings &settings) const;
    // Who the device is (name, wake word, onboarding stage). Three plain keys
    // rather than a blob: the phone app will read and write them one at a
    // time over sync, and a partial write must not corrupt the rest.
    DeviceIdentity load_identity() const;
    esp_err_t save_identity(const DeviceIdentity &identity) const;
};

}  // namespace eyes
