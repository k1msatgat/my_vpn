#!/usr/bin/env python3
"""시험용 OIDC 서버. client/my_vpn_login 의 흐름을 IdP 없이 끝까지 돌려 보려고 만든다.

    tools/idp/mock_oidc.py <keys-dir> <port> <user> [--groups ops] [--ttl 600]

mint.py keys 로 만든 키로 토큰을 발급한다. 기기 인가는 첫 번째 폴링에 "아직"
(authorization_pending)을 한 번 돌려준 뒤 승인된 것으로 친다 — 사람이 브라우저에서 로그인한
것처럼. refresh token 으로 새 토큰도 준다. 127.0.0.1 에서만 듣는다.

실제 IdP 와의 호환은 이것으로 확인되지 않는다. 확인되는 것은 my_vpn_login 이 RFC 8628 의
순서(발견 -> 기기 코드 -> 폴링 -> 토큰 -> 갱신)대로 움직이는지다.
"""

import argparse
import http.server
import json
import os
import secrets
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mint  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument("keys")
    p.add_argument("port", type=int)
    p.add_argument("user")
    p.add_argument("--groups", default="")
    p.add_argument("--ttl", type=int, default=600)
    a = p.parse_args()

    base = "http://127.0.0.1:%d" % a.port
    groups = [g for g in a.groups.split(",") if g]
    state = {"polls": 0, "refresh": set(), "issued": 0}

    def issue():
        state["issued"] += 1
        rt = secrets.token_urlsafe(16)
        state["refresh"].add(rt)
        tok = mint.token(a.keys, a.user, groups=groups, ttl=a.ttl)
        return {"access_token": tok, "token_type": "Bearer", "expires_in": a.ttl, "refresh_token": rt}

    class Handler(http.server.BaseHTTPRequestHandler):
        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            print("mock_oidc: " + fmt % args, flush=True)

        def do_GET(self):
            if self.path == "/.well-known/openid-configuration":
                return self.reply(200, {"issuer": mint.ISSUER, "token_endpoint": base + "/token",
                                        "device_authorization_endpoint": base + "/device"})
            self.reply(404, {})

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            form = dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))
            if self.path == "/device":
                return self.reply(200, {"device_code": "dc", "user_code": "WDJB-MJHT",
                                        "verification_uri": base + "/activate", "interval": 1, "expires_in": 60})
            if self.path != "/token":
                return self.reply(404, {})
            if form.get("grant_type") == "urn:ietf:params:oauth:grant-type:device_code":
                state["polls"] += 1
                if state["polls"] == 1:
                    return self.reply(400, {"error": "authorization_pending"})
                return self.reply(200, issue())
            if form.get("grant_type") == "refresh_token" and form.get("refresh_token") in state["refresh"]:
                state["refresh"].discard(form["refresh_token"])     # 한 번 쓴 refresh token 은 버린다
                return self.reply(200, issue())
            self.reply(400, {"error": "invalid_grant"})

    http.server.HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
