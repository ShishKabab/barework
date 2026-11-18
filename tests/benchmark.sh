#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
source tests/server.sh

# Usage: tests/benchmark.sh [requests-per-run] [concurrency] [worker-count ...]
requests=${1:-10000}
concurrency=${2:-32}
per_connection=${REQUESTS_PER_CONNECTION:-1}
if (($# >= 2)); then shift 2; else set --; fi
if (($# == 0)); then set -- 0 1 2 default; fi
[[ "$requests" =~ ^[1-9][0-9]*$ && ${#requests} -le 9 && "$requests" -le 100000000 ]]
[[ "$concurrency" =~ ^[1-9][0-9]*$ && ${#concurrency} -le 4 && "$concurrency" -le 1024 ]]
[[ "$per_connection" =~ ^[1-9][0-9]*$ && ${#per_connection} -le 7 && "$per_connection" -le 1000000 ]]
((requests % per_connection == 0))
for workers in "$@"; do
    [[ "$workers" == default || "$workers" =~ ^[0-9]+$ ]]
done
if (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    echo 'Benchmark port 8080 is already in use.' >&2
    exit 1
fi
./build.sh >&2
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iexternal/common -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/http_server.c external/common/os.c build/liburing/src/liburing.a -o build/http-server-test
server_pid=
Cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap Cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'workers,requests,concurrency,seconds,requests_per_second\n'
for workers in "$@"; do
    arguments=()
    if [[ "$workers" != default ]]; then arguments=(--workers "$workers"); fi
    server_log="build/benchmark-$workers.log"
    StartHttpServer "${arguments[@]}"
    if ! measurement=$(timeout 120s ./build/http-server-test --benchmark "$requests" "$concurrency" "$per_connection"); then
        cat "$server_log" >&2
        exit 1
    fi
    kill -0 "$server_pid"
    printf '%s,%s\n' "$workers" "$measurement"
    Cleanup
    server_pid=
done
