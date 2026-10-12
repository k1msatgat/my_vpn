#!/usr/bin/env python3
"""서버의 기기 인증을 프로토콜 수준에서 확인한다.

임시 기기 목록으로 vpn_server 를 띄우고, 클라이언트인 척 UDP 패킷을 직접 만들어
보내 응답을 본다. 서버의 tun 에 10.0.0.1/24 를 붙여 터널로 들어간 ping 에 커널이
답하게 하므로, 다른 vpn_server 가 돌고 있는 동안에는 쓸 수 없다.
TUN 을 만들고 주소를 붙여야 해서 root 로 돌린다.

    sudo python3 test/auth_check.py [port] [-v]      (-v: 서버 로그도 출력)

와이어 포맷은 lib/common/include/proto.h 의 주석과 같아야 한다.
"""

import hashlib
import hmac
import os
import signal
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


def disruption_tests():
    """기기 ID 만 아는 제3자가 진짜 기기의 핸드셰이크를 깨뜨릴 수 없는지."""
    global failed
    victim, attacker = Client(), Client()

    def report(name, ok, text):
        global failed
        print("%-4s %-52s %s" % ("ok" if ok else "FAIL", name, text))
        failed += not ok

    r = victim.request()
    again = attacker.request()
    report("REQ from elsewhere does not replace a live nonce", again[2] == r[2],
           "same nonce" if again[2] == r[2] else "nonce replaced")
    check("victim's AUTH after 20 foreign REQs",
          ([attacker.request() for _ in range(20)], victim.auth(r[2]))[1], "RES_HANDSHAKE")

    r = victim.request()
    check("garbage AUTH from elsewhere", attacker.auth(r[2], key=bytes(32)), "DENY(2)")
    check("victim's AUTH after the garbage AUTH", victim.auth(r[2]), "RES_HANDSHAKE")

    # 재사용은 서명이 맞는 AUTH 가 올 때까지만이다. 한 번 쓰인 nonce 는 다시 나오지 않는다.
    after = victim.request()
    report("a used nonce is not handed out again", after[2] != r[2],
           "new nonce" if after[2] != r[2] else "same nonce")

    # 서명은 맞지만 거절된 AUTH 도 nonce 를 쓴 것이다 (엿들은 AUTH 의 재전송 방지).
    wrong_ip = socket.inet_aton("10.0.0.9")
    check("properly signed AUTH for a wrong tunnel ip", victim.auth(after[2], tun_ip=wrong_ip), "DENY(3)")
    check("the same nonce, now with the right tunnel ip", victim.auth(after[2]), "DENY(2)")


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


def keepalive(idx, counter, key=DEV_KEY, post=None, tamper=None):
    """KEEPALIVE: header | counter(8) | posture(8) | mac(32). mac 은 헤더부터 포스처까지."""
    signed = header(KEEPALIVE, idx) + struct.pack("!Q", counter) + (post or posture())
    tag = mac(key, signed)
    if tamper:
        signed = tamper(signed)
    return signed + tag


def session(c, strict_dev=False):
    r = strict(c) if strict_dev else c.handshake()
    assert r and r[0] == RES_HANDSHAKE, "handshake failed: %s" % describe(r)
    return r[1]


def keepalive_tests():
    c = Client()
    ka = lambda pkt: (c.send(pkt), c.recv())[1]

    idx = session(c)
    check("keepalive, counter 0", ka(keepalive(idx, 0)), "KEEPALIVE")
    check("keepalive, counter 5", ka(keepalive(idx, 5)), "KEEPALIVE")
    good = keepalive(idx, 6)
    check("keepalive, counter 6", ka(good), "KEEPALIVE")
    check("the same packet again (replay)", ka(good), "no response")
    check("older counter (3), properly signed", ka(keepalive(idx, 3)), "no response")
    check("old-style keepalive (header only)", ka(header(KEEPALIVE, idx)), "no response")
    check("signed with the wrong key", ka(keepalive(idx, 7, key=bytes(32))), "no response")
    check("signed with another device's key", ka(keepalive(idx, 7, key=DEV2_KEY)), "no response")
    flip = lambda s: s[:-8] + posture(build=99999)
    check("posture changed after signing", ka(keepalive(idx, 7, tamper=flip)), "no response")
    check("session survives the forged packets", ka(keepalive(idx, 7)), "KEEPALIVE")

    # 다른 세션의 keepalive 를 끼워 넣기: mac 이 헤더의 idx 까지 덮으므로 통하지 않는다.
    other = keepalive(idx, 8)
    swapped = header(KEEPALIVE, idx ^ 0x100) + other[8:]
    check("keepalive replayed under another session idx", ka(swapped), "no response")

    # 다시 인증하면 새 세션이다. idx 가 바뀌고 counter 는 0 부터 다시 받는다.
    idx2 = session(c)
    global failed
    ok = idx2 != idx
    print("%-4s %-52s %s" % ("ok" if ok else "FAIL", "re-authentication opens a new session",
                             "idx %08x -> %08x" % (idx, idx2)))
    failed += not ok
    check("new session accepts counter 0", ka(keepalive(idx2, 0)), "KEEPALIVE")
    check("old session idx no longer answers", ka(keepalive(idx, 9)), "no response")

    # 세션 중에 포스처가 나빠진 경우 (포스처 조건이 걸린 기기)
    sidx = session(c, strict_dev=True)
    sk = lambda n, **post: ka(keepalive(sidx, n, key=DEV2_KEY, post=posture(**post)))
    check("strict device, keepalive with everything on", sk(0), "KEEPALIVE")
    check("strict device, keepalive reports firewall off", sk(1, firewall=STATE_OFF), "DENY(4)")
    check("that session is gone", sk(2), "no response")
    sidx = session(c, strict_dev=True)
    check("strict device can come back once the posture is fine", sk(0), "KEEPALIVE")


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return ~s & 0xffff


