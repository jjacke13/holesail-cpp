// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/colors.hpp"
#include "holesail/logger.hpp"

#include <regex>
#include <vector>

using namespace holesail;

namespace {

struct Captured { Level level; std::string line; };

Logger make_capturing(bool enabled, Level min_level, std::vector<Captured>& out) {
    Logger log("Holesail", enabled, min_level);
    log.set_clock([] { return std::string("2026-07-28T00:00:00.000Z"); });
    log.set_sink([&out](Level l, std::string_view line) {
        out.push_back({l, std::string(line)});
    });
    return log;
}

}  // namespace

TEST(Colors, MatchBarelyColoursCodes) {
    EXPECT_EQ(colors::red("x"), "\x1b[31mx\x1b[0m");
    EXPECT_EQ(colors::green("x"), "\x1b[32mx\x1b[0m");
    EXPECT_EQ(colors::yellow("x"), "\x1b[33mx\x1b[0m");
    EXPECT_EQ(colors::blue("x"), "\x1b[34mx\x1b[0m");
    EXPECT_EQ(colors::magenta("x"), "\x1b[35mx\x1b[0m");
    EXPECT_EQ(colors::cyan("x"), "\x1b[36mx\x1b[0m");
    EXPECT_EQ(colors::bright_black("x"), "\x1b[90mx\x1b[0m");
    EXPECT_EQ(colors::bold("x"), "\x1b[1mx\x1b[0m");
    EXPECT_EQ(colors::dim("x"), "\x1b[2mx\x1b[0m");
    EXPECT_EQ(colors::underline("x"), "\x1b[4mx\x1b[0m");
}

// JS nests colour calls and every helper appends a full reset, so the
// nesting produces several trailing resets. Reproduce it.
TEST(Colors, NestingProducesStackedResets) {
    EXPECT_EQ(colors::cyan(colors::underline(colors::bold("x"))),
              "\x1b[36m\x1b[4m\x1b[1mx\x1b[0m\x1b[0m\x1b[0m");
}

TEST(Logger, FormatMatchesHolesailLogger) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Debug, out);
    log.info("Starting server");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].line,
              "\x1b[90m2026-07-28T00:00:00.000Z\x1b[0m "
              "\x1b[34m[Holesail]\x1b[0m "
              "\x1b[32m[INFO]\x1b[0m Starting server");
}

TEST(Logger, LevelColorsAndLabels) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Debug, out);
    log.debug("d"); log.info("i"); log.warn("w"); log.error("e");
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NE(out[0].line.find("\x1b[90m[DEBUG]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[1].line.find("\x1b[32m[INFO]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[2].line.find("\x1b[33m[WARN]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[3].line.find("\x1b[31m[ERROR]\x1b[0m"), std::string::npos);
}

TEST(Logger, DropsMessagesBelowMinLevel) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Warn, out);
    log.debug("no"); log.info("no"); log.warn("yes"); log.error("yes");
    EXPECT_EQ(out.size(), 2u);
}

TEST(Logger, DisabledEmitsNothing) {
    std::vector<Captured> out;
    auto log = make_capturing(false, Level::Debug, out);
    log.error("boom");
    EXPECT_TRUE(out.empty());
}

TEST(Logger, Iso8601HasMillisecondsAndZSuffix) {
    const std::string ts = Logger::iso8601_now();
    EXPECT_TRUE(std::regex_match(
        ts, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)")))
        << "got: " << ts;
}
