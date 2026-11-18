# Sourced by the test/benchmark runners (which run from news-platform/).
# A killed io_uring process can leave listener teardown in flight briefly after
# wait returns. Confirm startup from this process's log, retrying only bind-busy.
StartHttpServer() {
    local attempt tick status
    for ((attempt = 0; attempt < 100; ++attempt)); do
        : > "$server_log"
        ./build/news-platform "$@" >"$server_log" 2>&1 &
        server_pid=$!
        for ((tick = 0; tick < 500; ++tick)); do
            if grep -q '^HTTP server listening' "$server_log"; then
                kill -0 "$server_pid"
                return
            fi
            if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
            sleep 0.01
        done
        if kill -0 "$server_pid" 2>/dev/null; then
            echo 'HTTP server startup timed out.' >&2
            return 1
        fi
        status=0
        wait "$server_pid" || status=$?
        server_pid=
        if ((status != 1)) || ! grep -q '^TCP listen failed: Address already in use' "$server_log"; then
            cat "$server_log" >&2
            return 1
        fi
        sleep 0.05
    done
    cat "$server_log" >&2
    return 1
}
