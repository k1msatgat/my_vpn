#!/usr/bin/env python3
"""인증 핸드셰이크의 서버 쪽을 흉내 내는 테스트용 UDP 응답기.

진짜 서버(Linux, server/)가 기기 인증을 구현하기 전에 Windows 클라이언트의
핸드셰이크를 확인하려고 만들었다. 터널은 없다 — 데이터 패킷은 세기만 하고 버린다.
와이어 포맷은 lib/common/include/proto.h 의 주석과 같아야 한다.

    python mock_server.py --device <id-hex>:<key-hex>:<tunnel-ip> [--port 9000]
                          [--require-firewall] [--require-antivirus] [--min-build N]
                          [--policy-file PATH]

<id-hex> 와 <key-hex> 는 `vpn_client enroll` 이 찍어 주는 값이다.

--policy-file 을 주면 정책을 그 파일에서 읽는다. 포스처를 평가할 때마다 다시 읽으므로
서버를 띄워 둔 채 파일을 고쳐 "관리자가 정책을 바꿨다"를 흉내 낼 수 있다.
한 줄에 하나씩: require-firewall / require-antivirus / min-build=N
"""

import argparse
import hashlib
import hmac
import os
import socket
import struct
import time

VERSION = 2

REQ_HANDSHAKE, RES_HANDSHAKE, DATA, KEEPALIVE, CHALLENGE, AUTH, DENY = range(1, 8)
TYPE_NAME = {1: "REQ_HS", 2: "RES_HS", 3: "DATA", 4: "KEEPALIVE", 5: "CHALLENGE", 6: "AUTH", 7: "DENY"}

DENY_UNKNOWN_DEVICE, DENY_BAD_AUTH, DENY_TUN_IP, DENY_POSTURE = range(1, 5)
DENY_NAME = {1: "unknown device", 2: "bad auth", 3: "tunnel ip", 4: "posture"}

HDR_LEN = 8
ID_LEN, NONCE_LEN, MAC_LEN, POSTURE_LEN = 16, 16, 32, 8
STATE = {0: "unknown", 1: "off", 2: "on"}

NONCE_TTL_SEC = 5


def header(msg_type, session_idx=0):
    return struct.pack("!BBHI", VERSION, msg_type, 0, session_idx)


