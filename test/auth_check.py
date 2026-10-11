#!/usr/bin/env python3
"""서버의 기기 인증을 프로토콜 수준에서 확인한다.

임시 기기 목록으로 vpn_server 를 띄우고, 클라이언트인 척 UDP 패킷을 직접 만들어
보내 응답을 본다. 터널 인터페이스에 주소를 붙이지 않으므로 데이터는 오가지 않는다.
TUN 을 만들어야 해서 root 로 돌린다.

    sudo python3 test/auth_check.py [port] [-v]      (-v: 서버 로그도 출력)

와이어 포맷은 lib/common/include/proto.h 의 주석과 같아야 한다.
"""

import hashlib
import hmac
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "server", "bin", "vpn_server")
PORT = int(next((a for a in sys.argv[1:] if a.isdigit()), 19000))
VERSION = 2

REQ_HANDSHAKE, RES_HANDSHAKE, DATA, KEEPALIVE, CHALLENGE, AUTH, DENY = range(1, 8)
DENY_UNKNOWN_DEVICE, DENY_BAD_AUTH, DENY_TUN_IP, DENY_POSTURE = range(1, 5)
STATE_UNKNOWN, STATE_OFF, STATE_ON = range(3)

DEV_ID = bytes(range(0x10, 0x20))
DEV_KEY = bytes(range(0x40, 0x60))
TUN_IP = socket.inet_aton("10.0.0.3")

# 포스처 조건이 걸린 기기
DEV2_ID = bytes(range(0x20, 0x30))
DEV2_KEY = bytes(range(0x60, 0x80))
TUN2_IP = socket.inet_aton("10.0.0.4")

failed = 0


def header(msg_type, session_idx=0):
    return struct.pack("!BBHI", VERSION, msg_type, 0, session_idx)


def posture(build=26200, firewall=STATE_ON, antivirus=STATE_ON):
    return struct.pack("!IBB2x", build, firewall, antivirus)


