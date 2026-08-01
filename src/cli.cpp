// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/cli.hpp"

#include "holesail/colors.hpp"

#include <sodium.h>
#include <uv.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

namespace holesail::cli {
namespace {

using colors::bold;
using colors::cyan;
using colors::dim;
using colors::green;
using colors::magenta;
using colors::red;
using colors::underline;
using colors::yellow;

// The JS reference reports its package version here; this is holesail-cpp's.
constexpr const char* kVersion = "0.1.1";
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr long kMaxPort = 65535;

const char* const kBadPort =
    "Error: Given port is not a valid number. Run holesail --help to see examples";

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// `type[0].toUpperCase() + type.slice(1)`
std::string capitalise(std::string_view text) {
    std::string out(text);
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// JS truthiness of a minimist value: absent is false, a bare flag is true, a
// numeric value is false only at zero, a string value is false only when empty.
bool truthy(const Args& args, std::string_view name) {
    if (args.boolean(name)) return true;
    const auto value = args.str(name);
    if (!value) return false;
    if (const auto number = args.num(name)) return *number != 0;
    return !value->empty();
}

// A port is "a valid number" only if it is one *and* a port. JS lets 70000
// through to Node, which then throws; the same message is a better answer.
bool valid_port(const Args& args, std::string_view name) {
    const auto number = args.num(name);
    return number && *number >= 0 && *number <= kMaxPort;
}

// JS: `--log` alone is INFO, a value is clamped to DEBUG..ERROR, an
// unparseable value falls back to INFO, and absent disables logging.
int log_level_of(const Args& args) {
    if (!args.has("log")) return -1;
    if (args.boolean("log")) return 1;
    const auto level = args.num("log");
    if (!level) return 1;
    return static_cast<int>(std::clamp(*level, 0L, 3L));
}

void set_value(Args& args, const std::string& name, std::string value) {
    args.flags.erase(name);
    args.values[name] = std::move(value);
}

// ---------------------------------------------------------------------------
// help.js
// ---------------------------------------------------------------------------

// The `--filemanager` command and its example are gone: divergence D1.
std::string help_general() {
    std::string out = "\n\n";
    out += bold("Usage:") + "\n";
    out += "  " + yellow("holesail [command] [options]") + "\n";
    out += "\n";
    out += "  \n";
    out += bold("Commands:") + "\n";
    out += "  " + yellow("--help <command>|-h") + "     Display help information.\n";
    out += "  " + yellow("--version|-v") + "            Display version information.\n";
    out += "  " + yellow("--live <port>") + "           Start a Holesail server on <port>.\n";
    out += "  " + yellow("--connect <key>") + "         Connect to a Holesail server.\n";
    out += "  " + yellow("--lookup <key>") + "          Lookup details for a Holesail connection key.\n";
    out += "\n";
    out += bold("Options:") + "\n";
    out += "  " + yellow("--host <host>") + "           Specify the host address (default: 127.0.0.1).\n";
    out += "  " + yellow("--udp") + "                   Use UDP instead of TCP.\n";
    out += "  " + yellow("--public") + "                Start in public mode for sharing.\n";
    out += "  " + yellow("--port <port>") + "           Use a custom port (default: 8989).\n";
    out += "  " + yellow("--log") + "                   Enable the debug log.\n";
    out += "\n";
    out += bold("Examples:") + "\n";
    out += "  " + yellow("holesail --live 3000") + "      Start a server on port 3000.\n";
    out += "  " + yellow("holesail <key>") + "            Connect to a server.\n";
    out += "\n";
    out += bold("Notes:") + "\n";
    out += "  Treat private keys like SSH keys. Public keys are shareable, but secure "
           "sensitive data with passwords or connectors.\n";
    return out;
}

std::string help_live() {
    std::string out = "\n  ";
    out += cyan("\nReverse proxy a specific port") + "    \n  ";
    out += bold("Usage:") + "\n  ";
    out += yellow("holesail --live <port> [options]") + "\n  ";
    out += bold("Options:") + "\n  ";
    out += yellow("--host <address>") + "   Use a custom host.\n  ";
    out += yellow("--udp") + "              Use UDP protocol.\n  ";
    out += yellow("--public") +
           "           Uses a different key to connect than the one used to start the server.\n  ";
    out += yellow("--key <key>") + "        Set a custom key.\n    ";
    return out;
}

std::string help_connect() {
    std::string out = "\n  ";
    out += cyan("\nConnect to a Holesail server") + "    \n  ";
    out += bold("Usage:") + "\n  ";
    out += yellow("holesail --connect <key> [options]") + "\n  ";
    out += yellow("holesail <key> [options]") + "\n  \n  ";
    out += bold("Options:") + "\n  ";
    out += yellow("--host <address>") + "   Use a custom host.\n  ";
    out += yellow("--udp") + "              Use UDP protocol.\n  ";
    out += yellow("--port <port>") + "      Use a custom port.\n  ";
    out += yellow("--public") +
           "           Force the connection to use public mode (otherwise autodetected).\n    ";
    return out;
}

// ---------------------------------------------------------------------------
// bin/holesail.mjs
// ---------------------------------------------------------------------------

Options common_options(const Args& args) {
    Options options;
    options.log_level = log_level_of(args);
    if (args.has("udp")) options.udp = truthy(args, "udp");
    return options;
}

Options server_options(const Args& args) {
    Options options = common_options(args);
    options.server = true;
    options.port = static_cast<uint16_t>(args.num("live").value_or(0));
    const auto host = args.str("host");
    options.host = (host && !host->empty()) ? *host : kDefaultHost;
    options.secure = args.has("public") ? !args.boolean("public") : true;
    options.key = args.str("key");
    return options;
}

Options client_options(const Args& args) {
    Options options = common_options(args);
    options.client = true;
    if (valid_port(args, "port")) options.port = static_cast<uint16_t>(*args.num("port"));
    if (const auto host = args.str("host"); host && !host->empty()) options.host = *host;
    options.key = args.str("connect").value_or(args.rest.empty() ? std::string() : args.rest.front());
    // Divergence D3 (quirk Q1): JS passes `secure: argv.public` here, so
    // --public made the link SECURE — the opposite of what --help says.
    // Leaving it unset keeps the `hs://` sniff in charge.
    if (args.has("public")) options.secure = !args.boolean("public");
    return options;
}

// Port of graceful-goodbye. `close()` only *schedules* the DHT teardown (see
// holesail.hpp), so the loop has to keep running afterwards — it does, because
// the reaper timer close() starts is an active handle.
struct Shutdown {
    uv_signal_t sigint{};
    uv_signal_t sigterm{};
    Holesail* conn = nullptr;
    bool installed = false;   // uv_signal_init opens a pipe; it can fail
    bool closing = false;
};

void begin_shutdown(Shutdown& shutdown) {
    if (shutdown.closing) return;   // a second Ctrl-C must not re-close handles
    shutdown.closing = true;
    if (shutdown.conn != nullptr) shutdown.conn->close();
    if (!shutdown.installed) return;
    uv_close(reinterpret_cast<uv_handle_t*>(&shutdown.sigint), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&shutdown.sigterm), nullptr);
}

void on_signal(uv_signal_t* handle, int /*signum*/) {
    begin_shutdown(*static_cast<Shutdown*>(handle->data));
}

// Everything holesail owns has gone through uv_close by the time this runs,
// but hyperdht leaves its interface-watcher timer stopped rather than closed,
// and one unclosed handle is enough to make uv_loop_close() return EBUSY and
// leak the loop with it. Close whatever is left; the process is exiting.
//
// ponytail: a workaround for a hyperdht-cpp teardown gap, not a fix — the
// watcher's own allocation still leaks over there. Drop the walk once hyperdht
// closes its handles.
int drain_and_close(uv_loop_t& loop) {
    uv_walk(
        &loop,
        [](uv_handle_t* handle, void*) {
            if (uv_is_closing(handle) == 0) uv_close(handle, nullptr);
        },
        nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    return uv_loop_close(&loop);
}

int run_endpoint(Options options) {
    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0) {
        std::cout << red("Error: could not create an event loop") << std::endl;
        return 1;
    }

    int status = 0;
    {
        std::unique_ptr<Holesail> conn;
        try {
            conn = std::make_unique<Holesail>(&loop, std::move(options));
        } catch (const OptionsError& error) {
            std::cout << red("Error: " + error.message) << std::endl;
            drain_and_close(loop);
            return 1;
        }

        Shutdown shutdown;
        shutdown.conn = conn.get();
        shutdown.sigint.data = &shutdown;
        shutdown.sigterm.data = &shutdown;
        shutdown.installed = uv_signal_init(&loop, &shutdown.sigint) == 0 &&
                             uv_signal_init(&loop, &shutdown.sigterm) == 0;
        if (shutdown.installed) {
            uv_signal_start(&shutdown.sigint, &on_signal, SIGINT);
            uv_signal_start(&shutdown.sigterm, &on_signal, SIGTERM);
        } else {
            // Without handlers the default disposition still ends the process;
            // drain_and_close() collects whatever did get registered.
            std::cout << red("Warning: could not install the signal handlers") << std::endl;
        }

        const auto fail = [&](int err) {
            std::cout << red(std::string("Error: ") + uv_strerror(err)) << std::endl;
            status = 1;
            begin_shutdown(shutdown);
        };

        const int started = conn->ready([&](int err) {
            if (err < 0) {
                fail(err);
                return;
            }
            std::cout << render_started(conn->info()) << std::endl;
        });
        if (started < 0) fail(started);

        uv_run(&loop, UV_RUN_DEFAULT);
        conn->close();                    // idempotent — a no-op after a signal
        uv_run(&loop, UV_RUN_DEFAULT);    // drain the deferred DHT teardown
    }
    drain_and_close(loop);
    return status;
}

int run_lookup(const std::string& key) {
    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0) {
        std::cerr << red("Error during lookup:") << " could not create an event loop\n";
        return 0;
    }

