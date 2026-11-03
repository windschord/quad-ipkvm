#!/bin/bash
# Runs tools/https_smoke_auth.sh against tools/test/mock_device.py, once per injected defect, and checks that
#   - the script PASSES (exit 0) on the correct mock,
#   - the script FAILS (exit 1..63 = number of failed checks) on every defect (this is the proof that it would have caught #55),
#   - the script REFUSES (exit 64) without --allow-state-change and when the device is already set up,
#   - nothing secret is printed.
# Needs: bash, curl, openssl, python3. No device. Uses 127.0.0.1 and the port below (not 443, so no root).
#
# Usage: tools/test/run_smoke_auth_mock.sh
# Exit status: the number of unexpected results (0 = the smoke script behaves as intended).

set -u
DIR=$(cd "$(dirname "$0")" && pwd)
SMOKE="$DIR/../https_smoke_auth.sh"
PORT=${MOCK_PORT:-18443}
SECRET="Zq7-Smoke-Secret-Do-Not-Print-9x"

umask 077
TMP=$(mktemp -d)
MOCK_PID=""
cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
    -subj "/CN=quad-ipkvm.local" -keyout "$TMP/key.pem" -out "$TMP/cert.pem" 2>/dev/null \
    || { echo "openssl failed"; exit 99; }

BAD=0

start_mock() { # <mode> [state]
    MOCK_MODE=$1 MOCK_STATE=${2:-needs_setup} python3 "$DIR/mock_device.py" "$PORT" "$TMP/cert.pem" "$TMP/key.pem" &
    MOCK_PID=$!
    local i
    for i in $(seq 1 50); do
        if curl -k -s -m 1 -o /dev/null "https://127.0.0.1:$PORT/api/v1/auth/status"; then return 0; fi
        sleep 0.1
    done
    echo "mock did not start"; exit 99
}
stop_mock() { kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""; }

# run_case <name> <expectation: pass|fail|refuse> <mode> [state] [extra args before --allow-state-change...]
run_case() {
    local name=$1 want=$2 mode=$3 state=${4:-needs_setup} code
    start_mock "$mode" "$state"
    if [ "$want" = "refuse-noflag" ]; then
        QUAD_IPKVM_SMOKE_PASSWORD=$SECRET QUAD_IPKVM_SMOKE_PORT=$PORT \
            "$SMOKE" 127.0.0.1 quad-ipkvm.local > "$TMP/out" 2>&1
    else
        QUAD_IPKVM_SMOKE_PASSWORD=$SECRET QUAD_IPKVM_SMOKE_PORT=$PORT \
            "$SMOKE" --allow-state-change 127.0.0.1 quad-ipkvm.local > "$TMP/out" 2>&1
    fi
    code=$?
    stop_mock
    local ok=0
    case $want in
        pass) [ "$code" -eq 0 ] && ok=1 ;;
        fail) [ "$code" -ge 1 ] && [ "$code" -le 63 ] && ok=1 ;;
        refuse|refuse-noflag) [ "$code" -eq 64 ] && ok=1 ;;
    esac
    # Nothing secret may be printed: not the password, not a cookie assignment.
    if grep -q -- "$SECRET" "$TMP/out" || grep -q '__Host-session=' "$TMP/out"; then
        printf '  LEAK  %-28s a secret appears in the output\n' "$name"
        BAD=$((BAD + 1))
    fi
    if [ "$ok" -eq 1 ]; then
        printf '  OK    %-28s exit %s (expected %s)\n' "$name" "$code" "$want"
    else
        printf '  WRONG %-28s exit %s (expected %s)\n' "$name" "$code" "$want"
        sed 's/^/        | /' "$TMP/out" | tail -25
        BAD=$((BAD + 1))
    fi
}

echo "Correct device"
run_case "ok"                       pass   ok

echo "Guards (nothing may be changed)"
run_case "no --allow-state-change"  refuse-noflag ok
run_case "already set up"           refuse ok ready

echo "Injected defects (each must be detected)"
run_case "login: no Set-Cookie (#55)"       fail no_cookie
run_case "login: headers sent twice (#55)"  fail dup_headers
run_case "setup: headers sent twice"        fail setup_dup_headers
run_case "cookie: no Secure"                fail cookie_no_secure
run_case "cookie: no HttpOnly"              fail cookie_no_httponly
run_case "cookie: SameSite=Lax"             fail cookie_samesite_lax
run_case "cookie: Domain attribute"         fail cookie_with_domain
run_case "cookie: no Max-Age"               fail cookie_no_maxage
run_case "wrong password sets a cookie"     fail bad_login_sets_cookie
run_case "logout does not clear cookie"     fail logout_keeps_cookie
run_case "logout keeps session on server"   fail no_invalidate
run_case "/video accepts without session"   fail ws_noauth
run_case "/video rejects a valid session"   fail ws_always_deny

printf '\nunexpected results: %s\n' "$BAD"
exit "$BAD"
