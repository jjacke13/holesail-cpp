// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/cli.hpp"

#include <vector>

using namespace holesail;
using namespace holesail::cli;

namespace {

Args parse(std::vector<const char*> argv_in) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("holesail"));
    for (const char* a : argv_in) argv.push_back(const_cast<char*>(a));
    return parse_argv(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(ArgvParser, HandlesTheThreeFlagForms) {
    const auto a = parse({"--live", "8080", "--host=0.0.0.0", "--udp"});
    EXPECT_EQ(a.num("live"), 8080);
    EXPECT_EQ(a.str("host"), "0.0.0.0");
    EXPECT_TRUE(a.boolean("udp"));
    EXPECT_FALSE(a.has("nope"));
}

TEST(ArgvParser, CollectsPositionals) {
    const auto a = parse({"hs://s000abc"});
    ASSERT_EQ(a.rest.size(), 1u);
    EXPECT_EQ(a.rest[0], "hs://s000abc");
}

TEST(ArgvParser, LogTakesAnOptionalValue) {
    EXPECT_TRUE(parse({"--log"}).boolean("log"));
    EXPECT_EQ(parse({"--log", "3"}).num("log"), 3);
}

// Rows straight out of validateInput.js.
TEST(Validate, EmptyKeyExitsZero) {
    const auto v = validate(parse({"--live", "8080", "--key"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("Key can not be empty"), std::string::npos);
    EXPECT_EQ(v.exit_code, 0);   // quirk Q3, reproduced
}

TEST(Validate, ShortKeyNeedsForce) {
    auto v = validate(parse({"--live", "8080", "--key", "tooshort"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("minimum length of 32 chars"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);

    v = validate(parse({"--live", "8080", "--key", "tooshort", "--force"}));
    EXPECT_TRUE(v.ok);
}

TEST(Validate, TwoConnectionStringsIsAnError) {
    const auto v = validate(parse({"--connect", "abc", "def"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("two connection strings"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, NonNumericLivePortIsAnError) {
    const auto v = validate(parse({"--live", "abc"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("not a valid number"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, NonNumericPortIsAnError) {
    const auto v = validate(parse({"--connect", "abc", "--port", "xyz"}));
    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, AcceptsAWellFormedServerInvocation) {
    EXPECT_TRUE(validate(parse({"--live", "8080"})).ok);
    EXPECT_TRUE(validate(parse({"--live", "8080", "--public"})).ok);
}

TEST(RenderStarted, ServerLinesMatchTheJsWording) {
    Info info;
    info.type = "server";
    info.protocol = "tcp";
    info.secure = true;
    info.host = "127.0.0.1";
    info.port = 8080;
    info.url = "hs://s000abc";

    const auto out = render_started(info);
    EXPECT_NE(out.find("Holesail TCP Server Started"), std::string::npos);
    EXPECT_NE(out.find("Private Connection String"), std::string::npos);
    EXPECT_NE(out.find("Holesail is now listening on 127.0.0.1:8080"), std::string::npos);
    EXPECT_NE(out.find("Connect with key: "), std::string::npos);
    EXPECT_NE(out.find("hs://s000abc"), std::string::npos);
    EXPECT_NE(out.find("TREAT PRIVATE CONNECTION STRINGS"), std::string::npos);
}

TEST(RenderStarted, ClientLinesMatchTheJsWording) {
    Info info;
    info.type = "client";
    info.protocol = "tcp";
    info.secure = false;
    info.host = "127.0.0.1";
    info.port = 8989;
    info.url = "hs://0000abc";

    const auto out = render_started(info);
    EXPECT_NE(out.find("Holesail TCP Client Started"), std::string::npos);
    EXPECT_NE(out.find("Public Connection String"), std::string::npos);
    EXPECT_NE(out.find("Access application on http://127.0.0.1:8989/"), std::string::npos);
    EXPECT_NE(out.find("Connected to key: "), std::string::npos);
    EXPECT_NE(out.find("NOTICE: TREAT PUBLIC STRING"), std::string::npos);
}

TEST(RenderHelp, ListsEveryImplementedCommand) {
    const auto out = render_help("");
    for (const char* needle : {"--live", "--connect", "--lookup", "--host",
                               "--udp", "--public", "--port", "--log", "--version"}) {
        EXPECT_NE(out.find(needle), std::string::npos) << needle;
    }
    // --filemanager is not implemented; it must not be advertised.
    EXPECT_EQ(out.find("--filemanager"), std::string::npos);
}

TEST(RenderLookup, PrintsAllFieldsAndHandlesAMiss) {
    LookupResult r{"127.0.0.1", 8080, "TCP", true};
    const auto out = render_lookup(r);
    EXPECT_NE(out.find("Holesail Lookup Result"), std::string::npos);
    EXPECT_NE(out.find("127.0.0.1"), std::string::npos);
    EXPECT_NE(out.find("8080"), std::string::npos);
    EXPECT_NE(out.find("TCP"), std::string::npos);
    EXPECT_NE(out.find("Yes"), std::string::npos);

    EXPECT_NE(render_lookup(std::nullopt).find("No record found"), std::string::npos);
}
