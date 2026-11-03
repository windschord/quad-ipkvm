#!/bin/bash
# HTTPS smoke test for the SUCCESS path of the authentication API (setup -> login -> status -> /video -> logout).
#
# WARNING: this script CHANGES THE DEVICE STATE. It sets the device password. The password stays set afterwards;
# the only way back to "needs_setup" is to erase the NVS partition (see the message printed at the end).
# Therefore it does nothing unless --allow-state-change is given, and it refuses to run (without changing
# anything) when the device is not in "needs_setup".
#
# Why it exists: tools/https_smoke.sh is read-only and never logs in, so it could not detect the bug where the
# PBKDF2 worker added the security headers a second time, used up max_resp_headers (8) and the login response
# lost its Set-Cookie (fixed in #55). This script walks the password-dependent path and checks, on every
# response, that each security header appears exactly once, and that the session cookie has the right shape.
#
# Usage:  tools/https_smoke_auth.sh --allow-state-change <device-ip> [allowed-hostname]
#   <device-ip>         IPv4 address of the device, e.g. 192.168.11.39
#   [allowed-hostname]  Host header to use (default: quad-ipkvm.local, the CONFIG_WEB_SERVER_ALLOWED_HOSTS default)
# Environment:
#   QUAD_IPKVM_SMOKE_PASSWORD  password to set (printable ASCII, at least 10 characters). Default: a random
#                              24-character password that is generated here and never printed.
#   QUAD_IPKVM_SMOKE_PORT      HTTPS port (default 443). Only for tools/test/run_smoke_auth_mock.sh, which runs
#                              this script against a mock server.
# Exit status: 0 = all checks passed; 1..63 = the number of failed checks (capped at 63);
#              64 = refused to run, nothing was changed (usage error, no --allow-state-change, device already set
#              up, device unreachable, bad environment). 64 never means a check failed.
#
# The password, the session cookie and the token are never written to stdout or stderr (no "set -x").
# They are kept only in a private temporary directory that is removed on exit. The password is passed to curl
# in a file, not on the command line, so it does not show up in `ps`.

set -u

usage() {
    sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
}

ALLOW=0
if [ $# -ge 1 ] && [ "$1" = "--allow-state-change" ]; then
    ALLOW=1
    shift
fi
if [ $# -ge 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
    usage
    exit 0
fi
if [ "$ALLOW" -ne 1 ]; then
    printf 'Refusing to run: this test sets the device password (state change).\n'
    printf 'The password stays set; going back to needs_setup requires erasing the NVS partition.\n'
    printf 'Run again with --allow-state-change if you accept that.\n\n'
    usage
    exit 64
fi
if [ $# -lt 1 ]; then
    usage
    exit 64
fi

IP=$1
HOST=${2:-quad-ipkvm.local}
PORT=${QUAD_IPKVM_SMOKE_PORT:-443}
case $PORT in
    ''|*[!0-9]*) printf 'QUAD_IPKVM_SMOKE_PORT must be a number. Nothing was changed.\n'; exit 64 ;;
esac
if [ "$PORT" = "443" ]; then BASE="https://$HOST"; else BASE="https://$HOST:$PORT"; fi
RES="--resolve $HOST:$PORT:$IP"
SEC_HEADERS="Content-Security-Policy Cache-Control X-Content-Type-Options Referrer-Policy"

umask 077
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILS=0

pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILS=$((FAILS + 1)); }

# fetch <label> <curl args...>: stores headers in $TMP/h.clean and the body in $TMP/b, prints the heading.
# (Same helper as tools/https_smoke.sh. It never prints headers or bodies.)
# --noproxy '*': the device is on the LAN and is reached by IP (--resolve); never through an HTTP(S) proxy.
# CURL_OK is 0 when curl itself failed (no HTTP response); the preflight uses it to tell "unreachable" apart.
fetch() {
    local label=$1
    shift
    printf '%s\n' "$label"
    rm -f "$TMP/h" "$TMP/b"
    CURL_OK=1
    local rc=0
    curl -k -sS -m 20 --noproxy '*' -D "$TMP/h" -o "$TMP/b" "$@" || rc=$?
    if [ "$rc" -ne 0 ]; then CURL_OK=0; fail "request failed (curl exit $rc)"; fi
    tr -d '\r' < "$TMP/h" > "$TMP/h.clean" 2>/dev/null || : > "$TMP/h.clean"
}

status_code() { head -1 "$TMP/h.clean" | awk '{print $2}'; }

status_is() {
    local want=$1 got
    got=$(status_code)
    if [ "$got" = "$want" ]; then pass "status $want"; else fail "status $want expected, got '${got:-none}'"; fi
}

# header_once <name>: the header must appear exactly once.
header_once() {
    local n
    n=$(grep -ic "^$1:" "$TMP/h.clean")
    if [ "$n" -eq 1 ]; then pass "$1 appears once"; else fail "$1 appears $n times (expected 1)"; fi
}

header_absent() {
    local n
    n=$(grep -ic "^$1:" "$TMP/h.clean")
    if [ "$n" -eq 0 ]; then pass "no $1"; else fail "$1 present ($n times), none expected"; fi
}

security_headers_once() {
    local h
    for h in $SEC_HEADERS; do header_once "$h"; done
}

body_has() {
    if grep -q -- "$1" "$TMP/b"; then pass "body has $1"; else fail "body does not have $1"; fi
}

# fetch_retry_busy <fetch args...>: like fetch, but repeats while the device answers 503 (PBKDF2 busy).
fetch_retry_busy() {
    local i
    for i in 1 2 3 4; do
        fetch "$@"
        [ "$(status_code)" = "503" ] || return 0
        printf '  (503 server_busy, retrying in 2 s)\n'
        sleep 2
    done
}

# check_session_cookie: the Set-Cookie of a successful login. Values are never printed.
check_session_cookie() {
    local line
    header_once Set-Cookie
    line=$(grep -i '^set-cookie:' "$TMP/h.clean" | head -1)
    if printf '%s' "$line" | grep -Eqi '^set-cookie: *__Host-session=[^;]+(;|$)'; then
        pass "cookie is __Host-session with a non-empty value"
    else
        fail "no __Host-session cookie with a value in Set-Cookie"
    fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)HttpOnly( *;|$)'; then pass "cookie has HttpOnly"; else fail "cookie lacks HttpOnly"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)Secure( *;|$)'; then pass "cookie has Secure"; else fail "cookie lacks Secure"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)SameSite=Strict( *;|$)'; then pass "cookie has SameSite=Strict"; else fail "cookie lacks SameSite=Strict"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)Path=/( *;|$)'; then pass "cookie has Path=/"; else fail "cookie lacks Path=/"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)Max-Age=[1-9][0-9]*( *;|$)'; then pass "cookie has a positive Max-Age"; else fail "cookie lacks a positive Max-Age"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)Domain='; then fail "cookie has a Domain attribute (not allowed with __Host-)"; else pass "cookie has no Domain attribute"; fi
}

