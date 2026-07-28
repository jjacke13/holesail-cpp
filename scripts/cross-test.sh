#!/usr/bin/env bash
# Live interop harness: the C++ holesail against the unmodified JS reference.
#
#   nix develop --command ./scripts/cross-test.sh [path/to/holesail]
#
# Every case joins the PUBLIC HyperDHT and moves real HTTP through the tunnel,
# so this needs working network — it is not a unit test. It also needs python3
# (serves the origin being tunnelled), curl (drives it) and node (runs the JS
# holesail); the flake devShell provides all three, the bare host does not.
#
# Cases: secure and public, in both interop directions, plus --lookup reading a
# record a JS server published. The C++/C++ secure case is the baseline — when
# it fails too, the bug is not an interop bug.
#
# Timing is the thing that makes this harness flaky when shortened: a server
# needs ~30s on the public DHT before a client finds it, and the client needs
# ~30s more before the first byte flows. Both are tunable below.
set -euo pipefail

CPP_BIN="${1:-./build/holesail}"
# HOLESAIL_JS may point at the binary or at the install prefix that contains it.
JS_BIN="${HOLESAIL_JS:-/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1/bin/holesail}"
[ -d "$JS_BIN" ] && JS_BIN="$JS_BIN/bin/holesail"

ORIGIN_PORT="${HOLESAIL_ORIGIN_PORT:-18080}"
SERVER_WAIT="${HOLESAIL_SERVER_WAIT:-30}"
CLIENT_WAIT="${HOLESAIL_CLIENT_WAIT:-30}"
CURL_TIMEOUT="${HOLESAIL_CURL_TIMEOUT:-25}"
RUN_ID="${HOLESAIL_RUN_ID:-$(date +%s)}"
MARKER="hello-from-holesail-$RUN_ID"

command -v python3 >/dev/null || { echo "python3 not on PATH — run under nix develop" >&2; exit 1; }
command -v curl    >/dev/null || { echo "curl not on PATH — run under nix develop" >&2; exit 1; }
[ -x "$CPP_BIN" ] || { echo "no such binary: $CPP_BIN" >&2; exit 1; }
[ -x "$JS_BIN" ]  || { echo "no JS holesail at $JS_BIN — set HOLESAIL_JS" >&2; exit 1; }
CPP_BIN=$(realpath "$CPP_BIN")
JS_BIN=$(realpath "$JS_BIN")

workdir=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$workdir"
}
trap cleanup EXIT

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }

bin_for() { case "$1" in cpp) printf '%s' "$CPP_BIN" ;; js) printf '%s' "$JS_BIN" ;; esac; }

# A fresh key per case AND per run: a stale announce from an earlier run under
# the same key would send the client at a server that is no longer there.
# "holesail-cpp-crosstest-" + a unix timestamp already clears the 32-char
# minimum --key enforces; the loop covers a short HOLESAIL_RUN_ID override.
case_key() {
    local k="holesail-cpp-crosstest-${RUN_ID}-$1"
    while [ "${#k}" -lt 32 ]; do k="${k}0"; done
    printf '%s' "$k"
}

dump() {
    echo "--- $1 server log ---"; strip_ansi "$2" | tail -n 25
    echo "--- $1 client log ---"; [ -s "$3" ] && strip_ansi "$3" | tail -n 25 || echo "(empty)"
}

