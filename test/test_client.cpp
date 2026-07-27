// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/client.hpp"
#include "holesail/keys.hpp"

using namespace holesail;

TEST(ClientResolve, PrefersCliOverRecordOverDefaults) {
    HolesailClient::Config cfg;
    Record rec;
    rec.host = "10.0.0.5";
    rec.port = 3000;
    rec.udp = true;

    auto r = HolesailClient::resolve(cfg, rec);
    EXPECT_EQ(r.port, 3000);
    EXPECT_EQ(r.host, "10.0.0.5");
    EXPECT_TRUE(r.udp);

    cfg.local_port = 9999;
    cfg.local_host = "127.0.0.1";
    cfg.udp = false;
    r = HolesailClient::resolve(cfg, rec);
    EXPECT_EQ(r.port, 9999);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_FALSE(r.udp);
}

TEST(ClientResolve, FallsBackToJsDefaultsWithNoRecord) {
    const HolesailClient::Config cfg;
    const auto r = HolesailClient::resolve(cfg, std::nullopt);
    EXPECT_EQ(r.port, 8989);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_FALSE(r.udp);
}

TEST(ClientResolve, RecordWithoutUdpKeyMeansTcp) {
    Record rec;
    rec.host = "127.0.0.1";
    rec.port = 8080;  // udp deliberately unset — the common JS record
    const HolesailClient::Config cfg;
    const auto r = HolesailClient::resolve(cfg, rec);
    EXPECT_FALSE(r.udp);
    EXPECT_EQ(r.port, 8080);
}

TEST(ClientTarget, SecureModeDerivesTheServersKeypair) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailClient::Config cfg;
    cfg.key = "this-is-a-32-char-holesail-key!!";
    cfg.secure = true;

    HolesailClient c(&loop, nullptr, cfg, logger);
    EXPECT_EQ(to_hex(c.target_public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    EXPECT_TRUE(c.uses_derived_identity());
    uv_loop_close(&loop);
}

TEST(ClientTarget, PublicModeDecodesTheKeyAsZ32) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailClient::Config cfg;
    cfg.key = "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    cfg.secure = false;

    HolesailClient c(&loop, nullptr, cfg, logger);
    EXPECT_EQ(to_hex(c.target_public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    EXPECT_FALSE(c.uses_derived_identity());
    uv_loop_close(&loop);
}
