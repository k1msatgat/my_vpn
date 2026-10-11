# Linux (서버 + 클라이언트)

와이어 프로토콜과 설계는 [루트 README](../README.md)에 있습니다. 이 문서는 Linux 쪽 빌드·실행·테스트와 ACL만 다룹니다.

---

## Build

```bash
make            # lib + client + server
make clean      # 오브젝트/산출물 삭제
make distclean  # bin/ 디렉터리 전체 삭제
```

- 요구사항: Linux, GCC, `/dev/net/tun`, OpenSSL 개발 헤더 (`openssl-devel` / `libssl-dev`) — 서버가 HMAC-SHA256에 씁니다
- `-Wall -Wextra` 경고 없이 빌드됩니다.

| 산출물 | 경로 |
|---|---|
| 공용 라이브러리 | `lib/common/bin/libcommon.a` |
| 클라이언트 | `client/bin/vpn_client` |
| 서버 | `server/bin/vpn_server` |

---

## Usage

TUN 디바이스 생성에 root 권한(`CAP_NET_ADMIN`)이 필요합니다.

### Server

```bash
sudo ./server/bin/vpn_server <port> <device-file> [acl-file]

# 다른 터미널에서 인터페이스 설정 (생성된 이름은 로그의 "tun device:" 확인)
sudo ip addr add 10.0.0.1/24 dev tun0
sudo ip link set tun0 mtu 1400 up

./server/bin/vpn_server selftest      # HMAC-SHA256 이 RFC 4231 벡터와 맞는지 (root 불필요)
sudo kill -HUP $(pidof vpn_server)    # 기기 목록을 다시 읽는다 (세션은 유지)
```

