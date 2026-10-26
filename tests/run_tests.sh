#!/usr/bin/env bash
# Runs devrun against small fake services and checks the results.
# usage: tests/run_tests.sh [path-to-devrun]

BIN="$(readlink -f "${1:-build/devrun}")"
if [ ! -x "$BIN" ]; then echo "devrun binary not found: $BIN"; exit 2; fi

PASS=0
FAIL=0
pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
check() { local name="$1"; shift; if "$@" >/dev/null 2>&1; then pass "$name"; else fail "$name"; fi; }

# Wait until a background devrun answers on its socket.
wait_up() {
    for _ in $(seq 50); do
        "$BIN" status >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

workdir() { D=$(mktemp -d); cd "$D" || exit 2; }
cleanup() { cd /; rm -rf "$D"; }

test_order() {
    echo "dependency ordering"
    workdir
    cat > devrun.conf << 'CONF'
[api]
cmd = echo api
needs = db, cache

[cache]
cmd = echo cache
needs = db

[db]
cmd = echo db
CONF
    out=$("$BIN" up 2>&1)
    check "services start in dependency order" grep -q "start order: db cache api" <<< "$out"
    cleanup
}

test_config_errors() {
    echo "config errors"
    workdir

    printf '[a]\ncmd = echo a\nneeds = b\n\n[b]\ncmd = echo b\nneeds = a\n' > devrun.conf
    out=$("$BIN" up 2>&1); code=$?
    check "circular dependency is reported" grep -q "circular dependency" <<< "$out"
    [ "$code" -eq 1 ] && pass "circular dependency exits with code 1" || fail "circular dependency exits with code 1"

    printf '[a]\ncmd = echo a\nneeds = zzz\n' > devrun.conf
    out=$("$BIN" up 2>&1)
    check "unknown dependency is reported" grep -q "unknown service 'zzz'" <<< "$out"

    printf 'oops\n' > devrun.conf
    out=$("$BIN" up 2>&1)
    check "line outside a section is reported" grep -q "line 1" <<< "$out"

    out=$("$BIN" up -f missing.conf 2>&1)
    check "missing config file is reported" grep -q "cannot open" <<< "$out"
    cleanup
}

test_restart() {
    echo "restart with backoff (takes ~6 s)"
    workdir
    printf '[crashy]\ncmd = exit 3\n' > devrun.conf
    out=$(timeout -s INT 6 "$BIN" up 2>&1)
    check "crash is detected with its exit code" grep -q "exited with code 3" <<< "$out"
    check "first restart waits 1 s" grep -q "restarting in 1 s" <<< "$out"
    check "second restart waits 2 s" grep -q "restarting in 2 s" <<< "$out"
    check "third restart waits 4 s" grep -q "restarting in 4 s" <<< "$out"
    cleanup
}

test_normal_exit_not_restarted() {
    echo "normal exit"
    workdir
    printf '[ok]\ncmd = echo fine\n' > devrun.conf
    out=$("$BIN" up 2>&1)
    check "exit code 0 is not restarted" bash -c '! grep -q restarting <<< "$1"' _ "$out"
    cleanup
}

test_health() {
    echo "health check (takes ~6 s)"
    workdir
    if ! command -v python3 >/dev/null; then echo "  skipped (python3 missing)"; cleanup; return; fi
    cat > devrun.conf << 'CONF'
[db]
cmd = sleep 2; exec python3 -m http.server 18099 --bind 127.0.0.1
ready = port 18099

[api]
cmd = echo api started; sleep 100
needs = db
CONF
    out=$(timeout -s INT 6 "$BIN" up 2>&1)
    r=$(grep -n "ready (port" <<< "$out" | head -1 | cut -d: -f1)
    a=$(grep -n "api started" <<< "$out" | head -1 | cut -d: -f1)
    if [ -n "$r" ] && [ -n "$a" ] && [ "$r" -lt "$a" ]; then
        pass "api starts only after db is ready"
    else
        fail "api starts only after db is ready"
    fi
    cleanup
}

test_daemon_and_logs() {
    echo "background mode, status and logs"
    workdir
    cat > devrun.conf << 'CONF'
[api]
cmd = echo hello-from-api; sleep 100
CONF
    "$BIN" up -d >/dev/null 2>&1
    if wait_up; then pass "background devrun answers on its socket"; else fail "background devrun answers on its socket"; fi

    ok=0
    for _ in $(seq 30); do
        "$BIN" logs api 2>/dev/null | grep -q "hello-from-api" && { ok=1; break; }
        sleep 0.1
    done
    [ "$ok" -eq 1 ] && pass "logs shows service output" || fail "logs shows service output"

    check "status shows the service as running" bash -c '"$1" status | grep -q running' _ "$BIN"
    check "logs for an unknown service is an error message" bash -c '"$1" logs nope | grep -q "no such service"' _ "$BIN"

    "$BIN" up -d > out.txt 2>&1
    check "second 'up' refuses to start twice" grep -q "already running" out.txt

    "$BIN" down >/dev/null 2>&1
    check "down removes the socket" test ! -e .devrun.sock
    cleanup
}

test_shutdown() {
    echo "clean shutdown (takes ~5 s)"
    workdir
    cat > devrun.conf << 'CONF'
[sleeper]
cmd = exec sleep 4242

[stubborn]
cmd = trap '' TERM; while true; do sleep 1; done
CONF
    "$BIN" up -d >/dev/null 2>&1
    wait_up
    sleep 1
    "$BIN" down >/dev/null 2>&1

    if pgrep -f "sleep 4242" >/dev/null; then fail "no leftover processes after down"; pkill -f "sleep 4242"; else pass "no leftover processes after down"; fi
    check "SIGTERM-ignoring service gets SIGKILL" grep -q "sending SIGKILL" .devrun.log
    check "polite service stops on SIGTERM" grep -q "sleeper.*stopped" .devrun.log
    cleanup
}

test_order
test_config_errors
test_normal_exit_not_restarted
test_restart
test_health
test_daemon_and_logs
test_shutdown

echo
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
