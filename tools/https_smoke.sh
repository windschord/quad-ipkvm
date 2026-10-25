#!/bin/bash
# Read-only HTTPS smoke test for a running quad-ipkvm device.
#
# Sends only GET requests and one harmless POST (no setup, no login), so it does not change the device state.
# Checks the status codes and, above all, that each security header appears exactly ONCE per response.
# (ESP-IDF's httpd_resp_set_hdr appends instead of replacing, so a handler that sets a header twice sends
# two headers, and a duplicated Content-Security-Policy makes the browser enforce both policies.)
#
# Usage:  tools/https_smoke.sh <device-ip> [allowed-hostname]
#   <device-ip>         IPv4 address of the device, e.g. 192.168.11.39
#   [allowed-hostname]  Host header to use for requests that must be accepted (default: quad-ipkvm.local,
#                       the CONFIG_WEB_SERVER_ALLOWED_HOSTS default)
# Exit status: the number of failed checks (0 = all passed).

set -u

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

IP=$1
HOST=${2:-quad-ipkvm.local}
BASE="https://$HOST"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILS=0

pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILS=$((FAILS + 1)); }

# fetch <label> <curl args...>: stores headers in $TMP/h and the body in $TMP/b, prints the heading.
fetch() {
    local label=$1
    shift
    printf '%s\n' "$label"
    rm -f "$TMP/h" "$TMP/b"
    curl -k -sS -m 20 -D "$TMP/h" -o "$TMP/b" "$@" || fail "request failed (curl exit $?)"
    tr -d '\r' < "$TMP/h" > "$TMP/h.clean" 2>/dev/null || : > "$TMP/h.clean"
}

status_is() {
    local want=$1 got
    got=$(head -1 "$TMP/h.clean" | awk '{print $2}')
    if [ "$got" = "$want" ]; then pass "status $want"; else fail "status $want expected, got '${got:-none}'"; fi
}

# header_once <name>: the header must appear exactly once.
header_once() {
    local n
    n=$(grep -ic "^$1:" "$TMP/h.clean")
    if [ "$n" -eq 1 ]; then pass "$1 appears once"; else fail "$1 appears $n times (expected 1)"; fi
}

header_matches() {
    local name=$1 pattern=$2
    if grep -i "^$name:" "$TMP/h.clean" | grep -qi -- "$pattern"; then
        pass "$name contains '$pattern'"
    else
        fail "$name does not contain '$pattern'"
    fi
}

SEC_HEADERS="Content-Security-Policy Cache-Control X-Content-Type-Options Referrer-Policy"
RES="--resolve $HOST:443:$IP"

# Pages and API responses that must carry every security header exactly once.
for path in / /app.css /app.js /api/v1/auth/status; do
    # shellcheck disable=SC2086
    fetch "GET $path" $RES "$BASE$path"
    status_is 200
    for h in $SEC_HEADERS; do header_once "$h"; done
done

# Content types and the page CSP of the HTML page.
# shellcheck disable=SC2086
fetch "GET / (content type and CSP)" $RES "$BASE/"
header_matches Content-Type "text/html"
header_matches Content-Security-Policy "script-src 'self'"
# shellcheck disable=SC2086
fetch "GET /app.css (content type)" $RES "$BASE/app.css"
header_matches Content-Type "text/css"
# shellcheck disable=SC2086
fetch "GET /app.js (content type)" $RES "$BASE/app.js"
header_matches Content-Type "javascript"
# shellcheck disable=SC2086
fetch "GET /api/v1/auth/status (content type)" $RES "$BASE/api/v1/auth/status"
header_matches Content-Type "application/json"

# Error paths (state-free).
# shellcheck disable=SC2086
fetch "GET /nonexistent" $RES "$BASE/nonexistent"
status_is 404
for h in $SEC_HEADERS; do header_once "$h"; done
# shellcheck disable=SC2086
fetch "POST / (method not allowed)" $RES -X POST -H "Origin: $BASE" "$BASE/"
status_is 405
for h in $SEC_HEADERS; do header_once "$h"; done

# Host header checks. Error responses must carry the security headers too (exactly once).
fetch "GET / with Host: evil.example (must be rejected)" -H "Host: evil.example" "https://$IP/"
status_is 403
for h in $SEC_HEADERS; do header_once "$h"; done
fetch "GET / with the IPv4 literal as Host (intended to be allowed, docs/HW_VERIFY.md D6)" "https://$IP/"
status_is 200

printf '\n%s\n' "failed checks: $FAILS"
exit "$FAILS"
