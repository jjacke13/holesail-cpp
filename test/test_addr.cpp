// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
//
// Regression cover for the "Error: invalid argument" field bug: a JS holesail
// server that published {"host":"localhost"} in its DHT record made every
// client here fail before it reached the tunnel, because uv_ip4_addr() is
// inet_pton-style and rejects names. The whole interop harness missed it —
// scripts/cross-test.sh passes --host 127.0.0.1 on every invocation, so a
// non-numeric host had never once been exercised.

#include "holesail/addr.hpp"

#include <gtest/gtest.h>
#include <uv.h>

#include <arpa/inet.h>

#include <string>

namespace {

std::string ip_of(const struct sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    uv_ip4_name(&addr, buf, sizeof(buf));
    return buf;
}

class AddrTest : public ::testing::Test {
  protected:
    void SetUp() override { ASSERT_EQ(uv_loop_init(&loop_), 0); }
    void TearDown() override {
        uv_run(&loop_, UV_RUN_NOWAIT);
        uv_loop_close(&loop_);
    }
    uv_loop_t loop_{};
};

TEST_F(AddrTest, NumericHostTakesTheFastPath) {
    struct sockaddr_in addr {};
    ASSERT_EQ(holesail::resolve_ip4(&loop_, "127.0.0.1", 8080, &addr), 0);
    EXPECT_EQ(ip_of(addr), "127.0.0.1");
    EXPECT_EQ(ntohs(addr.sin_port), 8080);
    EXPECT_EQ(addr.sin_family, AF_INET);
}

// The actual field failure. uv_ip4_addr() alone returns UV_EINVAL here.
TEST_F(AddrTest, LocalhostResolves) {
    struct sockaddr_in addr {};
    ASSERT_EQ(holesail::resolve_ip4(&loop_, "localhost", 8080, &addr), 0);
    EXPECT_EQ(ip_of(addr), "127.0.0.1");
    EXPECT_EQ(ntohs(addr.sin_port), 8080);
}

// Guards the bug this helper could easily have shipped with: getaddrinfo is
// asked for the name only, so it fills in port 0 and the port must be stamped
// on afterwards. Forget that line and every name-based bind lands on port 0.
TEST_F(AddrTest, PortSurvivesNameResolution) {
    struct sockaddr_in addr {};
    ASSERT_EQ(holesail::resolve_ip4(&loop_, "localhost", 45678, &addr), 0);
    EXPECT_EQ(ntohs(addr.sin_port), 45678);
    EXPECT_NE(ntohs(addr.sin_port), 0);
}

TEST_F(AddrTest, ZeroPortIsPreserved) {
    struct sockaddr_in addr {};
    ASSERT_EQ(holesail::resolve_ip4(&loop_, "127.0.0.1", 0, &addr), 0);
    EXPECT_EQ(ntohs(addr.sin_port), 0);
}

TEST_F(AddrTest, UnresolvableNameStillFails) {
    struct sockaddr_in addr {};
    // Reserved by RFC 6761 to never resolve, so this is a real negative and
    // not a flake against whatever the local resolver happens to answer.
    EXPECT_NE(holesail::resolve_ip4(&loop_, "nonexistent.invalid", 80, &addr), 0);
}

TEST_F(AddrTest, RejectsNullOut) {
    EXPECT_EQ(holesail::resolve_ip4(&loop_, "127.0.0.1", 80, nullptr), UV_EINVAL);
}

TEST_F(AddrTest, RejectsNullLoop) {
    struct sockaddr_in addr {};
    EXPECT_EQ(holesail::resolve_ip4(nullptr, "127.0.0.1", 80, &addr), UV_EINVAL);
}

}  // namespace
