#!/usr/bin/env bash
# Runs a command while nats-server serves copies of test/fixtures/jetstream: without auth at $NATS_JS_TEST_URL, and
# with TLS and users (test/fixtures/auth/server.conf) at $NATS_JS_TEST_AUTH_URL.
#   test/fixtures/serve.sh <command> [args...]
# The server is $NATS_SERVER, or nats-server on the PATH. With NATS_SERVER_VERSIONS (CI, on Linux x86-64 and macOS
# arm64), the command runs once for each version listed, downloaded from the nats-server releases and checked against
# the SHA256 below.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=${NATS_JS_TEST_PORT:-42222}
WORK=build/nats-test

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) PLATFORM=linux-amd64 ;;
    Darwin-arm64) PLATFORM=darwin-arm64 ;;
    *) PLATFORM=unknown ;;
esac

# SHA256 of nats-server-v<version>-<platform>.tar.gz, from the release's SHA256SUMS
sha256_of() {
    case "$1-$PLATFORM" in
        2.10.29-linux-amd64) echo e314da74a83a1d3876db8814c27eb990fe729640fd6452025b441bcede8390da ;;
        2.10.29-darwin-arm64) echo d7d8277882679d072c12f200c7b7006ee19aece00d2a0d75c3fbf2bf7fbd08ba ;;
        2.15.1-linux-amd64) echo bf5edc7b1ce98e885c368cb43a886eb148dc5d0551d99f3cf6141fb5e1954c70 ;;
        2.15.1-darwin-arm64) echo ed0fe39222ec2e317c4d0e1a9cdc16c63ec523d94f9355d24a902a4db2655ffd ;;
        *) echo "serve.sh: no checksum for nats-server $1 on $PLATFORM" >&2; return 1 ;;
    esac
}

sha256() {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

download() {
    local version=$1
    local dir=build/nats-server-v$version
    if [ ! -x "$dir/nats-server" ]; then
        local expected tarball=build/nats-server-v$version.tar.gz
        expected=$(sha256_of "$version")
        mkdir -p "$dir"
        curl -fsSL -o "$tarball" \
            "https://github.com/nats-io/nats-server/releases/download/v$version/nats-server-v$version-$PLATFORM.tar.gz"
        if [ "$(sha256 "$tarball")" != "$expected" ]; then
            echo "serve.sh: $tarball does not match its SHA256" >&2
            rm -f "$tarball"
            return 1
        fi
        tar xzf "$tarball" -C "$dir" --strip-components 1
    fi
    echo "$dir/nats-server"
}

PIDS=""
stop() {
    if [ -n "$PIDS" ]; then
        kill $PIDS 2> /dev/null || true
        wait $PIDS 2> /dev/null || true
    fi
    PIDS=""
}
trap stop EXIT

# serve <nats-server> <name> <port> [nats-server options]
serve() {
    local server=$1 name=$2 port=$3
    shift 3
    rm -rf "${WORK:?}/$name" && mkdir -p "$WORK/$name" && cp -R test/fixtures/jetstream "$WORK/$name/"
    "$server" -js -a 127.0.0.1 -p "$port" -sd "$WORK/$name" "$@" > "$WORK/$name.log" 2>&1 &
    local pid=$!
    PIDS="$PIDS $pid"
    until grep -q "Server is ready" "$WORK/$name.log"; do
        if ! kill -0 "$pid" 2> /dev/null; then
            cat "$WORK/$name.log" >&2
            exit 1
        fi
        sleep 0.1
    done
}

run() {
    local server=$1
    shift
    echo "serve.sh: $("$server" --version)"
    serve "$server" plain "$PORT"
    serve "$server" auth "$((PORT + 1))" -c test/fixtures/auth/server.conf
    NATS_JS_TEST_URL=nats://127.0.0.1:$PORT NATS_JS_TEST_AUTH_URL=tls://127.0.0.1:$((PORT + 1)) "$@"
    stop
}

if [ -n "${NATS_SERVER_VERSIONS:-}" ]; then
    for version in $NATS_SERVER_VERSIONS; do
        server=$(download "$version")
        run "$server" "$@"
    done
else
    run "${NATS_SERVER:-nats-server}" "$@"
fi