def echo_request(src, dst, seq):
    """IPv4 + ICMP echo request 한 개."""
    icmp = struct.pack("!BBHHH", 8, 0, 0, 0x4d56, seq) + b"my_vpn auth_check"
    icmp = icmp[:2] + struct.pack("!H", checksum(icmp)) + icmp[4:]
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(icmp), seq, 0, 64, 1, 0, src, dst)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return ip + icmp


def data(idx, counter, inner):
    return header(DATA, idx) + struct.pack("!Q", counter) + inner


def describe_data(r):
    """DATA 로 돌아온 inner 패킷이 ICMP echo reply 인지."""
    if r is None:
        return "no response"
    msg_type, idx, body = r
    if msg_type != DATA:
        return describe(r)
    inner = body[8:]
    if len(inner) >= 28 and inner[9] == 1 and inner[20] == 0:
        return "echo reply"
    return "DATA (not an echo reply)"


def check_data(name, got, want):
    global failed
    text = describe_data(got)
    ok = text == want
    print("%-4s %-52s %s%s" % ("ok" if ok else "FAIL", name, text, "" if ok else "   (want %s)" % want))
    failed += not ok


def data_tests():
    """인증된 세션으로만 데이터가 통하는지. 서버의 tun 에 10.0.0.1 을 붙여 커널이 ping 에 답하게 한다."""
    c = Client()
    send = lambda pkt: (c.send(pkt), c.recv())[1]
    server_ip = socket.inet_aton("10.0.0.1")

    idx = session(c)
    check_data("ping through an authenticated session",
               send(data(idx, 0, echo_request(TUN_IP, server_ip, 1))), "echo reply")
    check_data("same ping under a session idx nobody was given",
               send(data(idx ^ 0x100, 1, echo_request(TUN_IP, server_ip, 2))), "no response")
    check_data("inner source is another device's tunnel ip",
               send(data(idx, 2, echo_request(TUN2_IP, server_ip, 3))), "no response")

    # 세션 중에 포스처 때문에 거절되면 데이터 길도 같이 닫힌다.
    sidx = session(c, strict_dev=True)
    check_data("strict device, ping while the posture is fine",
               send(data(sidx, 0, echo_request(TUN2_IP, server_ip, 4))), "echo reply")
    check("strict device, keepalive reports firewall off",
          send(keepalive(sidx, 1, key=DEV2_KEY, post=posture(firewall=STATE_OFF))), "DENY(4)")
    check_data("strict device, ping after being denied",
               send(data(sidx, 2, echo_request(TUN2_IP, server_ip, 5))), "no response")


def setup_tun(log):
    """서버가 만든 tun 에 터널 주소를 붙인다. test/server.sh 의 setup_if 와 같은 일이다."""
    log.seek(0)
    for line in log.read().decode().splitlines():
        if line.startswith("tun device: "):
            dev = line.split()[2]
            subprocess.run(["ip", "addr", "add", "10.0.0.1/24", "dev", dev], check=True)
            subprocess.run(["ip", "link", "set", dev, "mtu", "1400", "up"], check=True)
            return dev
    sys.exit("tun device name not found in the server log")


LINE1 = "%s %s 10.0.0.3" % (DEV_ID.hex(), DEV_KEY.hex())
LINE2 = "%s %s 10.0.0.4 firewall=on antivirus=on min-build=22000  # strict" % (DEV2_ID.hex(), DEV2_KEY.hex())


def write_devices(path, *lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def reload_tests(srv, path):
    """서버를 끄지 않고 기기 목록을 바꾼다 (kill -HUP)."""
    c = Client()
    ka = lambda pkt: (c.send(pkt), c.recv())[1]

    def reload(*lines):
        write_devices(path, *lines)
        srv.send_signal(signal.SIGHUP)
        time.sleep(1.5)

    idx = session(c)
    check("before reload", ka(keepalive(idx, 0)), "KEEPALIVE")
    reload(LINE1 + " min-build=99999", LINE2)
    check("policy tightened: next keepalive of the open session", ka(keepalive(idx, 1)), "DENY(4)")
    check("policy tightened: new handshake", c.handshake(), "DENY(4)")

    reload(LINE1, LINE2)
    idx = session(c)
    check("policy relaxed: device is accepted again", ka(keepalive(idx, 0)), "KEEPALIVE")

    reload(LINE2)
    check("device removed: next keepalive of the open session", ka(keepalive(idx, 1)), "DENY(1)")
    check("device removed: new handshake", c.request(), "DENY(1)")

    reload(LINE1, LINE2)
    idx = session(c)
    reload(LINE1 + " firewal=on", LINE2)
    check("broken file: previous list stays in effect", ka(keepalive(idx, 0)), "KEEPALIVE")


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
        f.write(LINE1 + "\n" + LINE2 + "\n")

    loader_tests()

    log = tempfile.TemporaryFile()
    srv = subprocess.Popen(["stdbuf", "-oL", SERVER, str(PORT), path], stdout=log, stderr=subprocess.STDOUT)
    try:
        time.sleep(0.7)
        if srv.poll() is not None:
            log.seek(0)
            sys.exit("server failed to start:\n" + log.read().decode())
        setup_tun(log)
        handshake_tests()
        disruption_tests()
        posture_tests()
        keepalive_tests()
        data_tests()
        reload_tests(srv, path)
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
