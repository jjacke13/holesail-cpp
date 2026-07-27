// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/holesail.hpp"
#include "holesail/keys.hpp"

using namespace holesail;

TEST(Derive, ServerSecureWithAnExplicitKey) {
    Options o;
    o.server = true;
    o.secure = true;
    o.key = "this-is-a-32-char-holesail-key!!";
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key, "this-is-a-32-char-holesail-key!!");
    EXPECT_EQ(d.seed_hex,
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");
}

TEST(Derive, ServerSecureWithoutAKeyGenerates64HexChars) {
    Options o;
    o.server = true;
    o.secure = true;
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key.size(), 64u);
    EXPECT_EQ(d.seed_hex, to_hex(sha256(d.key).data(), 32));
}

TEST(Derive, ServerPublicWithoutAKeyLeavesSeedEmpty) {
    Options o;
    o.server = true;
    o.secure = false;
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
    EXPECT_TRUE(d.key.empty());
    EXPECT_TRUE(d.seed_hex.empty());  // HolesailServer generates one
}

TEST(Derive, ClientSecureSeedIsZ32OfTheHash) {
    Options o;
    o.client = true;
    o.key = "hs://s000this-is-a-32-char-holesail-key!!";
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key, "this-is-a-32-char-holesail-key!!");
}

TEST(Derive, ClientPublicKeepsTheKeyVerbatim) {
    Options o;
    o.client = true;
    o.key = "hs://0000az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
    EXPECT_EQ(d.key, "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
}

// Divergence D3 / spec quirk Q1: JS passes `secure: argv.public` on the client
// path, so --public turned the connection SECURE. We pass !--public.
TEST(Derive, D3ClientPublicFlagMeansPublicMode) {
    Options o;
    o.client = true;
    o.key = "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    o.secure = false;  // what main() sets when --public is present
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
}

// Quirk Q4, reproduced: an hs://s000 url beats --public.
TEST(Derive, Q4UrlSecurityWinsOverThePublicFlag) {
    Options o;
    o.client = true;
    o.key = "hs://s000this-is-a-32-char-holesail-key!!";
    o.secure = false;
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
}

TEST(MakeUrl, UsesTheRightModePrefix) {
    EXPECT_EQ(make_url(true, "abc"), "hs://s000abc");
    EXPECT_EQ(make_url(false, "abc"), "hs://0000abc");
}

TEST(Options, ValidateOptsRejectsBadCombinations) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    Options both;
    both.server = true;
    both.client = true;
    EXPECT_THROW({ Holesail h(&loop, both); }, OptionsError);

    Options client_no_key;
    client_no_key.client = true;
    EXPECT_THROW({ Holesail h(&loop, client_no_key); }, OptionsError);

    Options server_no_host;
    server_no_host.server = true;
    server_no_host.port = 8080;
    // JS validateOpts requires a host on the server path.
    EXPECT_THROW({ Holesail h(&loop, server_no_host); }, OptionsError);

    uv_loop_close(&loop);
}
