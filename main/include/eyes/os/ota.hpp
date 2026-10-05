#pragma once

// Over-the-air update: a platform-free, host-tested state machine that gates
// updates the way apollo does — never mid-conversation, never on low battery,
// bounded retries — over a small backend interface. The ESP-IDF implementation
// (esp_https_ota + app_update rollback) lives in the device-only
// main/ota_service.cpp; the sim and tests use a fake backend.
//
// Dual ota_0/ota_1 slots (see partitions.csv) give rollback: a freshly booted
// image runs a self-check and calls confirm() to mark itself valid, else the
// bootloader rolls back on next boot.

#include <cstdint>
#include <string>

namespace eyes {

enum class OtaState : std::uint8_t {
    idle,        // nothing to do
    checking,    // asking the release channel for a newer version
    available,   // a newer version exists, not yet downloading
    downloading, // streaming the image into the inactive slot
    ready,       // downloaded + verified, awaiting a reboot
    installing,  // rebooting into the new slot
    error,       // last attempt failed; will retry within limits
};

struct OtaStatus {
    OtaState state{OtaState::idle};
    std::string available_version;  // when state >= available
    int percent{0};                 // download progress 0..100
    std::string message;            // human line for the UI / voice
    bool required{false};           // a mandatory update (never deferred away)
};

// What the device hardware provides. The controller drives it; the backend
// never decides policy. Non-blocking: begin_* starts work, poll_* reports.
class OtaBackend {
public:
    virtual ~OtaBackend() = default;
    virtual const char *current_version() const = 0;
    // Start a version check; result read via check_result().
    virtual void begin_check() = 0;
    // "" while checking; the newer version string if one exists; current
    // version (or "none") when up to date.
    virtual std::string check_result() = 0;
    virtual void begin_download(const std::string &version) = 0;
    virtual int download_percent() = 0;   // 0..100, or <0 on failure
    virtual bool download_done() const = 0;
    virtual void apply_and_reboot() = 0;  // installs the ready image; never returns
    // Called by a freshly booted image once it has self-checked OK, so the
    // bootloader keeps it instead of rolling back.
    virtual void confirm_running_image() = 0;
    // True when the latest checked version is mandatory (a security/critical
    // release the manifest flags required). Default false keeps older backends
    // working; a required update is never deferred by the retry cap, though it
    // still respects the safety gates (idle + power). Read after a check.
    virtual bool required_update() const { return false; }
};

// Gating policy (apollo): update only when idle, battery healthy, off a
// cooldown, and under the per-version retry cap.
struct OtaPolicy {
    int min_battery_percent{50};
    std::uint32_t check_cooldown_ms{6U * 60U * 60U * 1000U};  // 6 h
    int max_attempts_per_version{3};
};

class OtaController {
public:
    explicit OtaController(OtaBackend &backend, OtaPolicy policy = {})
        : backend_(backend), policy_(policy)
    {
    }

    const OtaStatus &status() const { return status_; }

    // The boot self-check: call once the new image has proven itself healthy.
    void confirm_boot() { backend_.confirm_running_image(); }

    // Ask to check now (e.g. nightly, or user-initiated). Honours gating.
    void request_check(std::uint32_t now_ms)
    {
        check_requested_ms_ = now_ms;
        have_request_ = true;
    }

