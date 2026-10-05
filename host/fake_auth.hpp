#pragma once
// Host-only fakes for the on-device auth services. FakePasskeyService does REAL
// P-256 (ES256) crypto via OpenSSL — the same shape of signature the ESP32 secure
// element will produce over mbedTLS — so the whole create-credential → verify →
// sign flow is testable on the Mac before any hardware exists. NEVER shipped; the
// device implements the same PasskeyService interface over its secure storage.

#include "eyes/os/services.hpp"

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>
#include <string>

namespace eyes {

// A biometric you can script in tests: enroll once, set the next match result.
class FakeBiometricService final : public BiometricService {
public:
    bool available(Method) const override { return available_; }
    bool enroll(Method) override { enrolled_ = true; return true; }
    bool verify(Method, BiometricResult &out) override
    {
        out.matched = enrolled_ && next_match_;
        out.confidence = out.matched ? 0.99F : 0.0F;
        return out.matched;
    }
    void set_available(bool a) { available_ = a; }
    void set_next_match(bool m) { next_match_ = m; }

private:
    bool available_{true};
    bool enrolled_{false};
    bool next_match_{true};
};

// A software authenticator. Holds one P-256 credential; user verification is
// delegated to the biometric, so get_assertion signs only when the owner is
// confirmed present (require_uv) — no verification, no signature.
class FakePasskeyService final : public PasskeyService {
public:
    explicit FakePasskeyService(BiometricService *bio) : bio_(bio) {}
    ~FakePasskeyService() override
    {
        if (key_ != nullptr) { EVP_PKEY_free(key_); }
    }

    bool available() const override { return true; }
    bool has_credential(const char *rp_id) const override { return key_ != nullptr && rp_ == rp_id; }

    bool create_credential(const char *rp_id, const std::uint8_t *, std::uint16_t, PasskeyCredential &out) override
    {
        if (key_ != nullptr) { EVP_PKEY_free(key_); key_ = nullptr; }
        key_ = gen_p256();
        if (key_ == nullptr) { return false; }
        rp_ = rp_id;
        counter_ = 0;
        RAND_bytes(out.credential_id, static_cast<int>(sizeof(out.credential_id)));
        unsigned char *pub = nullptr;
        const std::size_t publen = EVP_PKEY_get1_encoded_public_key(key_, &pub); // 0x04||X||Y
        if (pub == nullptr || publen == 0 || publen > sizeof(out.public_key_cose)) {
            if (pub != nullptr) { OPENSSL_free(pub); }
            return false;
        }
        std::memcpy(out.public_key_cose, pub, publen);
        out.public_key_len = static_cast<std::uint16_t>(publen);
        OPENSSL_free(pub);
        return true;
    }

    bool get_assertion(const char *rp_id, const std::uint8_t *challenge, std::uint16_t challenge_len,
                       bool require_uv, PasskeyAssertion &out) override
    {
        if (key_ == nullptr || rp_ != rp_id) { return false; }
        if (require_uv) {
            BiometricResult r;
            if (bio_ == nullptr || !bio_->verify(BiometricService::Method::face, r) || !r.matched) {
                return false; // present-user check failed → refuse to sign
            }
        }
        ++counter_;
        // authenticatorData = SHA256(rpId) | flags | counter(BE)
        SHA256(reinterpret_cast<const unsigned char *>(rp_id), std::strlen(rp_id), out.authenticator_data);
        out.authenticator_data[32] = static_cast<std::uint8_t>(require_uv ? 0x05 : 0x01); // UP | (UV)
        out.authenticator_data[33] = static_cast<std::uint8_t>((counter_ >> 24) & 0xFFU);
        out.authenticator_data[34] = static_cast<std::uint8_t>((counter_ >> 16) & 0xFFU);
        out.authenticator_data[35] = static_cast<std::uint8_t>((counter_ >> 8) & 0xFFU);
        out.authenticator_data[36] = static_cast<std::uint8_t>(counter_ & 0xFFU);
        out.counter = counter_;
        // signed data = authenticatorData | SHA256(challenge)   (challenge stands in for clientDataJSON)
        unsigned char signed_data[37 + 32];
        std::memcpy(signed_data, out.authenticator_data, 37);
        SHA256(challenge, challenge_len, signed_data + 37);
        EVP_MD_CTX *md = EVP_MD_CTX_new();
        std::size_t siglen = sizeof(out.signature);
        const bool ok = md != nullptr
            && EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, key_) == 1
            && EVP_DigestSign(md, out.signature, &siglen, signed_data, sizeof(signed_data)) == 1;
        if (md != nullptr) { EVP_MD_CTX_free(md); }
        if (!ok) { return false; }
        out.signature_len = static_cast<std::uint16_t>(siglen);
        return true;
    }

private:
    static EVP_PKEY *gen_p256()
    {
        EVP_PKEY *k = nullptr;
        EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (c != nullptr && EVP_PKEY_keygen_init(c) == 1
            && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c, NID_X9_62_prime256v1) == 1) {
            EVP_PKEY_keygen(c, &k);
        }
        if (c != nullptr) { EVP_PKEY_CTX_free(c); }
        return k;
    }

    BiometricService *bio_{nullptr};
    EVP_PKEY *key_{nullptr};
    std::string rp_;
    std::uint32_t counter_{0};
};

}  // namespace eyes
