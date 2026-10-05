#pragma once

// The OS service layer: every feature the finished product will have exists
// here as an interface, whether or not the fitted hardware can provide it.
// A backend that is not fitted reports absent through its capability instead
// of being compiled out, so apps and the shell are written once against the
// full feature set and features light up as better hardware arrives. On the
// Waveshare ESP32-S3 target the cameras, NFC, GNSS, crown and haptics are
// absent (dual cameras cannot exist here at all: the S3 has a single DVP
// bus); the custom board later implements these same interfaces over its own
// drivers without the apps changing.
//
// Everything is host-compilable. The simulator substitutes fakes exactly the
// way DeviceUiHost stands in for NVS and the battery gauge, which also gives
// tests a way to exercise app behavior for hardware that does not exist yet.

#include <cstdint>

namespace eyes {

// One bit per hardware capability. `fitted` is the truth about the running
// board, not about the OS: services for absent capabilities still exist and
// simply refuse politely.
enum class Capability : std::uint8_t {
    display,
    touch,
    imu,
    sound_energy,
    battery_gauge,
    wifi,
    ble,
    camera_front,
    camera_rear,
    nfc,
    gnss,
    crown,
    haptics,
    face_unlock,     // on-device biometric (face/gesture) user verification
    secure_element,  // protected key storage for on-device passkeys
    count
};

class CapabilitySet {
public:
    constexpr bool has(Capability capability) const
    {
        return (bits_ & bit(capability)) != 0U;
    }
    constexpr void set(Capability capability, bool present = true)
    {
        if (present) {
            bits_ |= bit(capability);
        } else {
            bits_ &= ~bit(capability);
        }
    }

private:
    static constexpr std::uint32_t bit(Capability capability)
    {
        return 1U << static_cast<unsigned>(capability);
    }
    std::uint32_t bits_{0U};
};

// --- Cameras -----------------------------------------------------------
// Modeled on the target hardware direction (front + rear MIPI modules behind
// an ISP). Frames are borrowed, not owned: the backend keeps the buffer valid
// until the next capture or stop.

struct CameraFrame {
    enum class Format : std::uint8_t { rgb565, yuv422 };

    const std::uint8_t *data{nullptr};
    int width{0};
    int height{0};
    Format format{Format::rgb565};
};

class CameraService {
public:
    enum class Lens : std::uint8_t { front, rear };

    virtual ~CameraService() = default;
    virtual bool available(Lens lens) const = 0;
    // Both return false when the lens is not fitted; apps need no other path.
    virtual bool start(Lens lens) = 0;
    virtual void stop(Lens lens) = 0;
    virtual bool capture(Lens lens, CameraFrame &frame) = 0;
};

class NullCameraService final : public CameraService {
public:
    bool available(Lens) const override { return false; }
    bool start(Lens) override { return false; }
    void stop(Lens) override {}
    bool capture(Lens, CameraFrame &) override { return false; }
};

// --- NFC ---------------------------------------------------------------

struct NfcTag {
    std::uint8_t uid[10]{};
    std::uint8_t uid_length{0};
};

class NfcService {
public:
    virtual ~NfcService() = default;
    virtual bool available() const = 0;
    // Non-blocking poll; false means no tag in field (or no hardware).
    virtual bool read_tag(NfcTag &tag) = 0;
};

class NullNfcService final : public NfcService {
public:
    bool available() const override { return false; }
    bool read_tag(NfcTag &) override { return false; }
};

// --- GNSS --------------------------------------------------------------

struct GnssFix {
    double latitude_deg{0.0};
    double longitude_deg{0.0};
    float altitude_m{0.0F};
    float horizontal_accuracy_m{0.0F};
    std::uint32_t unix_time_s{0U};
};

class GnssService {
public:
    virtual ~GnssService() = default;
    virtual bool available() const = 0;
    // Latest fix if one is held; false with no fix yet (or no hardware).
    virtual bool fix(GnssFix &out) = 0;
};

class NullGnssService final : public GnssService {
public:
    bool available() const override { return false; }
    bool fix(GnssFix &) override { return false; }
};

// --- Crown (rotary input) ---------------------------------------------

class CrownService {
public:
    virtual ~CrownService() = default;
    virtual bool available() const = 0;
    // Rotation accumulated since the last call, in detents; positive is
    // clockwise. Reading consumes it, so exactly one caller owns the crown.
    virtual float take_rotation() = 0;
    virtual bool pressed() const = 0;
};

class NullCrownService final : public CrownService {
public:
    bool available() const override { return false; }
    float take_rotation() override { return 0.0F; }
    bool pressed() const override { return false; }
};

// --- Haptics -----------------------------------------------------------

class HapticsService {
public:
    virtual ~HapticsService() = default;
    virtual bool available() const = 0;
    // strength in [0,1]; a no-op when not fitted.
    virtual void pulse(std::uint32_t duration_ms, float strength) = 0;
};

class NullHapticsService final : public HapticsService {
public:
    bool available() const override { return false; }
    void pulse(std::uint32_t, float) override {}
};

// --- Biometric (on-device user verification) --------------------------
// Proving the owner is present, on the device itself — the job Apple's Face ID
// does. On the current board this is unfitted (no camera); the custom board adds
// a face model over the camera, and voice/gesture can stand in meanwhile. This
// is what gates a passkey assertion (below), so "unlock" happens on the podbot.

struct BiometricResult {
    bool matched{false};
    float confidence{0.0F};  // 0..1
};

class BiometricService {
public:
    enum class Method : std::uint8_t { face, voice, gesture };

