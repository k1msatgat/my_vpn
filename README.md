# my_vpn

[![ci](https://github.com/k1msatgat/my_vpn/actions/workflows/ci.yml/badge.svg)](https://github.com/k1msatgat/my_vpn/actions/workflows/ci.yml)

Linux에서 C로 만든 L3 VPN이다. TUN으로 IP 패킷을 받아 UDP로 터널링하고, 그 위에 ZTNA식 접근 제어를 얹었다. 등록된 기기만 세션을 받고, 세션 중에도 10초마다 단말 상태(포스처)를 다시 확인하고, ACL에 적힌 목적지로만 패킷을 넘긴다.

와이어 포맷, 핸드셰이크, 세션 관리, 접근 제어는 직접 설계했다. 암호 알고리즘은 OpenSSL을 쓴다(ECDH P-256, HKDF-SHA256, AES-256-GCM). 학습용이고 암호 프로토콜은 외부 검토를 받지 않았다.

```
 Linux 클라이언트                                  Linux 서버
 ┌──────────────────────┐   UDP 9000, AES-GCM   ┌──────────────────────────────────┐
 │ app → myvpn0 (tun)   │ ════════════════════▶ │ epoll 루프 (스레드 하나)         │
 │       10.0.0.2       │                       │  세션 조회 → tag 확인 → 재전송 창 │
 │ vpn_client           │ ◀════════════════════ │  → 출발지 검사 → ACL → tun0      │
 │  포스처: 커널, nft    │                       │ devices.conf / acl.conf           │
 └──────────────────────┘                       └────────────────┬─────────────────┘
                                                                  │ 커널 전달 + NAT
                                                                  ▼ (ACL로 연 곳만)
                                                           서버 뒤의 네트워크
```

같은 프로토콜을 쓰는 Windows 클라이언트(`windows/`, Wintun + MFC)도 있다. 지금 이 저장소의 기본 경로는 Linux 서버와 Linux 클라이언트다.

## 돌려 보기

한 장비에서 서버와 클라이언트를 네트워크 네임스페이스 두 개에 띄워 전체 흐름을 돌린다. root가 아니면 `unshare -rn` 안에서 돈다.

```
$ make && test/linux_e2e.sh          # 출력 일부
== 1. enrollment
ok   enroll: client reports success
ok   enroll: identity file is mode 600
ok   enroll: the code cannot be used twice
== 2. connect
ok   connect: tun has the address the server assigned
ok   connect: route from the ACL allow rule (198.51.100.0/24)
ok   connect: ping 10.0.0.1 through the tunnel
== 3. acl
ok   acl: udp to 10.0.0.1:22 is dropped by the server
== 4. posture
ok   posture: policy tightened -> denied at the next keepalive
     (denied after 10s)
ok   posture: firewall on -> client re-handshakes by itself
     (reconnected after 1s)
all passed
```

4번은 기기 정책에 `firewall=on`을 걸고 `SIGHUP`을 보낸 뒤, 클라이언트 쪽 네임스페이스에 nft로 인바운드 기본 거부를 넣는 순서다. 그때 클라이언트 로그는 이렇다.

```
[myvpn0] connected
[deny] posture does not meet policy
[posture] changed: kernel=331264 firewall=on antivirus=unknown
[HS] retry after deny
[HS] established idx=029b8c00
[myvpn0] connected
```

컨테이너로는 `docker/demo.sh`가 같은 시나리오를 서버와 클라이언트 두 컨테이너로 돌린다(`CAP_NET_ADMIN`과 `/dev/net/tun`만 준다). 둘 다 CI에서 푸시마다 돈다.

### 장비 두 대

서버 (Rocky Linux 9 기준):

```bash
sudo dnf install -y gcc make openssl-devel nftables
make server
sudo firewall-cmd --permanent --add-port=9000/udp && sudo firewall-cmd --reload
# 클라우드라면 콘솔의 보안 그룹에서도 UDP 9000 을 연다

touch devices.conf enroll.conf && chmod 600 devices.conf enroll.conf
cat > acl.conf <<'EOF'
10.0.0.0/24  10.0.0.1  icmp  any  allow
any          any       any   any  deny
EOF
chmod 600 acl.conf
sudo ./server/bin/vpn_server -e enroll.conf -a audit.log 9000 devices.conf acl.conf
# 다른 터미널에서. 서버가 찍은 "tun device: tun0" 의 이름을 쓴다
sudo ip addr add 10.0.0.1/24 dev tun0 && sudo ip link set tun0 mtu 1400 up

# 등록 코드 (한 번만 쓸 수 있고 15분 뒤 만료). 서버에 SIGHUP 도 보낸다
sudo env ENROLL_FILE=./enroll.conf docker/enroll-code.sh 10.0.0.2 -l laptop
```

클라이언트 (Ubuntu, WSL2 포함):

```bash
sudo apt install -y build-essential libssl-dev nftables
make client
sudo ./client/bin/vpn_client enroll <서버 IP> 9000 <등록 코드>   # 신원은 /var/lib/my_vpn/device (0600)
sudo ./client/bin/vpn_client run <서버 IP> 9000                  # 주소, MTU, 경로는 서버가 준다
ping 10.0.0.1
```

`vpn_client posture`로 서버에 보고할 값을 미리 볼 수 있다. 이 두 장비 절차는 아직 실제 장비에서 처음부터 끝까지 따라 해 보지 않았다. WSL2처럼 nft 대신 iptables-legacy를 쓰는 환경에서는 방화벽 항목이 "모름"으로 나올 수 있으니, 그런 기기에 `firewall=on` 정책을 걸기 전에 확인해야 한다.

서버를 systemd 서비스로 설치하고 AI 에이전트(MCP)로 기기, ACL, 등록 코드, 감사 로그를 다루려면 `mcp/install.sh`를 쓴다. 서버 뒤의 네트워크로 내보내려면 `/etc/my_vpn/server.env`에 `FORWARD_OUT=eth0`을 적는다. 서비스가 뜰 때 IP 전달, NAT, "안에서 시작한 연결의 답만 들인다"는 nft 규칙을 넣고 내려갈 때 뺀다.

### 설정 파일

```
# devices.conf — 등록 코드로 온 기기는 서버가 직접 적는다
# id (32 hex)                      key (64 hex)  tunnel-ip  [포스처 조건]
2bd5d6e0610d2b7c07f884d0c5afb043   a24d4c3d…     10.0.0.2   firewall=on min-build=331264

# acl.conf — 위에서부터 첫 일치, 일치가 없으면 거부
# src          dst              proto  dport  action  [포스처 조건]
10.0.0.0/24    10.101.0.0/16    tcp    443    allow
10.0.0.0/24    10.101.0.7       tcp    22     allow   firewall=on
any            any              any    any    deny
```

포스처 조건은 `firewall=on`, `antivirus=on`, `min-build=N`이다. Linux 클라이언트는 `min-build` 자리에 커널 버전(`major<<16 | minor<<8 | patch`, 5.14.0 → 331264)을, `firewall` 자리에 nftables 상태(input 훅에 `policy drop` 체인이 있거나 firewalld가 돌면 on)를 보고한다. 백신 항목은 Linux에 대응하는 게 없어 보고하지 않는다. 못 읽은 값은 통과로 치지 않는다. 두 파일 모두 `kill -HUP`으로 세션을 끊지 않고 다시 읽는다.

## 설계에서 고민한 것

**열리지 않은 패킷은 아무것도 바꾸지 못한다.** 서버는 세션 패킷을 받으면 세션 번호로 세션을 찾고, 세션 키로 tag를 확인한 뒤에야 재전송 창을 갱신하고, NAT 뒤에서 바뀐 클라이언트의 외부 주소를 반영하고, 세션을 내린다. 순서가 반대면 세션 번호(평문 4바이트)만 아는 사람이 위조 패킷 한 통으로 남의 세션을 끊거나 응답을 자기 쪽으로 돌릴 수 있다. 핸드셰이크도 같다. 서명이 틀린 `AUTH`는 진행 중인 nonce를 버리지 않고, 살아 있는 nonce가 있으면 다시 온 `REQ`에 같은 nonce를 준다. (`server/src/server.c`)

**세션 번호에 테이블 자리를 넣었다.** 32비트 세션 번호의 하위 8비트가 피어 테이블의 인덱스, 상위 24비트가 `getrandom` 난수다. 패킷마다 하는 조회가 배열 접근 한 번이고, 같은 자리를 다시 써도 이전 세션의 번호는 난수 부분이 달라 무효가 된다. 대신 세션 수는 256개를 넘을 수 없다(지금은 `MAX_PEERS` 8, `_Static_assert`로 묶어 둠). (`server/src/peer.c`)

**서버 인증서 없이 서버를 확인한다.** 세션 키는 `HKDF(salt = 기기 키, ikm = 일회용 ECDH의 합의 값)`에서 나온다. 기기 키를 모르는 쪽은 사이에 끼어 자기 공개키를 내밀어도 같은 키에 닿지 못하므로, `RES_HANDSHAKE`의 tag가 맞았다는 것이 곧 서버가 진짜라는 증명이다. 일회용 ECDH 덕분에 기기 키가 나중에 새도 지난 트래픽은 풀리지 않는다. 대가는 서버가 모든 기기의 대칭 키를 들고 있다는 것이다. 서버가 뚫리면 모든 기기로 가장할 수 있다. 공개키 방식이면 서버엔 공개키만 있으면 된다. (`lib/common/src/secure.c`)

**데이터는 순서가 바뀌어도 받고, 포스처 보고는 순서대로만 받는다.** 세션 패킷의 64비트 counter가 GCM nonce이자 순번이다. 받은 최댓값 아래 64개를 비트맵으로 기억해 UDP가 순서를 섞어도 받고 같은 값은 버린다. 그런데 포스처 보고는 "지금 상태"라서, 방화벽이 켜져 있을 때의 보고를 붙잡아 뒀다가 끈 뒤에 풀어 놓는 공격을 막으려면 직전보다 큰 counter만 받아야 한다. 같은 counter에 규칙을 두 개 둔 셈이다.

**정책은 서버에만 있다.** 클라이언트에는 서버 주소만 적는다. 터널 주소, MTU, 어디를 터널로 보낼지는 서버가 수락과 keepalive 응답에 실어 보내고, 경로 목록은 ACL의 `allow` 규칙에서 뽑는다. 경로를 따로 관리하면 ACL과 어긋난다. 경로는 있는데 규칙이 없어 버려지거나, 규칙은 있는데 경로가 없어 터널로 가지도 않는다. 클라이언트는 받은 경로라도 `/0`과 서버 주소를 덮는 대역은 넣지 않는다. (`server/src/alc.c`, `client/src/netcfg.c`)

**등록에 새 핸드셰이크를 만들지 않았다.** 관리자가 만든 일회용 코드에서 HMAC으로 임시 ID와 키를 뽑고, 기기는 그걸로 평소의 핸드셰이크를 돈 뒤 그 세션 안에서 자기 진짜 키를 보낸다. 코드를 모르는 쪽은 세션을 못 열고, 키는 세션 키로 감싸여 가며, 새로 생긴 메시지는 `ENROLL` 하나다. 클라이언트는 서버가 받아 준 뒤에야 신원을 저장한다.

**서버는 스레드 하나다.** epoll 하나에 UDP 소켓, tun, 상태 소켓을 건다. 세션 표와 정책에 잠금이 없다. `SIGHUP` 핸들러는 플래그만 세우고, 루프가 1초 타임아웃으로 깨어나 새 파일을 임시 표에 읽어 검사가 끝나야 바꿔 끼운다. 그 대가가 아래 성능 숫자다.

## 프로토콜

UDP 페이로드는 8바이트 헤더로 시작한다. 세션이 열린 뒤엔 종류와 관계없이 모양이 같다.

```
 | version | type | reserved(2) | session_idx(4) | counter(8) | AES-256-GCM 암호문 | tag(16) |
 └─────────── 암호화하지 않지만 tag가 덮는다 ───────────────┘
```

GCM nonce는 `0x00000000 | counter`이고, 방향마다 키가 달라 양쪽이 counter를 0부터 세도 (키, nonce)가 겹치지 않는다. 바깥 IP 20 + UDP 8 + 헤더 16 + tag 16 = 60바이트가 붙어서 터널 MTU를 1400으로 둔다.

핸드셰이크는 네 메시지다.

```
C → S  REQ_HANDSHAKE  기기 ID
S → C  CHALLENGE      nonce (1회용, 5초)
C → S  AUTH           기기 ID | nonce | 일회용 공개키 | 포스처 | HMAC(기기 키, 앞의 전부)
S → C  RES_HANDSHAKE  세션 번호 | 서버 공개키 | 터널 설정(암호화) | tag      또는 DENY(사유)
```

서버는 기기 → nonce/mac → 포스처 순으로 본다. mac을 확인하기 전엔 거절 사유를 뭉뚱그리고 ECDH도 하지 않는다. 메시지별 바이트 배치는 `lib/common/include/proto.h` 주석에 있다.

keepalive는 10초마다 포스처를 다시 싣고(포스처는 2초마다 읽어 바뀌면 바로 보낸다), 서버는 받을 때마다 정책을 다시 평가한다. 클라이언트는 서버에서 30초 동안 아무것도 못 받으면 다시 핸드셰이크하고, 서버는 60초 조용한 세션을 지운다. 세션 중 거절당한 클라이언트는 tun을 닫지 않는다. 닫으면 사용자가 방화벽을 다시 켜도 스스로 돌아올 수 없다.

## 표준 프로토콜과 비교

| 이것 | 대응하는 표준 | 같은 것 | 단순화했고, 그래서 약한 곳 |
|---|---|---|---|
| 핸드셰이크 | IKEv2 (RFC 7296) | 2왕복, 일회용 DH로 전방 비밀성, 비싼 계산은 상대 확인 뒤 | 알고리즘 협상이 없다. 신원(기기 ID)과 포스처를 암호화하지 않아 수동 관찰자가 기기를 추적할 수 있다. 응답자 쿠키가 없어 출발지를 위조한 `REQ`(24B)로 반사를 시킬 수 있다. 모르는 ID면 `DENY` 41B로 약 1.7배 |
| 데이터 패킷 | ESP + AES-GCM (RFC 4303, 4106) | SPI ↔ 세션 번호, 순번이 곧 재전송 방지, AEAD 하나로 암호화와 무결성 | 순번 64비트를 그대로 싣는다(ESN 추정이 필요 없는 대신 4바이트 더). IV를 따로 싣지 않는 건 RFC 8750의 암묵적 IV와 같은 생각이다. 패딩이 없어 길이가 그대로 보인다 |
| 재전송 창 | ESP anti-replay (RFC 4303 §3.4.3) | 최댓값 + 비트맵, 무결성 확인 뒤에만 갱신. 창 64는 RFC가 권하는 기본값과 같다 | 고속 링크에서 64개 넘게 뒤섞이면 정상 패킷도 버린다 |
| 키 합의 | WireGuard (Noise IK) | 고정 암호 묶음, 일회용 DH, counter = nonce | WireGuard는 1왕복이고, 개시자 공개키를 암호화하고, 2분마다 키를 바꾸고, 인증 안 된 패킷엔 답하지 않는다. 여기는 2왕복(서버가 nonce를 준다), 재키잉 없음, `DENY`로 답하니 스캔에 서버가 드러난다. 서버가 피어 공개키가 아니라 대칭 키를 쥔다 |

이 표는 명세와 설계를 비교한 것이다. 같은 서버에 WireGuard를 올려 핸드셰이크를 나란히 캡처하는 실험(실험 8)으로 보강할 예정이다.

## 테스트와 성능

| 무엇 | 어떻게 |
|---|---|
| 전체 경로 | `test/linux_e2e.sh` — netns 두 개, 18개 항목 (CI) |
| 컨테이너 | `docker/demo.sh` — 같은 시나리오를 compose로 (CI) |
| 서버 프로토콜 | `test/auth_check.py` — 클라이언트인 척 패킷을 직접 만들어 176개 경우를 본다. 틀린 키, nonce 재사용, 서명 뒤 변조, 곡선 밖의 공개키, 세션 패킷 변조·재전송·순서 뒤섞기, 정책 다시 읽기, 등록 코드 재사용. 세션 키 유도와 패킷 형식을 서버 코드를 거치지 않고 Python으로 따로 구현했다 (CI) |
| 암호 도구 | 서버, Linux 클라이언트, Windows 클라이언트의 `selftest`가 같은 표준 벡터 20개(RFC 4231, 5869, 5903 등) (CI) |

서버 혼자의 처리량은 `tools/loadgen`으로 쟀다. TUN 없이 UDP 소켓으로 핸드셰이크한 뒤 터널 안에 ICMP echo를 실어 서버 tun 주소로 최대 속도로 보낸다. 커널이 답하고 서버가 돌려주므로 서버의 두 방향을 모두 지난 왕복이다.

| 페이로드 | 왕복 pps | Mbps | 서버 CPU |
|---:|---:|---:|---:|
| 56 B | 44,000 | 30 | 90% |
| 512 B | 43,000–47,000 | 186–203 | 91% |
| 1372 B | 42,000 | 475 | 90% |

GitHub Actions 러너(AMD EPYC 9V74, 4 vCPU 공용 VM), 루프백, 세션 하나, 서버를 CPU 0에 고정했다. 크기와 상관없이 pps가 같으니 한계는 바이트가 아니라 패킷당 비용이다. 왕복마다 시스템 콜 네 번(`recvfrom`, `write`, `read`, `sendto`)과 커널의 라우팅, netfilter, skb 할당을 지나고, perf 상위도 거의 커널 함수였다. 패킷마다 찍는 `printf`가 병목일 거라 예상했는데 로그를 `/dev/null`로 보내도 숫자가 같았다. 해석은 [docs/experiments.md](docs/experiments.md)의 0번에 있다. 줄이려면 `recvmmsg`/`sendmmsg`로 시스템 콜을 묶거나, `SO_REUSEPORT`와 멀티 큐 TUN으로 코어를 나누거나, 아예 커널 안(XDP 등)으로 옮겨야 한다.

## 한계

- 암호 프로토콜은 아무도 검토하지 않았다. 테스트가 보여 주는 건 의도대로 동작한다는 것까지다.
- ACL은 상태를 추적하지 않는다. 클라이언트가 보낸 방향에만 걸리고, 돌아오는 방향은 서버의 nft 전달 규칙(conntrack)에 맡긴다.
- 서버가 기기 키를 평문 파일(600)로 갖고 있다. 세션 키를 도중에 바꾸지 않는다.
- 포스처는 클라이언트의 자기 보고다. 키를 가진 변조된 클라이언트는 거짓말을 할 수 있다(막으려면 TPM 원격 증명이 필요하다).
- 기기만 인증하고 그 기기를 쓰는 사람은 보지 않는다.
- 재핸드셰이크는 블로킹이라 최대 5초 동안 터널 패킷이 멈춘다. 종료 통지가 없어 서버는 60초 뒤에야 세션을 지운다.
- IPv4만 다룬다. 동시 세션은 8개다.

다음에 한다면: mTLS/JWT로 IdP와 연동해 "누가"를 ACL에 넣는 것, XDP로 헤더가 깨졌거나 모르는 세션 번호인 패킷을 커널 앞단에서 버리는 빠른 경로.

## 구조

```
lib/common/     프로토콜(proto), 세션 키와 패킷 암호(secure), OpenSSL 구현 — 서버와 두 클라이언트가 같이 빌드
server/src/     server.c 루프와 데이터 경로, peer.c 세션 표, alc.c ACL, device.c / enroll.c 기기와 등록,
                policy.c 포스처 판정, audit.c 감사 로그, status.c 상태 소켓
client/src/     client.c 루프, session.c 핸드셰이크와 등록(loadgen과 공유), identity.c 신원 파일,
                posture.c 커널 / nft, netcfg.c 주소·MTU·경로 적용(ioctl)
tools/loadgen/  부하 생성기와 측정 스크립트
test/           linux_e2e.sh, auth_check.py, 두 장비용 server.sh / client.sh
docker/         entrypoint, 등록 코드 스크립트, compose 데모
mcp/            systemd 서비스, root 헬퍼(my_vpn_ctl), MCP 서버
windows/        Windows 클라이언트 (Visual Studio 솔루션, Wintun은 wintun.net에서 받아 third_party/에 푼다)
```

## AI 사용 내역

처음의 터널, 와이어 헤더, 핸드셰이크, 피어 테이블, 위조 방어, keepalive와 재핸드셰이크, ACL 매칭은 직접 짰고 AI(Claude Code)에게 리뷰를 받았다. 그 뒤의 확장은 방향과 범위를 내가 정하고 구현은 AI가 했다.

- 기기 인증, 포스처, 세션 암호화, 서버가 내려 주는 설정, 등록 코드, 감사 로그, 상태 소켓 (서버와 `lib/common`)
- Linux 클라이언트 v4 — 초기 클라이언트는 epoll 루프의 골격만 남고 다시 쓰였다
- Windows 클라이언트 전체, MCP 서버, Docker, 부하 생성기, CI, `test/`의 스크립트
- 이 README와 `docs/experiments.md`의 해석 초안

AI가 구현한 부분은 직접 돌려 보고 패킷을 캡처, 재전송, 변조해 보는 실험으로 검증한다. 기록은 [docs/experiments.md](docs/experiments.md)에 있고, 지금은 0번(성능)만 채웠다. 자동 시험(`auth_check.py`, `linux_e2e.sh`, `demo.sh`)은 CI에서 매번 돈다.
