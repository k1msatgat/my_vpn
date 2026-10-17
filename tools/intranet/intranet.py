#!/usr/bin/env python3
"""터널 안에 두는 시험용 사내 서비스. 터널을 지나온 요청을 실제로 받는 목적지다.

    sudo python3 tools/intranet/intranet.py [-b 10.0.0.1] [-p 8080] [-p 8081]
                 [-s /run/ztgate/status.sock] [-d devices.conf] [-l intranet.log]

서버는 클라이언트가 보낸 패킷을 풀어 tun 에 쓰기만 하고, 그 뒤는 커널이 받는다. 지금까지
그 패킷의 목적지는 ping 에 답하는 커널뿐이라 "닿는다 / 막힌다"까지만 볼 수 있었다. 이
서비스는 서버의 터널 주소에서 HTTP 로 듣고, 요청마다 다음을 한다.

- 요청한 쪽의 터널 주소로 서버의 상태 소켓에서 세션을 찾아, 어느 기기(라벨)가 어떤 포스처로
  붙어 있고 누가 로그인했는지 페이지로 돌려준다. 브라우저로 열면 "사내 포털"처럼 보인다.
- 요청을 한 줄씩 시간순으로 남긴다. 시나리오 시험에서 "그 시각에 그 기기의 요청이 여기까지
  왔는가"의 판정 근거가 된다. 서버의 감사 로그는 ACL 판정을, 이 로그는 실제 도착을 보여 준다.

포트를 둘 열고 ACL 에서 하나만 허용하면, 같은 브라우저에서 한쪽은 열리고 다른 쪽은 시간이
초과되는 것으로 ACL 을 보여 줄 수 있다.

터널 주소에만 바인드한다. 0.0.0.0 에서 들으면 인터넷 쪽 주소로도 열려 터널을 지나지 않은
요청이 섞인다. 그래서 서버가 tun 에 주소를 붙인 뒤에 띄워야 한다.

상태 소켓과 기기 목록은 600 이라 root 로 돌린다. 파이썬 표준 라이브러리만 쓴다 (3.9 이상).
"""

import argparse
import datetime
import html
import http.server
import json
import socket
import socketserver
import sys
import threading


def read_status(path):
    """상태 소켓에 붙어 JSON 한 덩어리를 받는다. 서버는 다 쓰면 닫는다 (server/include/status.h)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    try:
        s.connect(path)
        chunks = []
        while True:
            b = s.recv(65536)
            if not b:
                break
            chunks.append(b)
    finally:
        s.close()
    return json.loads(b"".join(chunks))


def read_labels(path):
    """기기 목록에서 id -> 라벨. 라벨은 서버가 등록할 때 줄 끝에 붙이는 '# label' 주석이다."""
    labels = {}
    if not path:
        return labels
    try:
        with open(path) as f:
            for line in f:
                body, sep, comment = line.partition("#")
                fields = body.split()
                if fields and sep:
                    labels[fields[0].lower()] = comment.strip()
    except OSError as e:
        print("intranet: cannot read %s: %s" % (path, e), file=sys.stderr)
    return labels


class Lookup:
    """요청마다 상태 소켓을 다시 읽는다. 세션은 몇 초 만에 바뀔 수 있고 (거절, 재연결),
    요청은 사람이 누르는 속도로 오므로 캐시할 이유가 없다."""

    def __init__(self, status_path, devices_path):
        self.status_path = status_path
        self.devices_path = devices_path

    def who(self, ip):
        info = {"tunnel_ip": ip, "session": None, "label": None, "error": None}
        try:
            st = read_status(self.status_path)
        except (OSError, ValueError) as e:
            info["error"] = "status socket: %s" % e
            return info
        for sess in st.get("sessions", []):
            if sess.get("tunnel_ip") == ip:
                info["session"] = sess
                info["label"] = read_labels(self.devices_path).get(sess.get("device_id", ""))
                break
        return info


class Log:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.f = open(path, "a", buffering=1) if path else None

    def write(self, line):
        with self.lock:
            print(line, flush=True)
            if self.f:
                self.f.write(line + "\n")


def page(port, info, now):
    sess = info["session"]
    rows = [("터널 주소", info["tunnel_ip"]), ("받은 포트", str(port)), ("시각", now)]
    if sess:
        p = sess["posture"]
        user = sess.get("user")
        rows += [
            ("기기", "%s (%s)" % (info["label"] or "라벨 없음", sess["device_id"])),
            ("바깥 주소", sess["endpoint"]),
            ("세션", "%s, %d초째" % (sess["session_idx"], sess["age_sec"])),
            ("포스처", "빌드 %d, 방화벽 %s, 백신 %s" % (p["os_build"], p["firewall"], p["antivirus"])),
            ("사용자", "%s (%s)" % (user["name"], ", ".join(user["groups"]) or "그룹 없음")
             if user else "로그인하지 않음"),
        ]
    else:
        rows.append(("세션", info["error"] or "이 주소의 세션이 서버에 없음"))
    body = "".join("<tr><th>%s</th><td>%s</td></tr>" % (html.escape(k), html.escape(v))
                   for k, v in rows)
    return ("<!doctype html><meta charset=utf-8><title>ztgate 사내 포털</title>"
            "<style>body{font-family:sans-serif;margin:2em}th{text-align:left;padding-right:1em}"
            "</style><h1>ztgate 사내 포털</h1><p>터널을 지나 여기까지 왔습니다.</p>"
            "<table>%s</table>" % body).encode()


def make_handler(port, lookup, log):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            ip = self.client_address[0]
            info = lookup.who(ip)
            now = datetime.datetime.now().isoformat(timespec="seconds")
            sess = info["session"]
            log.write("%s port=%d src=%s device=%s user=%s %s %s" % (
                now, port, ip,
                (info["label"] or sess["device_id"]) if sess else "-",
                sess["user"]["name"] if sess and sess.get("user") else "-",
                self.command, self.path))

            # /whoami 는 스크립트가 판정에 쓰는 JSON, 나머지는 사람이 보는 페이지
            if self.path == "/whoami":
                out = json.dumps({"port": port, "time": now, **info}).encode()
                ctype = "application/json"
            else:
                out = page(port, info, now)
                ctype = "text/html; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(out)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(out)

        def log_message(self, fmt, *args):
            pass	# 요청 로그는 위에서 한 줄로 남긴다

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description="ztgate test intranet service")
    ap.add_argument("-b", "--bind", default="10.0.0.1", help="server tunnel address")
    ap.add_argument("-p", "--port", type=int, action="append", help="repeatable, default 8080")
    ap.add_argument("-s", "--status", default="/run/ztgate/status.sock")
    ap.add_argument("-d", "--devices", help="device file, for labels")
    ap.add_argument("-l", "--log", help="also append request lines here")
    a = ap.parse_args()

    lookup = Lookup(a.status, a.devices)
    log = Log(a.log)
    servers = []
    for port in a.port or [8080]:
        try:
            servers.append(Server((a.bind, port), make_handler(port, lookup, log)))
        except OSError as e:
            sys.exit("intranet: cannot listen on %s:%d: %s (is the server up?)" % (a.bind, port, e))
        log.write("intranet: listening on %s:%d" % (a.bind, port))

    for srv in servers[1:]:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        servers[0].serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