# run_case <name> <secure|public> <cpp|js server> <cpp|js client> <local port>
run_case() {
    local name=$1 mode=$2 server=$3 client=$4 local_port=$5
    local slog="$workdir/$name.server.log" clog="$workdir/$name.client.log"
    local key url code
    : >"$clog"
    key=$(case_key "$name")

    echo "=== $name: $server server <-> $client client, $mode ==="
    if [ "$mode" = secure ]; then
        "$(bin_for "$server")" --live "$ORIGIN_PORT" --host 127.0.0.1 --key "$key" >"$slog" 2>&1 &
    else
        "$(bin_for "$server")" --live "$ORIGIN_PORT" --host 127.0.0.1 --public >"$slog" 2>&1 &
    fi
    local server_pid=$!
    pids+=("$server_pid")
    sleep "$SERVER_WAIT"

    if [ "$mode" = secure ]; then
        url="hs://s000$key"
    else
        # Public mode mints a random keypair per start, so the connection string
        # only exists in the server's own output. Both implementations print it
        # as "Connect with key: hs://0000<z32>".
        url=$(strip_ansi "$slog" | grep -oE 'hs://[0-9a-z]+' | head -n1 || true)
    fi
    if [ -z "$url" ]; then
        echo "FAIL: $name (server never printed a connection string)"
        dump "$name" "$slog" "$clog"
        kill "$server_pid" 2>/dev/null || true
        return 1
    fi

    "$(bin_for "$client")" --connect "$url" --port "$local_port" --host 127.0.0.1 >"$clog" 2>&1 &
    local client_pid=$!
    pids+=("$client_pid")
    sleep "$CLIENT_WAIT"

    code=$(curl -s -o "$workdir/$name.body" -w '%{http_code}' \
                --max-time "$CURL_TIMEOUT" "http://127.0.0.1:$local_port/probe.txt" || true)
    local rc=0
    if [ "$code" = 200 ] && grep -q "$MARKER" "$workdir/$name.body"; then
        echo "PASS: $name (http=$code, body verified)"
    else
        echo "FAIL: $name (http=${code:-none}, url=$url)"
        dump "$name" "$slog" "$clog"
        rc=1
    fi
    kill "$server_pid" "$client_pid" 2>/dev/null || true
    sleep 2
    return "$rc"
}

# --lookup: the C++ binary reads the metadata record a JS server published.
lookup_case() {
    local name=lookup-js-record slog="$workdir/$name.server.log" out="$workdir/$name.out"
    local key
    key=$(case_key "$name")

    echo "=== $name: cpp --lookup <-> js server, secure ==="
    "$JS_BIN" --live "$ORIGIN_PORT" --host 127.0.0.1 --key "$key" >"$slog" 2>&1 &
    local server_pid=$!
    pids+=("$server_pid")
    sleep "$SERVER_WAIT"

    "$CPP_BIN" --lookup "hs://s000$key" >"$out" 2>&1 || true
    strip_ansi "$out"
    local rc=0
    if grep -q "Port: $ORIGIN_PORT" <(strip_ansi "$out") && grep -q "Private: Yes" <(strip_ansi "$out"); then
        echo "PASS: $name"
    else
        echo "FAIL: $name (no matching record)"
        echo "--- $name server log ---"; strip_ansi "$slog" | tail -n 25
        rc=1
    fi
    kill "$server_pid" 2>/dev/null || true
    sleep 2
    return "$rc"
}

results=()
failures=0
record() {
    local name=$1; shift
    if "$@"; then results+=("PASS  $name"); else results+=("FAIL  $name"); failures=$((failures + 1)); fi
}

# The origin every case tunnels: a static file with a run-unique marker, so a
# stale tunnel from an earlier run cannot make a case pass.
echo "$MARKER" >"$workdir/probe.txt"
python3 -m http.server "$ORIGIN_PORT" --bind 127.0.0.1 --directory "$workdir" >/dev/null 2>&1 &
pids+=($!)
sleep 2
curl -sf --max-time 5 "http://127.0.0.1:$ORIGIN_PORT/probe.txt" >/dev/null \
    || { echo "origin http server did not start on $ORIGIN_PORT" >&2; exit 1; }

echo "cpp=$CPP_BIN"
echo "js=$JS_BIN ($("$JS_BIN" --version 2>/dev/null | tr -d '\n'))"
echo "origin=127.0.0.1:$ORIGIN_PORT waits=${SERVER_WAIT}s/${CLIENT_WAIT}s run=$RUN_ID"
echo

record cpp-cpp-secure run_case cpp-cpp-secure secure cpp cpp 18081
record cpp-js-secure  run_case cpp-js-secure  secure cpp js  18082
record js-cpp-secure  run_case js-cpp-secure  secure js  cpp 18083
record cpp-js-public  run_case cpp-js-public  public cpp js  18084
record js-cpp-public  run_case js-cpp-public  public js  cpp 18085
record lookup         lookup_case

echo
echo "=== summary ==="
for line in "${results[@]}"; do echo "  $line"; done
if [ "$failures" -ne 0 ]; then
    echo "$failures case(s) failed."
    exit 1
fi
echo "All interop cases passed."
