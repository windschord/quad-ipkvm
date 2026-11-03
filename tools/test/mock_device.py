#!/usr/bin/env python3
"""Mock of the quad-ipkvm auth API, for testing tools/https_smoke_auth.sh WITHOUT a device.

It speaks HTTPS with a self-signed certificate and implements /api/v1/auth/{status,setup,login,logout} and a
WebSocket upgrade on /video, following docs/WEB_API.md. MOCK_MODE injects one defect at a time, so the smoke
script can be checked to FAIL on each of them (tools/test/run_smoke_auth_mock.sh runs the whole matrix).

Standard library only. Not part of the firmware, never run on the device.

Usage: mock_device.py <port> <cert.pem> <key.pem>     (environment: MOCK_MODE, MOCK_STATE)
"""
import http.server
import json
import os
import secrets
import ssl
import sys
import time

MODE = os.environ.get("MOCK_MODE", "ok")
STATE = {"state": os.environ.get("MOCK_STATE", "needs_setup"), "password": None, "sessions": set()}

SEC = [
    ("Content-Security-Policy", "default-src 'none'; script-src 'self'"),
    ("Cache-Control", "no-store"),
    ("X-Content-Type-Options", "nosniff"),
    ("Referrer-Policy", "no-referrer"),
]
COOKIE_NAME = "__Host-session"


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "mock"
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):  # keep the test output quiet
        pass

    # -- helpers ---------------------------------------------------------------------------------------------
    def session_ok(self):
        for part in self.headers.get("Cookie", "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == COOKIE_NAME and value in STATE["sessions"]:
                return True
        return False

    def reply(self, code, body=b"{}", extra=(), dup_security=False):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for name, value in SEC:
            self.send_header(name, value)
            if dup_security:
                self.send_header(name, value)
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def read_password(self):
        n = int(self.headers.get("Content-Length", "0"))
        try:
            return json.loads(self.rfile.read(n) or b"{}").get("password")
        except ValueError:
            return None

    def new_cookie(self):
        token = secrets.token_hex(16)
        STATE["sessions"].add(token)
        attrs = "HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400"
        if MODE == "cookie_no_secure":
            attrs = "HttpOnly; SameSite=Strict; Path=/; Max-Age=86400"
        if MODE == "cookie_no_httponly":
            attrs = "Secure; SameSite=Strict; Path=/; Max-Age=86400"
        if MODE == "cookie_samesite_lax":
            attrs = "HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=86400"
        if MODE == "cookie_with_domain":
            attrs += "; Domain=quad-ipkvm.local"
        if MODE == "cookie_no_maxage":
            attrs = "HttpOnly; Secure; SameSite=Strict; Path=/"
        return token, ("Set-Cookie", f"{COOKIE_NAME}={token}; {attrs}")

    # -- routes ----------------------------------------------------------------------------------------------
    def do_GET(self):
        if self.path == "/api/v1/auth/status":
            # compact, like the device ({"state":"ready","authenticated":false}); the smoke scripts grep for it
            body = json.dumps({"state": STATE["state"], "authenticated": self.session_ok()}, separators=(",", ":")).encode()
            return self.reply(200, body)
        if self.path == "/video" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.video()
        self.reply(404, b'{"error":"not_found"}')

    def video(self):
        origin_ok = self.headers.get("Origin") == "https://" + self.headers.get("Host", "")
        allowed = (self.session_ok() and origin_ok) or MODE == "ws_noauth"
        if MODE == "ws_always_deny":
            allowed = False
        if not allowed:
            self.close_connection = True  # like the device: no HTTP response at all
            return
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
        self.end_headers()
        self.wfile.flush()
        time.sleep(8)  # hold the connection open; the client stops after its own timeout

    def do_POST(self):
        if self.path == "/api/v1/auth/setup":
            if STATE["state"] != "needs_setup":
                return self.reply(409, b'{"error":"already_setup"}')
            password = self.read_password()
            if not password or len(password.encode()) < 10:
                return self.reply(400, b'{"error":"password_too_short"}')
            STATE["state"], STATE["password"] = "ready", password
            return self.reply(200, dup_security=(MODE == "setup_dup_headers"))
        if self.path == "/api/v1/auth/login":
            if STATE["state"] != "ready":
                return self.reply(403, b'{"error":"needs_setup"}')
            if self.read_password() != STATE["password"]:
                extra = [("Set-Cookie", f"{COOKIE_NAME}=bad; HttpOnly")] if MODE == "bad_login_sets_cookie" else []
                return self.reply(401, b'{"error":"unauthorized"}', extra)
            token, cookie = self.new_cookie()
            extra = [] if MODE == "no_cookie" else [cookie]
            return self.reply(200, extra=extra, dup_security=(MODE == "dup_headers"))
        if self.path == "/api/v1/auth/logout":
            if not self.session_ok():
                return self.reply(401, b'{"error":"unauthorized"}')
            if MODE != "no_invalidate":
                for part in self.headers.get("Cookie", "").split(";"):
                    name, _, value = part.strip().partition("=")
                    if name == COOKIE_NAME:
                        STATE["sessions"].discard(value)
            clear = f"{COOKIE_NAME}=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"
            if MODE == "logout_keeps_cookie":
                clear = f"{COOKIE_NAME}=x; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400"
            return self.reply(200, extra=[("Set-Cookie", clear)])
        self.reply(404, b'{"error":"not_found"}')


def main():
    port, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