# check_cookie_cleared: the Set-Cookie of a logout (Max-Age=0).
check_cookie_cleared() {
    local line
    header_once Set-Cookie
    line=$(grep -i '^set-cookie:' "$TMP/h.clean" | head -1)
    if printf '%s' "$line" | grep -Eqi '^set-cookie: *__Host-session=(;|$)'; then pass "cookie value is cleared"; else fail "cookie value is not cleared"; fi
    if printf '%s' "$line" | grep -Eqi '(^|; *)Max-Age=0( *;|$)'; then pass "cookie has Max-Age=0"; else fail "cookie lacks Max-Age=0"; fi
}

# ws_upgrade_code <origin> [curl args...]: prints the HTTP status of a WebSocket upgrade on /video ("000" = no
# HTTP response, i.e. the connection was closed). An accepted upgrade (101) keeps the connection open, so
# curl is stopped by -m and the code is still reported.
ws_upgrade_code() {
    local origin=$1
    shift
    # shellcheck disable=SC2086
    curl -k -s -m 4 --noproxy '*' --http1.1 $RES -o /dev/null -w '%{http_code}' \
        -H "Upgrade: websocket" -H "Connection: Upgrade" -H "Sec-WebSocket-Version: 13" \
        -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Origin: $origin" "$@" "$BASE/video"
}

ws_must_not_upgrade() {
    local code
    printf '%s\n' "$1"
    shift
    code=$(ws_upgrade_code "$@")
    if [ "$code" = "101" ]; then fail "WS upgrade accepted (101), must be rejected"; else pass "WS upgrade not accepted (status ${code:-none})"; fi
}

# --- the password -------------------------------------------------------------------------------------------
# Written into $TMP files only (printf is a shell builtin, so the value never appears in a process list).
if [ -n "${QUAD_IPKVM_SMOKE_PASSWORD:-}" ]; then
    PW=$QUAD_IPKVM_SMOKE_PASSWORD
    if printf '%s' "$PW" | LC_ALL=C grep -q '[^ -~]'; then
        printf 'QUAD_IPKVM_SMOKE_PASSWORD must be printable ASCII only. Nothing was changed.\n'
        exit 64
    fi
