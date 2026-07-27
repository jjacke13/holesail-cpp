// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include <hyperdht/hyperdht.h>
#include <sodium.h>

#include <cstdio>
#include <string>

namespace {

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

}  // namespace

// The whole port rests on this: C++ SHA-256 of the key STRING, fed to
// hyperdht_keypair_from_seed, must yield the same public key that
// JS `HyperDHT.keyPair(sha256(key))` yields. Vector captured from
// holesail 2.4.1's dependency tree.
TEST(Smoke, KeyDerivationMatchesJsReference) {
    ASSERT_GE(sodium_init(), 0);

    const std::string key = "this-is-a-32-char-holesail-key!!";

    uint8_t seed[32];
    crypto_hash_sha256(seed, reinterpret_cast<const uint8_t*>(key.data()), key.size());
    EXPECT_EQ(to_hex(seed, 32),
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");

    hyperdht_keypair_t kp;
    hyperdht_keypair_from_seed(&kp, seed);
    EXPECT_EQ(to_hex(kp.public_key, 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
}
