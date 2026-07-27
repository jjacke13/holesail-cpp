// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/keys.hpp"
#include "holesail/server.hpp"

using namespace holesail;

// JS: seq = old ? (same ? old.seq : old.seq + 1) : <default>
TEST(ServerSeq, MatchesJsPutRule) {
    EXPECT_EQ(HolesailServer::next_seq(false, 0, false), 0u);  // no prior record
    EXPECT_EQ(HolesailServer::next_seq(true, 7, true), 7u);    // unchanged value
    EXPECT_EQ(HolesailServer::next_seq(true, 7, false), 8u);   // changed value
}

TEST(ServerRefresh, IntervalIsFiftyMinutes) {
    EXPECT_EQ(HolesailServer::kRefreshIntervalMs, 3000000u);
}

TEST(ServerKey, SecureModeKeyIsZ32OfTheSeed) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailServer::Config cfg;
    cfg.local_port = 8080;
    cfg.secure = true;
    cfg.seed_hex = "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0";

    HolesailServer srv(&loop, nullptr, cfg, logger);
    EXPECT_EQ(srv.key(), "wuoiy1zuge9zc3q7d4uwm76g9bsuiw3kb5ffun85ioc9bwauq5ay");
    EXPECT_EQ(to_hex(srv.public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    uv_loop_close(&loop);
}

TEST(ServerKey, PublicModeKeyIsZ32OfThePublicKey) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailServer::Config cfg;
    cfg.local_port = 8080;
    cfg.secure = false;
    cfg.seed_hex = "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0";

    HolesailServer srv(&loop, nullptr, cfg, logger);
    EXPECT_EQ(srv.key(), "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    uv_loop_close(&loop);
}

TEST(ServerKey, EmptySeedGeneratesADistinctIdentityEachTime) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);
    HolesailServer::Config cfg;
    cfg.local_port = 1;
    HolesailServer a(&loop, nullptr, cfg, logger);
    HolesailServer b(&loop, nullptr, cfg, logger);
    EXPECT_NE(a.key(), b.key());
    uv_loop_close(&loop);
}