def mac(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()


class Client:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(1.0)
        self.addr = ("127.0.0.1", PORT)

    def send(self, pkt):
        self.sock.sendto(pkt, self.addr)

    def recv(self):
        """(type, session_idx, body) 또는 응답이 없으면 None."""
        try:
            pkt, _ = self.sock.recvfrom(2048)
        except socket.timeout:
            return None
        version, msg_type, _, idx = struct.unpack("!BBHI", pkt[:8])
        return msg_type, idx, pkt[8:]

    def request(self, dev_id=DEV_ID, tun_ip=TUN_IP):
        self.send(header(REQ_HANDSHAKE) + tun_ip + dev_id)
        return self.recv()

    def auth(self, nonce, key=DEV_KEY, dev_id=DEV_ID, tun_ip=TUN_IP, post=None, tamper=None):
        signed = tun_ip + dev_id + nonce + (post or posture())
        tag = mac(key, signed)
        if tamper:
            signed = tamper(signed)
        self.send(header(AUTH) + signed + tag)
        return self.recv()

    def handshake(self, **kw):
        r = self.request()
        assert r and r[0] == CHALLENGE, "no challenge: %r" % (r,)
        return self.auth(r[2], **kw)


def describe(r):
    if r is None:
        return "no response"
    msg_type, idx, body = r
    if msg_type == DENY:
        return "DENY(%d)" % body[0]
    if msg_type == RES_HANDSHAKE:
        return "RES_HANDSHAKE" if idx else "RES_HANDSHAKE(idx=0)"
    if msg_type == CHALLENGE:
        return "CHALLENGE"
    if msg_type == KEEPALIVE:
        return "KEEPALIVE"
    return "type %d" % msg_type


def check(name, got, want):
    global failed
    text = describe(got)
    ok = text == want
    print("%-4s %-52s %s%s" % ("ok" if ok else "FAIL", name, text, "" if ok else "   (want %s)" % want))
    failed += not ok


def handshake_tests():
    c = Client()

    check("unknown device id", c.request(dev_id=bytes(16)), "DENY(1)")
    check("request too short", (c.send(header(REQ_HANDSHAKE) + TUN_IP + DEV_ID[:8]), c.recv())[1], "no response")
    check("old client (tunnel ip only, no device id)", (c.send(header(REQ_HANDSHAKE) + TUN_IP), c.recv())[1], "no response")
    check("registered device gets a challenge", c.request(), "CHALLENGE")

    check("correct key", c.handshake(), "RES_HANDSHAKE")
    check("wrong key", c.handshake(key=bytes(32)), "DENY(2)")

    r = c.request()
    nonce = r[2]
    check("first AUTH with this nonce", c.auth(nonce), "RES_HANDSHAKE")
    check("same AUTH sent again (replay)", c.auth(nonce), "DENY(2)")

    check("AUTH with a nonce the server never issued", (c.request(), c.auth(os.urandom(16)))[1], "DENY(2)")

    r = c.request()
    time.sleep(6.5)
    check("AUTH after the nonce expired (6.5s)", c.auth(r[2]), "DENY(2)")

    flip = lambda s: s[:-8] + posture(firewall=STATE_OFF)
    check("posture changed after signing", c.handshake(tamper=flip), "DENY(2)")

    check("tunnel ip not assigned to this device (properly signed)",
          c.handshake(tun_ip=socket.inet_aton("10.0.0.9")), "DENY(3)")

    c.send(header(AUTH) + TUN_IP + DEV_ID)
    check("AUTH too short", c.recv(), "no response")


def strict(c, **post):
    """포스처 조건이 걸린 두 번째 기기로 핸드셰이크한다."""
    r = c.request(dev_id=DEV2_ID, tun_ip=TUN2_IP)
    assert r and r[0] == CHALLENGE, "no challenge: %r" % (r,)
    return c.auth(r[2], key=DEV2_KEY, dev_id=DEV2_ID, tun_ip=TUN2_IP, post=posture(**post))


def posture_tests():
    c = Client()

    # 두 번째 기기: firewall=on antivirus=on min-build=22000
    check("strict device, everything on", strict(c), "RES_HANDSHAKE")
    check("strict device, firewall off", strict(c, firewall=STATE_OFF), "DENY(4)")
    check("strict device, firewall unknown", strict(c, firewall=STATE_UNKNOWN), "DENY(4)")
    check("strict device, antivirus off", strict(c, antivirus=STATE_OFF), "DENY(4)")
    check("strict device, build below minimum", strict(c, build=19045), "DENY(4)")
    check("strict device, build exactly minimum", strict(c, build=22000), "RES_HANDSHAKE")
    check("strict device, build unknown (0)", strict(c, build=0), "DENY(4)")

    # 첫 번째 기기에는 조건이 없다. 정책은 기기마다 따로다.
    check("device without requirements, firewall off",
          c.handshake(post=posture(firewall=STATE_OFF, antivirus=STATE_OFF, build=0)), "RES_HANDSHAKE")


def loader_tests():
    """오타가 난 조건을 조용히 넘기지 않는지."""
    global failed
    fd, path = tempfile.mkstemp(prefix="devices.", suffix=".conf")
    with os.fdopen(fd, "w") as f:
        f.write("%s %s 10.0.0.3 firewal=on\n" % (DEV_ID.hex(), DEV_KEY.hex()))
    p = subprocess.run([SERVER, str(PORT), path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.unlink(path)
    ok = p.returncode != 0 and b"unknown requirement 'firewal=on'" in p.stdout
    print("%-4s %-52s %s" % ("ok" if ok else "FAIL", "misspelled requirement in the device file",
                             "refused to start" if ok else "exit=%d" % p.returncode))
    failed += not ok


def main():
    if os.geteuid() != 0:
        sys.exit("run as root: sudo python3 test/auth_check.py")

    fd, path = tempfile.mkstemp(prefix="devices.", suffix=".conf")
    with os.fdopen(fd, "w") as f:      # mkstemp 는 0600 으로 만든다
        f.write("%s %s 10.0.0.3\n" % (DEV_ID.hex(), DEV_KEY.hex()))
        f.write("%s %s 10.0.0.4 firewall=on antivirus=on min-build=22000  # strict\n"
                % (DEV2_ID.hex(), DEV2_KEY.hex()))

    loader_tests()

    log = tempfile.TemporaryFile()
    srv = subprocess.Popen(["stdbuf", "-oL", SERVER, str(PORT), path], stdout=log, stderr=subprocess.STDOUT)
    try:
        time.sleep(0.7)
        if srv.poll() is not None:
            log.seek(0)
            sys.exit("server failed to start:\n" + log.read().decode())
        handshake_tests()
        posture_tests()
    finally:
        srv.terminate()
        srv.wait()
        os.unlink(path)

    if "-v" in sys.argv:
        log.seek(0)
        print("\n--- server log\n" + log.read().decode())
    print("\n%s" % ("all passed" if not failed else "%d FAILED" % failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