    virtual ~BiometricService() = default;
    virtual bool available(Method method) const = 0;
    // Capture a reference for `method`; false when unfitted.
    virtual bool enroll(Method method) = 0;
    // Present-user check. false (and matched=false) when unfitted or no match.
    virtual bool verify(Method method, BiometricResult &out) = 0;
};

class NullBiometricService final : public BiometricService {
public:
    bool available(Method) const override { return false; }
    bool enroll(Method) override { return false; }
    bool verify(Method, BiometricResult &out) override { out = {}; return false; }
};

// --- Passkeys (on-device WebAuthn / FIDO2 authenticator) --------------
// The device holds the private key in secure storage and signs challenges, so it
// IS the authenticator: sign-ins and payment step-ups are approved on the podbot,
// no phone required. The backend (better-auth passkey plugin) verifies the
// assertion — it does not care that the authenticator is an ESP32, not an iPhone.
// User verification is delegated to the BiometricService, so a passkey can only
// be used after the owner is confirmed present on the device.

struct PasskeyCredential {
    std::uint8_t credential_id[32]{};
    std::uint8_t public_key_cose[80]{};  // COSE_Key, EC2 P-256 (ES256)
    std::uint16_t public_key_len{0};
};

struct PasskeyAssertion {
    std::uint8_t authenticator_data[37]{};  // rpIdHash(32) | flags(1) | counter(4)
    std::uint8_t signature[72]{};           // DER ECDSA P-256 (max length)
    std::uint16_t signature_len{0};
    std::uint32_t counter{0};
};

class PasskeyService {
public:
    virtual ~PasskeyService() = default;
    virtual bool available() const = 0;  // secure key storage fitted
    virtual bool has_credential(const char *rp_id) const = 0;
    // Register: generate a P-256 keypair bound to (rp_id, user_id), return the
    // public key for the server to store. false when unfitted.
    virtual bool create_credential(const char *rp_id, const std::uint8_t *user_id,
                                   std::uint16_t user_id_len, PasskeyCredential &out) = 0;
    // Authenticate: sign `challenge` with the stored key. When require_uv, the
    // implementation MUST pass a BiometricService check first (face/gesture) or
    // return false — no verification, no signature.
    virtual bool get_assertion(const char *rp_id, const std::uint8_t *challenge,
                               std::uint16_t challenge_len, bool require_uv,
                               PasskeyAssertion &out) = 0;
};

class NullPasskeyService final : public PasskeyService {
public:
    bool available() const override { return false; }
    bool has_credential(const char *) const override { return false; }
    bool create_credential(const char *, const std::uint8_t *, std::uint16_t, PasskeyCredential &out) override { out = {}; return false; }
    bool get_assertion(const char *, const std::uint8_t *, std::uint16_t, bool, PasskeyAssertion &out) override { out = {}; return false; }
};

// --- Aggregate ---------------------------------------------------------
// The handle apps receive. Pointers are never null: absent hardware points at
// the shared null backends, so call sites need no guards beyond checking the
// return values (or `caps` when they want to hide UI up front).

struct Services {
    CameraService *camera{nullptr};
    NfcService *nfc{nullptr};
    GnssService *gnss{nullptr};
    CrownService *crown{nullptr};
    HapticsService *haptics{nullptr};
    BiometricService *biometric{nullptr};
    PasskeyService *passkey{nullptr};
    CapabilitySet caps{};

    // Baseline for the current Waveshare ESP32-S3 target: everything the
    // board actually has is flagged fitted, everything else routes to the
    // null backends until better hardware (or a simulator fake) replaces it.
    static Services waveshare_amoled_175c()
    {
        static NullCameraService null_camera;
        static NullNfcService null_nfc;
        static NullGnssService null_gnss;
        static NullCrownService null_crown;
        static NullHapticsService null_haptics;
        static NullBiometricService null_biometric;
        static NullPasskeyService null_passkey;
        Services services;
        services.camera = &null_camera;
        services.nfc = &null_nfc;
        services.gnss = &null_gnss;
        services.crown = &null_crown;
        services.haptics = &null_haptics;
        services.biometric = &null_biometric;
        services.passkey = &null_passkey;
        services.caps.set(Capability::display);
        services.caps.set(Capability::touch);
        services.caps.set(Capability::imu);
        services.caps.set(Capability::sound_energy);
        services.caps.set(Capability::battery_gauge);
        services.caps.set(Capability::wifi);
        services.caps.set(Capability::ble);
        return services;
    }
};

}  // namespace eyes