def posture_text(raw):
    build, firewall, antivirus = struct.unpack("!IBB2x", raw)
    return build, firewall, antivirus, "build=%d firewall=%s antivirus=%s" % (
        build, STATE.get(firewall, "?"), STATE.get(antivirus, "?"))


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--device", action="append", required=True, metavar="ID:KEY:TUNIP")
    ap.add_argument("--require-firewall", action="store_true")
    ap.add_argument("--require-antivirus", action="store_true")
    ap.add_argument("--min-build", type=int, default=0)
    ap.add_argument("--policy-file")
    args = ap.parse_args()

    devices = {}
    for spec in args.device:
        dev_id, key, tun_ip = spec.split(":")
        devices[bytes.fromhex(dev_id)] = {
            "key": bytes.fromhex(key),
            "tun_ip": socket.inet_aton(tun_ip),
            "nonce": None,
            "nonce_at": 0.0,
        }

    def policy():
        need_fw, need_av, min_build = args.require_firewall, args.require_antivirus, args.min_build
        if args.policy_file:
            need_fw = need_av = False
            min_build = 0
            with open(args.policy_file, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line == "require-firewall":
                        need_fw = True
                    elif line == "require-antivirus":
                        need_av = True
                    elif line.startswith("min-build="):
                        min_build = int(line.split("=", 1)[1])
        return need_fw, need_av, min_build

    def posture_ok(raw):
        need_fw, need_av, min_build = policy()
        build, firewall, antivirus, _ = posture_text(raw)
        if need_fw and firewall != 2:
            return False
        if need_av and antivirus != 2:
            return False
        return build >= min_build

    sessions = {}  # session_idx -> {"dev": 기기, "counter": 마지막으로 받아들인 keepalive counter}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    log("mock server on %s:%d, %d device(s)" % (args.bind, args.port, len(devices)))

    def deny(addr, reason, session_idx=0):
        log("  -> DENY (%s)" % DENY_NAME[reason])
        sock.sendto(header(DENY, session_idx) + bytes([reason]), addr)

    while True:
        pkt, addr = sock.recvfrom(2048)
        if len(pkt) < HDR_LEN:
            continue
        version, msg_type, _, session_idx = struct.unpack("!BBHI", pkt[:HDR_LEN])
        body = pkt[HDR_LEN:]
        if version != VERSION:
            log("drop: version %d from %s:%d" % (version, *addr))
            continue

        if msg_type == REQ_HANDSHAKE:
            if len(body) < 4 + ID_LEN:
                log("drop: short REQ_HS (%d bytes) — old client?" % len(pkt))
                continue
            tun_ip, dev_id = body[:4], body[4:4 + ID_LEN]
            log("REQ_HS tun_ip=%s device=%s" % (socket.inet_ntoa(tun_ip), dev_id.hex()))
            dev = devices.get(dev_id)
            if dev is None:
                deny(addr, DENY_UNKNOWN_DEVICE)
                continue
            dev["nonce"] = os.urandom(NONCE_LEN)
            dev["nonce_at"] = time.monotonic()
            log("  -> CHALLENGE")
            sock.sendto(header(CHALLENGE) + dev["nonce"], addr)

        elif msg_type == AUTH:
            signed_len = 4 + ID_LEN + NONCE_LEN + POSTURE_LEN
            if len(body) < signed_len + MAC_LEN:
                continue
            signed, mac = body[:signed_len], body[signed_len:signed_len + MAC_LEN]
            tun_ip, dev_id = signed[:4], signed[4:4 + ID_LEN]
            nonce = signed[4 + ID_LEN:4 + ID_LEN + NONCE_LEN]
            posture = signed[-POSTURE_LEN:]
            log("AUTH   device=%s %s" % (dev_id.hex(), posture_text(posture)[3]))

            dev = devices.get(dev_id)
            if dev is None:
                deny(addr, DENY_UNKNOWN_DEVICE)
                continue
            # nonce 는 한 번만 쓴다. 맞든 틀리든 여기서 버려야 같은 AUTH 를 다시 보낼 수 없다.
            issued, issued_at = dev["nonce"], dev["nonce_at"]
            dev["nonce"] = None
            fresh = issued is not None and time.monotonic() - issued_at <= NONCE_TTL_SEC
            want = hmac.new(dev["key"], signed, hashlib.sha256).digest()
            if not fresh or nonce != issued or not hmac.compare_digest(mac, want):
                deny(addr, DENY_BAD_AUTH)
                continue
            if tun_ip != dev["tun_ip"]:
                deny(addr, DENY_TUN_IP)
                continue
            if not posture_ok(posture):
                deny(addr, DENY_POSTURE)
                continue

            idx = struct.unpack("!I", os.urandom(4))[0] or 1
            sessions[idx] = {"dev": dev, "counter": -1}
            log("  -> RES_HS idx=%08x" % idx)
            sock.sendto(header(RES_HANDSHAKE, idx), addr)

        elif msg_type == KEEPALIVE:
            sess = sessions.get(session_idx)
            if sess is None:
                log("drop: KEEPALIVE for unknown session %08x" % session_idx)
                continue
            if len(body) < 8 + POSTURE_LEN + MAC_LEN:
                log("drop: KEEPALIVE without posture (%d bytes)" % len(pkt))
                continue
            # mac 은 헤더부터 포스처까지를 덮는다.
            signed_len = HDR_LEN + 8 + POSTURE_LEN
            want = hmac.new(sess["dev"]["key"], pkt[:signed_len], hashlib.sha256).digest()
            if not hmac.compare_digest(pkt[signed_len:signed_len + MAC_LEN], want):
                log("drop: KEEPALIVE bad mac")
                continue
            # 예전 keepalive 를 다시 보내 "방화벽 켜짐"이던 때의 보고를 되살리는 것을 막는다.
            counter = struct.unpack("!Q", body[:8])[0]
            if counter <= sess["counter"]:
                log("drop: KEEPALIVE replayed counter %d" % counter)
                continue
            sess["counter"] = counter

            posture = body[8:8 + POSTURE_LEN]
            log("KEEPALIVE idx=%08x counter=%d %s" % (session_idx, counter, posture_text(posture)[3]))
            if not posture_ok(posture):
                # 세션 중에 정책에 못 미치게 됐다. 알리고 세션을 내린다.
                del sessions[session_idx]
                deny(addr, DENY_POSTURE, session_idx)
                continue
            sock.sendto(header(KEEPALIVE, session_idx), addr)

        else:
            log("%s idx=%08x (%d bytes)" % (TYPE_NAME.get(msg_type, "type %d" % msg_type),
                                            session_idx, len(pkt)))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