    // Drive the machine. idle = no conversation/anim in progress.
    void tick(std::uint32_t now_ms, int battery_percent, bool charging, bool idle)
    {
        const bool power_ok = charging || battery_percent >= policy_.min_battery_percent;
        switch (status_.state) {
            case OtaState::idle: {
                if (!have_request_ || !idle || !power_ok) {
                    return;
                }
                if (last_check_ms_ != 0U &&
                    static_cast<std::int32_t>(now_ms - last_check_ms_) <
                        static_cast<std::int32_t>(policy_.check_cooldown_ms)) {
                    return;  // still cooling down
                }
                have_request_ = false;
                last_check_ms_ = now_ms;
                status_ = {OtaState::checking, "", 0, "Checking for updates"};
                backend_.begin_check();
                break;
            }
            case OtaState::checking: {
                const std::string result = backend_.check_result();
                if (result.empty()) {
                    return;  // still checking
                }
                if (result == "none" || result == backend_.current_version()) {
                    status_ = {OtaState::idle, "", 0, "Up to date", false};
                    return;
                }
                const bool req = backend_.required_update();
                status_ = {OtaState::available, result, 0,
                           req ? "Update required" : "Update available", req};
                break;
            }
            case OtaState::available: {
                if (!idle || !power_ok) {
                    return;
                }
                // A required update is never abandoned; only optional ones defer
                // once they've burned the retry budget on this version.
                if (!status_.required &&
                    attempts_for(status_.available_version) >= policy_.max_attempts_per_version) {
                    status_ = {OtaState::idle, "", 0, "Update deferred (too many attempts)", false};
                    return;
                }
                note_attempt(status_.available_version);
                status_.state = OtaState::downloading;
                status_.message = status_.required ? "Downloading (required)" : "Downloading";
                backend_.begin_download(status_.available_version);
                break;
            }
            case OtaState::downloading: {
                const int pct = backend_.download_percent();
                if (pct < 0) {
                    // Mutate in place so `required` + version survive the error.
                    status_.state = OtaState::error;
                    status_.percent = 0;
                    status_.message = "Download failed";
                    error_since_ms_ = now_ms;
                    return;
                }
                status_.percent = pct;
                if (backend_.download_done()) {
                    status_.state = OtaState::ready;
                    status_.percent = 100;
                    status_.message = "Update ready";
                }
                break;
            }
            case OtaState::ready: {
                // Only reboot when idle (never mid-conversation).
                if (!idle || !power_ok) {
                    return;
                }
                status_.state = OtaState::installing;
                status_.message = "Installing";
                backend_.apply_and_reboot();  // does not return on device
                break;
            }
            case OtaState::installing:
                return;  // reboot pending
            case OtaState::error: {
                // Back to idle after a short hold so it can be retried later.
                if (static_cast<std::int32_t>(now_ms - error_since_ms_) > 30000) {
                    status_ = {OtaState::idle, "", 0, ""};
                }
                break;
            }
        }
    }

private:
    int attempts_for(const std::string &version) const
    {
        return version == attempt_version_ ? attempt_count_ : 0;
    }
    void note_attempt(const std::string &version)
    {
        if (version != attempt_version_) {
            attempt_version_ = version;
            attempt_count_ = 0;
        }
        ++attempt_count_;
    }

    OtaBackend &backend_;
    OtaPolicy policy_;
    OtaStatus status_{};
    bool have_request_{false};
    std::uint32_t check_requested_ms_{0};
    std::uint32_t last_check_ms_{0};
    std::uint32_t error_since_ms_{0};
    std::string attempt_version_;
    int attempt_count_{0};
};

// --- Provisioning ------------------------------------------------------
// Getting Wi-Fi credentials onto the device without a keyboard: a phone sends
// them over BLE (ESP-IDF wifi_provisioning + BLE transport). Interface here,
// device impl in main/wifi_provisioning.cpp, fake in the sim.
class ProvisioningBackend {
public:
    virtual ~ProvisioningBackend() = default;
    virtual bool is_provisioned() const = 0;      // credentials already stored?
    virtual bool start(const char *service_name) = 0;  // begin BLE provisioning
    virtual void stop() = 0;
    virtual bool connected() const = 0;           // Wi-Fi up after provisioning
};

class NullProvisioningBackend final : public ProvisioningBackend {
public:
    bool is_provisioned() const override { return false; }
    bool start(const char *) override { return false; }
    void stop() override {}
    bool connected() const override { return false; }
};

}  // namespace eyes