    const int started =
        Holesail::lookup(&loop, key, [](int err, const std::optional<LookupResult>& result) {
            if (err < 0) {
                std::cerr << red("Error during lookup:") << " " << uv_strerror(err) << "\n";
                return;
            }
            std::cout << render_lookup(result) << std::endl;
        });

    if (started < 0) {
        // JS throws `Invalid key format: <key>` when the key is not 32 bytes of
        // z32; anything else surfaces as the libuv reason.
        const std::string reason =
            started == UV_EINVAL ? "Invalid key format: " + key : uv_strerror(started);
        std::cerr << red("Error during lookup:") << " " << reason << "\n";
    } else {
        uv_run(&loop, UV_RUN_DEFAULT);
    }
    drain_and_close(loop);
    return 0;   // the JS lookup branch exits 0 either way
}

}  // namespace

// ---------------------------------------------------------------------------
// minimist
// ---------------------------------------------------------------------------

bool Args::has(std::string_view name) const {
    const std::string key(name);
    return values.count(key) != 0 || flags.count(key) != 0;
}

std::optional<std::string> Args::str(std::string_view name) const {
    const auto it = values.find(std::string(name));
    if (it == values.end()) return std::nullopt;
    return it->second;
}

std::optional<long> Args::num(std::string_view name) const {
    const auto value = str(name);
    if (!value || value->empty()) return std::nullopt;
    long parsed = 0;
    const char* const end = value->data() + value->size();
    const auto result = std::from_chars(value->data(), end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return parsed;
}

bool Args::boolean(std::string_view name) const { return flags.count(std::string(name)) != 0; }

Args parse_argv(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; i++) {
        const std::string token = argv[i];
        std::string name;
        if (token.size() > 2 && token.compare(0, 2, "--") == 0) {
            name = token.substr(2);
        } else if (token.size() > 1 && token[0] == '-' && token[1] != '-' &&
                   std::isdigit(static_cast<unsigned char>(token[1])) == 0) {
            name = token.substr(1);   // -h; a digit here is a negative number
        } else {
            args.rest.push_back(token);
            continue;
        }

        if (const auto eq = name.find('='); eq != std::string::npos) {
            set_value(args, name.substr(0, eq), name.substr(eq + 1));
            continue;
        }
        // A flag only swallows the next token when that token is not itself an
        // option.
        if (i + 1 < argc && std::string_view(argv[i + 1]).compare(0, 2, "--") != 0) {
            set_value(args, name, argv[++i]);
        } else {
            args.values.erase(name);
            args.flags.insert(name);
        }
    }
    return args;
}