else
    # Random alphanumerics. Only "admin" (case-insensitive) must be avoided; the other policy rules (all same,
    # sequential, common list) cannot realistically match 24 random characters.
    while :; do
        PW=$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 24)
        [ ${#PW} -ge 10 ] || continue
        case $(printf '%s' "$PW" | tr 'A-Z' 'a-z') in *admin*) continue ;; esac
        break
    done
fi
write_json_password() { # <password> <file>
    local esc
    esc=$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')
    printf '{"password":"%s"}' "$esc" > "$2"
}
write_json_password "$PW" "$TMP/pw_good.json"
write_json_password "WrongPassw0rd-smoke-test" "$TMP/pw_bad.json"
unset PW

AUTH="$BASE/api/v1/auth"
ORIGIN_JSON=(-H "Origin: $BASE" -H "Content-Type: application/json")

# --- preflight: refuse (without changing anything) unless the device is in needs_setup ----------------------
# shellcheck disable=SC2086
fetch "GET /api/v1/auth/status (preflight: is the device in needs_setup?)" $RES "$AUTH/status"
if [ "$CURL_OK" -ne 1 ] || [ "$(status_code)" != "200" ]; then
    printf 'The device is unreachable or did not answer the status request with 200. Nothing was changed.\n'
    exit 64
fi
if ! grep -q '"state":"needs_setup"' "$TMP/b"; then
    printf 'The device is already set up. Nothing was changed.\n'
    printf 'To run this test, erase the NVS partition first (see below), reboot, then run again.\n'
    printf 'NVS erase (default partition table: nvs at 0x9000, size 0x6000; also deletes the certificate, so the\n'
    printf 'browser asks again to accept the new self-signed one):\n'
    printf '  esptool --chip esp32p4 -p <port> erase-region 0x9000 0x6000\n'
    exit 64
fi
pass "device is in needs_setup"

# --- 1. setup ---------------------------------------------------------------------------------------------
# shellcheck disable=SC2086
fetch_retry_busy "POST /api/v1/auth/setup (sets the password)" $RES -X POST "${ORIGIN_JSON[@]}" \
    --data @"$TMP/pw_good.json" "$AUTH/setup"
status_is 200
if grep -qi '^content-type:.*application/json' "$TMP/h.clean"; then pass "Content-Type is application/json"; else fail "Content-Type is not application/json"; fi
security_headers_once

# --- 2. state after setup, without a session ----------------------------------------------------------------
# shellcheck disable=SC2086
fetch "GET /api/v1/auth/status (after setup, no cookie)" $RES "$AUTH/status"
status_is 200
body_has '"state":"ready"'
body_has '"authenticated":false'
security_headers_once

# --- 3. wrong password (one failure only: the lockout is 5 per IP per 5 minutes) ----------------------------
# shellcheck disable=SC2086
fetch_retry_busy "POST /api/v1/auth/login (wrong password)" $RES -X POST "${ORIGIN_JSON[@]}" \
    --data @"$TMP/pw_bad.json" "$AUTH/login"
status_is 401
body_has '"error":"unauthorized"'
header_absent Set-Cookie
security_headers_once

# --- 4. login ---------------------------------------------------------------------------------------------
# shellcheck disable=SC2086
fetch_retry_busy "POST /api/v1/auth/login (correct password)" $RES -X POST "${ORIGIN_JSON[@]}" \
    --data @"$TMP/pw_good.json" -c "$TMP/jar" "$AUTH/login"
status_is 200
check_session_cookie
security_headers_once

# --- 5. status with the cookie ------------------------------------------------------------------------------
# shellcheck disable=SC2086
fetch "GET /api/v1/auth/status (with the session cookie)" $RES -b "$TMP/jar" "$AUTH/status"
status_is 200
body_has '"authenticated":true'
security_headers_once

# --- 6. WebSocket /video ------------------------------------------------------------------------------------
ws_must_not_upgrade "WS upgrade on /video without a session (Origin: $BASE)" "$BASE"
ws_must_not_upgrade "WS upgrade on /video with the session but a foreign Origin" "https://evil.example" -b "$TMP/jar"
printf '%s\n' "WS upgrade on /video with the session cookie (Origin: $BASE)"
code=$(ws_upgrade_code "$BASE" -b "$TMP/jar")
if [ "$code" = "101" ]; then pass "WS upgrade accepted (101)"; else fail "WS upgrade with a valid session expected 101, got '${code:-none}'"; fi

# --- 7. logout ---------------------------------------------------------------------------------------------
cp "$TMP/jar" "$TMP/jar.before_logout"
# shellcheck disable=SC2086
fetch "POST /api/v1/auth/logout (with the session cookie)" $RES -X POST -H "Origin: $BASE" \
    -b "$TMP/jar" -c "$TMP/jar" "$AUTH/logout"
status_is 200
check_cookie_cleared
security_headers_once

# --- 8. the old session must be dead on the server, not only cleared in the client ---------------------------
# shellcheck disable=SC2086
fetch "GET /api/v1/auth/status (replaying the cookie from before the logout)" $RES -b "$TMP/jar.before_logout" "$AUTH/status"
status_is 200
body_has '"authenticated":false'
ws_must_not_upgrade "WS upgrade on /video replaying the cookie from before the logout" "$BASE" -b "$TMP/jar.before_logout"

# --- summary -----------------------------------------------------------------------------------------------
printf '\nfailed checks: %s\n' "$FAILS"
printf '\nNOTE: the device password is now SET (to a value that was not printed).\n'
printf 'To go back to needs_setup, erase the NVS partition (this also deletes the certificate):\n'
printf '  esptool --chip esp32p4 -p <port> erase-region 0x9000 0x6000   # default partition table\n'
[ "$FAILS" -gt 63 ] && FAILS=63
exit "$FAILS"
