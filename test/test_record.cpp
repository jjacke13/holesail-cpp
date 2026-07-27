// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/record.hpp"

using namespace holesail;

// JSON.stringify drops undefined values, so the common record has NO udp key.
TEST(RecordEncode, MatchesJsStringifyByteForByte) {
    EXPECT_EQ(encode_record("127.0.0.1", std::nullopt, 8080),
              R"({"host":"127.0.0.1","port":8080})");
    EXPECT_EQ(encode_record("127.0.0.1", true, 8080),
              R"({"host":"127.0.0.1","udp":true,"port":8080})");
    EXPECT_EQ(encode_record("127.0.0.1", false, 8080),
              R"({"host":"127.0.0.1","udp":false,"port":8080})");
}

// --host is user input and lands inside a JSON string literal.
TEST(RecordEncode, EscapesTheHostString) {
    EXPECT_EQ(encode_record(R"(a"b)", std::nullopt, 1),
              R"({"host":"a\"b","port":1})");
    EXPECT_EQ(encode_record(R"(a\b)", std::nullopt, 1),
              R"({"host":"a\\b","port":1})");
    EXPECT_EQ(encode_record("a\nb", std::nullopt, 1),
              R"({"host":"a\nb","port":1})");
}

TEST(RecordDecode, ReadsJsProducedRecords) {
    auto r = decode_record(R"({"host":"127.0.0.1","port":8080})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "127.0.0.1");
    EXPECT_FALSE(r->udp.has_value());
    ASSERT_TRUE(r->port.has_value());
    EXPECT_EQ(*r->port, 8080);

    r = decode_record(R"({"host":"0.0.0.0","udp":true,"port":53})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "0.0.0.0");
    ASSERT_TRUE(r->udp.has_value());
    EXPECT_TRUE(*r->udp);
    EXPECT_EQ(*r->port, 53);
}

TEST(RecordDecode, ToleratesWhitespaceReorderingAndUnknownKeys) {
    const auto r = decode_record(
        R"({ "port" : 9000 , "extra" : "ignored" , "udp" : false , "host" : "example" })");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "example");
    ASSERT_TRUE(r->udp.has_value());
    EXPECT_FALSE(*r->udp);
    EXPECT_EQ(*r->port, 9000);
}

TEST(RecordDecode, HandlesEscapesInTheHostString) {
    const auto r = decode_record(R"({"host":"a\"b\\c\nd","port":1})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "a\"b\\c\nd");
}

TEST(RecordDecode, RejectsMalformedInput) {
    EXPECT_FALSE(decode_record("").has_value());
    EXPECT_FALSE(decode_record("not json").has_value());
    EXPECT_FALSE(decode_record("{").has_value());
    EXPECT_FALSE(decode_record(R"({"host":"x")").has_value());   // unterminated
    EXPECT_FALSE(decode_record(R"({"host":"x\)").has_value());   // trailing escape
}

TEST(RecordDecode, RejectsOutOfRangePorts) {
    // The record still parses; only the port is reported absent, so the
    // client falls back to 8989 rather than failing outright.
    auto r = decode_record(R"({"host":"x","port":70000})");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->port.has_value());

    r = decode_record(R"({"host":"x","port":-1})");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->port.has_value());
}

TEST(RecordDecode, MissingHostYieldsEmptyHostNotFailure) {
    // The client falls back to 127.0.0.1 when host is absent, so an
    // otherwise-valid object without `host` must still parse.
    const auto r = decode_record(R"({"port":8080})");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->host.empty());
    EXPECT_EQ(*r->port, 8080);
}
