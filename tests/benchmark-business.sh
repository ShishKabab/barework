#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
source tests/server.sh
# Rotate each round to reduce ordering bias. Same native client and guest modules
# in every mode; separate pools use disjoint P-cores until X+Y exceeds capacity.
requests=${1:-300000}
concurrency=${2:-32}
per_connection=${REQUESTS_PER_CONNECTION:-10}
rounds=${ROUNDS:-3}
[[ "$requests" =~ ^[1-9][0-9]*$ && ${#requests} -le 9 && "$requests" -le 100000000 ]]
[[ "$concurrency" =~ ^[1-9][0-9]*$ && ${#concurrency} -le 4 && "$concurrency" -le 1024 ]]
[[ "$per_connection" =~ ^[1-9][0-9]*$ && ${#per_connection} -le 7 && "$per_connection" -le 1000000 ]]
[[ "$rounds" =~ ^[1-9][0-9]*$ && ${#rounds} -le 2 ]]
((requests % per_connection == 0 && 20000 % per_connection == 0))
configs=(local-0 local-1 local-2 local-4 local-6 main-1 main-2 main-4 main-6
    split-1-1 split-1-3 split-2-2 split-3-1 split-1-5 split-2-4 split-3-3 split-4-2 split-5-1)
if (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    echo 'Benchmark port 8080 is already in use.' >&2; exit 1
fi
./build.sh >&2
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iexternal/common -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/http_server.c external/common/os.c build/liburing/src/liburing.a -o build/http-server-test
output="build/benchmarks/business-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$output"
printf 'round,configuration,requests,concurrency,seconds,requests_per_second\n' > "$output/runs.csv"
printf 'requests_per_connection=%s\nrounds=%s\n' "$per_connection" "$rounds" > "$output/settings.txt"
server_pid=
Cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}
trap Cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
for ((round = 0; round < rounds; ++round)); do
    for ((index = 0; index < ${#configs[@]}; ++index)); do
        config=${configs[(index + round * 7) % ${#configs[@]}]}
        case "$config" in
            local-*) arguments=(--workers "${config#local-}" --business-dispatch local) ;;
            main-*) arguments=(--workers "${config#main-}" --business-dispatch main) ;;
            split-*) split=${config#split-}; arguments=(--workers "${split%-*}" --business-workers "${split#*-}") ;;
        esac
        server_log="$output/$round-$config-server.log"
        StartHttpServer "${arguments[@]}"
        timeout 180s ./build/http-server-test --benchmark 20000 "$concurrency" "$per_connection" > "$output/$round-$config-warmup.csv"
        if ! measurement=$(timeout 300s ./build/http-server-test --benchmark "$requests" "$concurrency" "$per_connection"); then
            cat "$server_log" >&2; exit 1
        fi
        kill -0 "$server_pid"
        printf '%s,%s,%s\n' "$round" "$config" "$measurement" | tee -a "$output/runs.csv"
        Cleanup
    done
done
printf 'configuration,median_requests_per_second,min,max\n' > "$output/summary.csv"
for config in "${configs[@]}"; do
    rates=()
    while IFS=, read -r round name count clients seconds rate; do
        if [[ "$name" == "$config" ]]; then rates+=("$rate"); fi
    done < "$output/runs.csv"
    mapfile -t sorted < <(printf '%s\n' "${rates[@]}" | sort -n)
    # Odd rounds give the middle observation; even rounds use the lower middle.
    median=${sorted[(rounds - 1) / 2]}
    printf '%s,%s,%s,%s\n' "$config" "$median" "${sorted[0]}" "${sorted[rounds - 1]}" >> "$output/summary.csv"
done
cat "$output/summary.csv"
printf 'Artifacts: %s\n' "$output"