// ---------------------------------------------------------------------------
// validateInput.js
// ---------------------------------------------------------------------------

Validation validate(const Args& args) {
    // The UDP/host check only warns, so an error printed later has to keep its
    // line — `message` carries both, in the order JS logs them.
    std::string log;
    const auto with = [&log](const std::string& line) {
        return log.empty() ? line : log + "\n" + line;
    };

    // Quirk Q3, reproduced: `process.exit()` with no argument — status 0.
    if (args.boolean("key")) return {false, with(red("Error: Key can not be empty")), 0};

    if (truthy(args, "udp")) {
        const auto host = args.str("host");
        if (host && (*host == "localhost" || *host == "0.0.0.0")) {
            log = yellow(
                "Warning: localhost or 0.0.0.0 may not work properly within netcat with UDP.");
        }
    }

    if (const auto key = args.str("key");
        key && !key->empty() && key->size() < 32 && !truthy(args, "force")) {
        return {false,
                with(red("Error: A key should have a minimum length of 32 chars for security "
                         "purposes. If you still wish to proceed use --force")),
                2};
    }

    if (truthy(args, "connect") && !args.rest.empty()) {
        return {false,
                with(red("Error: Are you trying to use two connection strings at once? Get some "
                         "holesail --help")),
                2};
    }

    if (truthy(args, "live") && !valid_port(args, "live")) return {false, with(red(kBadPort)), 2};

    if (truthy(args, "port")) {
        if (!valid_port(args, "port")) return {false, with(red(kBadPort)), 2};
    } else if (const auto port = args.str("port"); port && port->empty()) {
        return {false, with(red(kBadPort)), 2};   // `--port=`
    }

    return {true, log, 0};
}