- `<device-file>`은 필수입니다. 등록된 기기만 세션을 받으므로, 목록이 없으면 누구도 받아 줄 수 없습니다.
  아래 [기기 등록과 포스처 정책](#기기-등록과-포스처-정책)을 보세요.

### Client

```bash
sudo ./client/bin/vpn_client <ifname> <server-ip> <port> <tunnel-ip>
# 예) sudo ./client/bin/vpn_client tun0 203.0.113.10 9000 10.0.0.2

sudo ip addr add 10.0.0.2/24 dev tun0
sudo ip link set tun0 mtu 1400 up
```

> **Linux 클라이언트는 아직 기기 인증을 구현하지 않았습니다.** 예전 형식의 핸드셰이크를 보내므로
> 지금 서버에는 붙지 못합니다(`[HS] short request`로 버려집니다). 아래 내용과 `test/client.sh`의
> 시나리오는 인증을 구현한 뒤에 다시 유효해집니다.

- `<tunnel-ip>`는 인터페이스에 할당한 주소와 같아야 합니다. 서버는 이 주소로 응답 패킷의 목적지 피어를 찾습니다.
- MTU 1400은 외부 IP(20) + UDP(8) + 터널 헤더(16)를 고려한 값입니다.

### Test

```bash
ping 10.0.0.1     # client → server
```

```
[HS] request 1/5
[HS] established idx=5f3a9c02
[tun0][tun->udp] 84 bytes, proto=1, icmp type=8
[tun0][udp->tun] 84 bytes, proto=1, icmp type=0
```

### Test scripts

`test/`의 스크립트는 빌드, 실행, 인터페이스 설정(IP/MTU)을 한 번에 하고 시나리오별로 `PASS` / `FAIL`을 출력합니다.
서버 장비에서 먼저 실행한 뒤 클라이언트 장비에서 실행합니다.

| 시나리오 | 서버 | 클라이언트 | 확인하는 것 |
|---|---|---|---|
| 기본 연결 | `test/server.sh` | `test/client.sh` | 핸드셰이크와 ping 왕복 |
| 유휴 120초 | `test/server.sh` | `test/client.sh idle` | keepalive로 세션이 유지되고 재핸드셰이크가 발동하지 않음 |
| 클라이언트 정지 70초 | `test/server.sh` | `test/client.sh stop` | 서버의 세션 만료, 클라이언트의 재핸드셰이크 |
| 서버 재시작 | `test/server.sh restart` | `test/client.sh restart` | 서버 `kill -9` 후 재시작 시 새 세션으로 복구 |

```bash
SERVER_IP=203.0.113.10 test/client.sh idle    # 설정은 환경변수로 변경 (기본값은 test/common.sh)
```
- `test/server.sh`는 기기 목록으로 `test/devices.conf`를 씁니다 (`DEVICE_FILE`로 변경). 없으면 무엇을 해야 하는지 알려 주고 멈춥니다.
- 로그는 `test/logs/`에도 저장됩니다.
- 서버 장비에 클라이언트 터널 IP가 로컬 주소로 남아 있으면(이전 테스트의 persistent tun 등) 커널이 터널로 들어온 패킷을 버리므로, 서버 스크립트가 시작 전에 이를 검사합니다.

### 인증 검사 (`test/auth_check.py`)

서버의 기기 인증을 클라이언트 없이 확인합니다. 임시 기기 목록으로 서버를 띄우고, 클라이언트인 척
UDP 패킷을 직접 만들어 보내 응답을 봅니다. 서명은 Python의 `hmac`으로 하므로 서버(OpenSSL)와는
다른 구현끼리 맞춰 보는 셈입니다.

```bash
sudo python3 test/auth_check.py        # -v 를 붙이면 서버 로그도 출력
```

| 묶음 | 확인하는 것 |
|---|---|
| 핸드셰이크 | 모르는 기기, 틀린 키, `AUTH` 재전송, 만료된 nonce, 서명 뒤 포스처 변조, 배정되지 않은 터널 IP |
| 포스처 | 방화벽 꺼짐 / 모름, 백신 꺼짐, 빌드 미달 / 경계값 / 모름, 조건이 없는 기기 |
| keepalive | 재전송, 예전 counter, 틀린 키, 다른 기기의 키, 위조 패킷 뒤에도 세션 유지, 재인증 시 새 세션, 세션 중 거절 |
| 데이터 | 인증된 세션으로 ping 왕복, 모르는 idx, inner src 위조, 거절된 뒤의 ping |
| 다시 읽기 | 정책 강화 / 완화, 기기 삭제, 깨진 파일일 때 이전 목록 유지 |

서버의 tun에 `10.0.0.1/24`를 붙이므로 다른 `vpn_server`가 도는 동안에는 쓸 수 없습니다.

---

## 기기 등록과 포스처 정책

서버는 기기 목록 파일에 있는 기기만 받습니다. 한 줄에 기기 하나입니다.

```
# id (32 hex)                      key (64 hex)   tunnel-ip   requirements
2bd5d6e0610d2b7c07f884d0c5afb043   a24d4c3d...    10.0.0.3    firewall=on antivirus=on min-build=22000
9c1e77a0b2d34f5e8a6b0c1d2e3f4a5b   77e0c1b2...    10.0.0.4
```

1. 기기에서 ID와 키를 만듭니다. Windows 콘솔은 `vpn_client.exe enroll`, GUI는 `포스처` 탭의 `기기 등록...` → `복사`.
2. 그 값을 이 파일에 옮겨 적고 터널 IP를 배정합니다. `test/devices.conf.example`을 복사해 시작하면 됩니다.
3. `chmod 600`. 모든 기기의 키가 평문으로 들어 있어, 그룹이나 다른 사용자가 읽을 수 있으면 서버가 읽기를 거부합니다.
4. 서버를 띄우거나, 이미 돌고 있으면 `sudo kill -HUP $(pidof vpn_server)`.

| 조건 | 뜻 |
|---|---|
| `firewall=on` | Windows 방화벽이 켜져 있어야 함 (활성 프로필 기준) |
| `antivirus=on` | Windows 보안 센터가 백신을 정상으로 봐야 함 |
| `min-build=N` | OS 빌드가 N 이상 (Windows 11은 22000부터) |

- 조건은 **기기마다 따로** 겁니다. 생략한 항목은 검사하지 않습니다.
- 클라이언트가 값을 읽지 못해 "모름"으로 보고한 항목은 **통과로 치지 않습니다.** 통과시키면 조회를 실패하게 만드는 것만으로 정책을 피할 수 있습니다.
- 모르는 단어가 있으면 서버가 시작하지 않습니다. `firewal=on` 같은 오타가 조용히 지나가면 걸었다고 믿는 정책이 실제로는 걸리지 않습니다.
- 같은 ID나 같은 터널 IP가 두 번 나와도 거부합니다.
- 핸드셰이크와 **매 keepalive**에서 평가합니다. 연결된 뒤에 방화벽을 끄면 늦어도 keepalive 주기 안에 세션이 내려갑니다.
- `kill -HUP` 뒤에도 열려 있는 세션은 건드리지 않고, 다음 keepalive가 새 목록에 대 보고 걸러집니다.
  목록에서 지운 기기는 그때 끊깁니다. 파일이 깨져 있으면 이전 목록을 그대로 씁니다.

```
[HS] device 2bd5d6e0... authenticated as 10.0.0.3 (build=26200 firewall=2 antivirus=2)
[ka] device 2bd5d6e0... posture rejected (build=26200 firewall=1 antivirus=2)
[peer] drop 10.0.0.3 idx=f5421101
```

ACL과의 관계: 기기 정책은 "이 기기를 터널에 들일 것인가"를, ACL은 "들어온 패킷이 어디로 갈 수 있는가"를 정합니다.
지금은 포스처 조건이 기기 단위에만 걸립니다.

---

## ACL

규칙 파일은 한 줄에 하나씩, **위에서부터 처음 일치한 규칙**이 적용되며 일치하는 규칙이 없으면 거부합니다.

```
# src           dst            proto   dport        action
10.0.0.0/24     10.0.0.1       icmp    any          allow
10.0.0.0/24     10.0.0.1       tcp     22           allow
10.0.0.0/24     10.0.0.1       tcp     8000-8080    allow
any             any            any     any          deny
```

| 필드 | 형식 |
|---|---|
| src / dst | `a.b.c.d`, `a.b.c.d/n`, `any` |
| proto | `icmp`, `tcp`, `udp`, `any`, 또는 숫자 |
| dport | `n`, `lo-hi`, `any`, `-` (tcp/udp에만 적용) |
| action | `allow`, `deny` |

- 최대 32개 규칙, `#` 이후는 주석입니다.
- 현재는 클라이언트 → 서버 방향(UDP → TUN)에 적용됩니다.

