// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/keys.hpp"

#include <sodium.h>

using namespace holesail;

namespace {

std::vector<uint8_t> hexbytes(std::string_view hex) {
    auto out = from_hex(hex);
    EXPECT_TRUE(out.has_value()) << "bad hex literal in test: " << hex;
    return out.value_or(std::vector<uint8_t>{});
}

}  // namespace

TEST(Z32, MatchesJsVectors) {
    // hex input -> z32 output, captured from the JS `z32` module.
    const std::pair<const char*, const char*> kVectors[] = {
        {"", ""},
        {"00", "yy"},
        {"0001", "yyyo"},
        {"000102", "yyyor"},
        {"00010203", "yyyorya"},
        {"0001020304", "yyyoryar"},
        {"ff", "9h"},
        {"ffffffff", "999999a"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"},
        {"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
         "yrbygbyfyadoonekbcgy4doxnyetrrawnwmbqgy3depta8e6dhoy"},
    };
    for (const auto& [hex, z] : kVectors) {
        const auto bytes = hexbytes(hex);
        EXPECT_EQ(z32_encode(bytes), z) << "encoding " << hex;
        const auto back = z32_decode(z);
        ASSERT_TRUE(back.has_value()) << "decoding " << z;
        EXPECT_EQ(to_hex(back->data(), back->size()), hex) << "round-tripping " << z;
    }
}

TEST(Z32, RejectsCharactersOutsideTheAlphabet) {
    EXPECT_FALSE(z32_decode("!!!!").has_value());
    EXPECT_FALSE(z32_decode("yyyv").has_value());  // 'v' is not in the alphabet
    EXPECT_FALSE(z32_decode("YY").has_value());    // uppercase is not accepted
}

TEST(Keys, Sha256MatchesJsReference) {
    ASSERT_GE(sodium_init(), 0);
    const auto seed = sha256("this-is-a-32-char-holesail-key!!");
    EXPECT_EQ(to_hex(seed.data(), seed.size()),
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");
}

TEST(Keys, HexRoundTrip) {
    EXPECT_EQ(from_hex("zz"), std::nullopt);
    EXPECT_EQ(from_hex("abc"), std::nullopt);  // odd length
    const auto b = from_hex("00ff10");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(to_hex(b->data(), b->size()), "00ff10");
}

TEST(Keys, RandomHex32Is64LowercaseHexChars) {
    ASSERT_GE(sodium_init(), 0);
    const auto a = random_hex32();
    const auto b = random_hex32();
    EXPECT_EQ(a.size(), 64u);
    EXPECT_NE(a, b);
    EXPECT_TRUE(from_hex(a).has_value());
    for (const char c : a) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex character in " << a;
    }
}

TEST(UrlParser, SecureLink) {
    const auto p = parse_url("hs://s000abcdef");
    EXPECT_EQ(p.key, "abcdef");
    ASSERT_TRUE(p.secure.has_value());
    EXPECT_TRUE(*p.secure);
}

TEST(UrlParser, PublicLink) {
    const auto p = parse_url("hs://0000abcdef");
    EXPECT_EQ(p.key, "abcdef");
    EXPECT_FALSE(p.secure.has_value());  // JS leaves it `undefined`, never false
}

TEST(UrlParser, BareKeyPassesThrough) {
    const auto p = parse_url("az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    EXPECT_EQ(p.key, "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    EXPECT_FALSE(p.secure.has_value());
}

TEST(UrlParser, EmptyUrl) {
    const auto p = parse_url("");
    EXPECT_EQ(p.key, "");
    EXPECT_FALSE(p.secure.has_value());
}

TEST(UrlParser, ShortUrlIsNotTreatedAsPrefixed) {
    // JS requires url.substring(5, 9).length === 4, i.e. at least 9 chars.
    const auto p = parse_url("hs://s0");
    EXPECT_EQ(p.key, "hs://s0");
}

// Quirk Q2, reproduced deliberately: the `secure` sniff runs on ANY string,
// so a bare key whose 6th character is 's' reads as secure.
TEST(UrlParser, QuirkQ2BareKeyWithSAtIndexFiveSniffsSecure) {
    const auto p = parse_url("abcdesomething");
    EXPECT_EQ(p.key, "abcdesomething");
    ASSERT_TRUE(p.secure.has_value());
    EXPECT_TRUE(*p.secure);
}