// ---------------------------------------------------------------------------
// help.js / stdout.js
// ---------------------------------------------------------------------------

std::string render_help(std::string_view topic) {
    const std::string header = cyan("Holesail CLI - Instantly share and connect to servers");
    if (topic == "live") return header + help_live();
    if (topic == "connect") return header + help_connect();
    return header + help_general();
}

std::string render_started(const Info& info) {
    const bool server = info.type == "server";
    const std::string where = info.host + ":" + std::to_string(info.port);

    std::string out = cyan(underline(bold("Holesail " + upper(info.protocol) + " " +
                                          capitalise(info.type) + " Started")));
    out += " ⛵️\n";
    out += magenta("Connection Mode: ");
    out += info.secure ? green("Private Connection String") : yellow("Public Connection String");
    out += "\n";
    // JS colours only the label of the server line and leaves `host:port`
    // plain; both lines are wrapped whole here so the sentence stays one
    // uninterrupted run of text.
    out += server ? magenta("Holesail is now listening on " + where)
                  : magenta("Access application on http://" + where + "/");
    out += "\n";
    out += server ? "Connect with key: " : "Connected to key: ";
    out += dim(info.url);
    out += "\n";
    out += info.secure
               ? dim("   NOTE: TREAT PRIVATE CONNECTION STRINGS HOW YOU WOULD TREAT SSH KEY, DO "
                     "NOT SHARE IT WITH ANYONE YOU DO NOT TRUST    ")
               : dim("   NOTICE: TREAT PUBLIC STRING LIKE YOU WOULD TREAT A DOMAIN NAME ON PUBLIC "
                     "SERVER, IF THERE IS ANYTHING PRIVATE ON IT, IT IS YOUR RESPONSIBILITY TO "
                     "PASSWORD PROTECT IT OR USE PRIVATE MODE   \n");
    return out;
}

std::string render_lookup(const std::optional<LookupResult>& result) {
    if (!result) return red("No record found for the provided key.");

    // `data.host || 'N/A'` — an empty host or a zero port is falsy in JS.
    std::string out = cyan(underline(bold("Holesail Lookup Result"))) + " \U0001f50d\n";
    out += magenta("Host: ") + green(result->host.empty() ? "N/A" : result->host) + "\n";
    out += magenta("Port: ") +
           green(result->port == 0 ? "N/A" : std::to_string(result->port)) + "\n";
    out += magenta("Protocol: ") +
           green(result->protocol.empty() ? "N/A" : upper(result->protocol)) + "\n";
    out += magenta("Private: ") + green(result->secure ? "Yes" : "No");
    return out;
}

// ---------------------------------------------------------------------------
// The dispatch, in the order bin/holesail.mjs uses
// ---------------------------------------------------------------------------

int run(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cout << red("Error: could not initialise libsodium") << std::endl;
        return 1;
    }

    const Args args = parse_argv(argc, argv);

    const Validation validation = validate(args);
    if (!validation.message.empty()) std::cout << validation.message << std::endl;
    if (!validation.ok) return validation.exit_code;

    if (truthy(args, "help") || truthy(args, "h")) {
        // JS reads the topic off `--help` only, so `-h live` prints the general
        // help.
        std::cout << render_help(args.str("help").value_or("")) << std::endl;
        return 0;
    }
    if (truthy(args, "version")) {
        std::cout << kVersion << std::endl;
        return 0;
    }
    if (truthy(args, "live")) return run_endpoint(server_options(args));
    if (truthy(args, "connect") || !args.rest.empty()) return run_endpoint(client_options(args));
    if (truthy(args, "filemanager")) {
        // Divergence D1 — better a refusal than silently printing the help.
        std::cout << "Error: --filemanager is not supported by holesail-cpp" << std::endl;
        return 2;
    }
    if (truthy(args, "lookup")) return run_lookup(args.str("lookup").value_or(""));

    std::cout << render_help("") << std::endl;
    return 0;
}

}  // namespace holesail::cli
