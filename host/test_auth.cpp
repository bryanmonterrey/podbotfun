// Host self-test for the on-device passkey + biometric fakes. Proves the whole
// WebAuthn-style flow works with real P-256 crypto (create → verify → sign), and
// — the load-bearing security property — that NO biometric means NO signature.

#include "fake_auth.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/sha.h>

#include <cstdio>
#include <cstring>

using namespace eyes;

// Verify an assertion the way the backend would: rebuild the public key from the
// exported uncompressed point and check the ECDSA-SHA256 signature.
static bool verify_assertion(const PasskeyCredential &cred,
                             const std::uint8_t *challenge, std::uint16_t clen,
                             const PasskeyAssertion &a)
{
    char group[] = "prime256v1";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
            const_cast<std::uint8_t *>(cred.public_key_cose), cred.public_key_len),
        OSSL_PARAM_construct_end(),
    };
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY *pub = nullptr;
    const bool init = ctx != nullptr && EVP_PKEY_fromdata_init(ctx) == 1
        && EVP_PKEY_fromdata(ctx, &pub, EVP_PKEY_PUBLIC_KEY, params) == 1;
    if (ctx != nullptr) { EVP_PKEY_CTX_free(ctx); }
    if (!init || pub == nullptr) { if (pub != nullptr) { EVP_PKEY_free(pub); } return false; }

    unsigned char signed_data[37 + 32];
    std::memcpy(signed_data, a.authenticator_data, 37);
    SHA256(challenge, clen, signed_data + 37);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    const bool ok = md != nullptr
        && EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, pub) == 1
        && EVP_DigestVerify(md, a.signature, a.signature_len, signed_data, sizeof(signed_data)) == 1;
    if (md != nullptr) { EVP_MD_CTX_free(md); }
    EVP_PKEY_free(pub);
    return ok;
}

int main()
{
    int fails = 0;
    const auto check = [&fails](bool c, const char *w) {
        std::printf("  %-52s %s\n", w, c ? "ok" : "FAILED");
        if (!c) { ++fails; }
    };

    FakeBiometricService bio;
    FakePasskeyService pk(&bio);
    const char *rp = "api.podbot.fun";
    const std::uint8_t user[] = {1, 2, 3, 4};
    const std::uint8_t challenge[] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

    std::printf("on-device passkey + biometric:\n");

    PasskeyCredential cred{};
    check(pk.create_credential(rp, user, sizeof(user), cred), "create_credential returns a P-256 pubkey");
    check(cred.public_key_len == 65 && cred.public_key_cose[0] == 0x04, "pubkey is an uncompressed P-256 point (65B)");
    check(pk.has_credential(rp), "has_credential(rp) after create");

    bio.enroll(BiometricService::Method::face);
    bio.set_next_match(true);
    PasskeyAssertion a{};
    check(pk.get_assertion(rp, challenge, sizeof(challenge), true, a), "get_assertion signs with the owner present");
    check(verify_assertion(cred, challenge, sizeof(challenge), a), "the assertion verifies against the pubkey");
    check((a.authenticator_data[32] & 0x04) != 0, "UV flag set in authenticatorData");

    // The security property: no biometric match → no signature.
    bio.set_next_match(false);
    PasskeyAssertion blocked{};
    check(!pk.get_assertion(rp, challenge, sizeof(challenge), true, blocked), "no biometric → NO signature (fail-closed)");

    // A different challenge must not verify against this signature.
    bio.set_next_match(true);
    PasskeyAssertion a2{};
    pk.get_assertion(rp, challenge, sizeof(challenge), true, a2);
    const std::uint8_t other[] = {1, 1, 1, 1};
    check(!verify_assertion(cred, other, sizeof(other), a2), "a tampered challenge fails verification");

    // The null backend refuses politely (absent secure element).
    NullPasskeyService np;
    PasskeyCredential nc{};
    check(!np.available() && !np.create_credential(rp, user, sizeof(user), nc), "null passkey service refuses politely");

    std::printf("%s\n", fails == 0 ? "auth fake self-test passed" : "auth fake self-test FAILED");
    return fails == 0 ? 0 : 1;
}
