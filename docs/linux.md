# Linux (서버 + 클라이언트)

와이어 프로토콜과 설계는 [루트 README](../README.md)에 있습니다. 이 문서는 Linux 쪽 빌드·실행·테스트와 ACL만 다룹니다.

---

## Build

```bash
make            # lib + client + server
make clean      # 오브젝트/산출물 삭제
make distclean  # bin/ 디렉터리 전체 삭제
```

- 요구사항: Linux, GCC, `/dev/net/tun`, OpenSSL 3.0 이상의 개발 헤더 (`openssl-devel` / `libssl-dev`) — 서버와 클라이언트가 기기 인증과 세션 암호화에 씁니다. 클라이언트의 방화벽 포스처는 `nft`(nftables)를 읽습니다
- `-Wall -Wextra` 경고 없이 빌드됩니다.

| 산출물 | 경로 |
|---|---|
| 공용 라이브러리 | `lib/common/bin/libcommon.a` (프로토콜, 세션 암호화, OpenSSL 암호 도구) |
| 클라이언트 쪽 프로토콜 | `client/bin/libclient.a` (핸드셰이크 / 등록 / 신원 / 포스처 / 터널 설정 — 클라이언트와 부하 생성기가 함께 쓴다) |
| 클라이언트 | `client/bin/vpn_client` |
| 서버 | `server/bin/vpn_server` |
| 부하 생성기 | `tools/loadgen/bin/loadgen` |

---

## Usage

TUN 디바이스 생성에 root 권한(`CAP_NET_ADMIN`)이 필요합니다.

### Server

```bash
sudo ./server/bin/vpn_server [-s status-socket] [-a audit-log] [-e enroll-file] [-n prefix-len] [-m mtu] <port> <device-file> [acl-file]

# 다른 터미널에서 인터페이스 설정 (생성된 이름은 로그의 "tun device:" 확인)
sudo ip addr add 10.0.0.1/24 dev tun0
sudo ip link set tun0 mtu 1400 up

./server/bin/vpn_server selftest      # 암호 도구와 세션 암호화가 표준 벡터와 맞는지 (root 불필요)
sudo kill -HUP $(pidof vpn_server)    # 기기 목록과 ACL 을 다시 읽는다 (세션은 유지)
```

- `<device-file>`은 필수입니다. 등록된 기기만 세션을 받으므로, 목록이 없으면 누구도 받아 줄 수 없습니다.
  아래 [기기 등록과 포스처 정책](#기기-등록과-포스처-정책)을 보세요.
- `-s <path>`를 주면 그 경로에 [상태 소켓](#상태-소켓)을 엽니다. 주지 않으면 열지 않습니다.
- `-a <path>`를 주면 그 파일에 [감사 로그](#감사-로그)를 남깁니다.
- `-e <path>`를 주면 그 파일을 [등록 코드](#등록-코드)의 대기 목록으로 쓰고, 코드로 온 기기를 `<device-file>`에 직접 올립니다.
- `-n` / `-m`은 터널 대역의 prefix 길이와 MTU입니다 (기본 `24`, `1400`). 서버 자신의 tun에 붙인 값(`ip addr add ...`, `ip link set ... mtu`)과 같게 주세요. 서버는 이 값과 기기 목록의 터널 주소, ACL에서 뽑은 경로를 클라이언트에 내려 줍니다 — 클라이언트 쪽에는 터널 주소를 적지 않습니다.
- 서비스로 띄우고 AI 클라이언트로 제어하려면 [MCP 서버](mcp.md)를 보세요.

### Client

```bash
# 1. 등록 (한 번). 관리자에게 받은 등록 코드를 적는다. 신원은 서버가 받아 준 뒤에 저장된다.
sudo ./client/bin/vpn_client enroll 203.0.113.10 9000 7K2QF-9XWM4-ABCDE-FGHJK-MNPQR

# 2. 연결. 터널 주소, MTU, 경로는 서버가 내려 준다 — 여기서 적지 않는다.
sudo ./client/bin/vpn_client run 203.0.113.10 9000

./client/bin/vpn_client posture     # 서버에 보고할 포스처를 미리 본다 (nft 를 읽으려면 root)
./client/bin/vpn_client selftest    # 암호 도구의 표준 벡터
```

| 옵션 | 뜻 |
|---|---|
| `-k <file>` | 기기 신원 파일 (기본 `/var/lib/my_vpn/device`). `id(32 hex) key(64 hex)` 한 줄, 권한 `600`. 넓으면 읽지 않는다 |
| `-i <name>` | tun 장치 이름 (기본 `myvpn0`) |
| `-f` | `enroll`: 이미 있는 신원을 바꾼다 (서버에 등록된 예전 키는 더 이상 맞지 않는다) |
| `-v` | 패킷마다 로그 |

```
[HS] posture: kernel=331264 firewall=off antivirus=unknown
[HS] request 1/5
[HS] established idx=b892bf00
[myvpn0] tun device ready
[cfg] address 10.0.0.2/24
[cfg] mtu 1400
[cfg] route + 198.51.100.0/24
[myvpn0] connected
```

- 서버가 받아 준 뒤에 tun 을 만든다. 거절되면 장치를 만들지 않고 종료 코드 2로 끝난다.
- 동작은 Windows 클라이언트와 같다: 10초마다 keepalive 로 포스처를 다시 보고하고(포스처는 2초마다 읽어 바뀌면 바로 보고), 서버가 30초 동안 조용하면 다시 핸드셰이크한다. 세션 중에 거절되면 tun 은 두고 패킷만 막으며, 포스처가 바뀌면 바로 다시 묻는다.
- MTU 1400 은 바깥 IP(20) + UDP(8) + 우리 헤더(16) + GCM tag(16) = 60바이트를 더해도 1500 을 넘지 않는 값이다.

**Linux 포스처**: 와이어 형식은 Windows 와 같고, 칸마다 Linux 에서 뜻이 있는 값을 넣는다 (`client/include/posture.h`).

| 항목 | Linux 에서 | 서버 정책 |
|---|---|---|
| OS 빌드 | 커널 버전, `LINUX_VERSION_CODE` 모양 (`major<<16 \| minor<<8 \| patch`, 5.14.0 → 331264) | `min-build=331264` |
| 방화벽 | nftables 에 input 훅 `policy drop` 체인이 있거나 firewalld 의 표(`table inet firewalld`)가 있으면 켜짐. nft 를 못 읽으면 "모름" | `firewall=on` |
| 백신 | 수집하지 않는다 (늘 "모름") | Linux 기기에 `antivirus=on` 을 걸지 않는다 |

### End-to-end (Linux 서버 ↔ Linux 클라이언트, 한 장비)

```bash
make && test/linux_e2e.sh          # root 가 아니면 unshare -rn 으로 사용자 네임스페이스 안에서 돈다
```

네트워크 네임스페이스 두 개(서버 / 클라이언트)를 veth 로 이어 등록 → 연결(서버가 준 주소·MTU·경로) → ACL 차단 → 포스처 거부와 복구(nft 로 방화벽 켜기)까지 확인한다. CI 에서도 돈다.

### Test scripts

`test/`의 스크립트는 빌드와 실행을 한 번에 하고 시나리오별로 `PASS` / `FAIL`을 출력합니다.
서버 장비에서 먼저 실행한 뒤 클라이언트 장비에서 실행합니다. 클라이언트는 미리 등록돼 있어야 합니다(`IDENTITY_FILE`, 기본 `/var/lib/my_vpn/device`).

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
| 핸드셰이크 | 모르는 기기, 틀린 키, 이전 버전, `AUTH` 재전송, 만료된 nonce, 서명 뒤 포스처 변조 / 일회용 공개키 바꿔치기, 곡선 위에 없는 공개키, 배정되지 않은 터널 IP, 거절에 붙는 mac, 핸드셰이크마다 다른 세션 키 |
| 핸드셰이크 방해 | 다른 곳에서 온 `REQ` 20개 / 엉터리 `AUTH` 뒤에도 진짜 기기가 통과, 쓰인 nonce는 다시 나오지 않음 |
| 포스처 | 방화벽 꺼짐 / 모름, 백신 꺼짐, 빌드 미달 / 경계값 / 모름, 조건이 없는 기기 |
| keepalive | 재전송, 예전 counter, 합의한 적 없는 키 / 다른 세션의 키 / 반대 방향의 키, 한 비트 변조, 세션 번호와 종류 바꿔치기, 위조 패킷 뒤에도 세션 유지, 재인증 시 새 세션과 새 키, 세션 키로 감싼 거절 |
| 데이터 | 인증된 세션으로 ping 왕복, 내용이 와이어에 보이지 않음, 재전송 / 한 비트 변조 / 틀린 키, 순서가 바뀐 패킷과 창 밖으로 밀려난 패킷, 모르는 idx, inner src 위조, 거절된 뒤의 ping |
| 다시 읽기 | 정책 강화 / 완화, 기기 삭제, 깨진 파일일 때 이전 목록 유지 |
| ACL | 규칙 파일의 실수(deny에 붙은 조건, 오타, 빠진 필드), `SIGHUP`으로 규칙 교체, 깨진 파일일 때 이전 규칙 유지 |
| 규칙별 포스처 | keepalive로 포스처만 바꿔 가며 같은 패킷의 판정 — 꺼짐 / 모름 / 빌드 미달, 조건 없는 규칙은 통과, 아래쪽의 넓은 allow가 구해 주지 않음 |
| 상태 소켓 | 소켓 권한, 세션 / 포스처 / 패킷 수 / 거부 횟수, 내려간 세션, 버려진 접속 50개, 입력 무시, 종료 시 파일 삭제 |

서버를 두 번 띄웁니다 (ACL 없이 한 번, ACL을 켜고 한 번). 서버의 tun에 `10.0.0.1/24`를 붙이므로 다른 `vpn_server`가 도는 동안에는 쓸 수 없습니다.

---

## Docker

서버와 Linux 클라이언트를 한 이미지(`Dockerfile`, debian bookworm-slim)에 담았습니다. 첫 인자 `server` / `client` 로 고릅니다.

```bash
docker build -t my_vpn .

# 서버. 설정(기기 목록, ACL, 등록 대기 목록, 감사 로그)은 /etc/my_vpn 볼륨에 남는다.
docker run -d --name vpn --cap-add NET_ADMIN --device /dev/net/tun \
    -p 9000:9000/udp -v my_vpn_etc:/etc/my_vpn my_vpn server

# 등록 코드 (서버 컨테이너 안에서, 셸과 openssl 만으로 만든다)
docker exec vpn enroll-code.sh 10.0.0.2 firewall=on -l laptop
#   -> 7K2QF-9XWM4-ABCDE-FGHJK-MNPQR

# 클라이언트. 신원은 /var/lib/my_vpn 볼륨에 남고, 다음부터는 ENROLL_CODE 없이 붙는다.
docker run -d --cap-add NET_ADMIN --device /dev/net/tun \
    -e SERVER=203.0.113.10 -e ENROLL_CODE=7K2QF-9XWM4-ABCDE-FGHJK-MNPQR \
    -v my_vpn_id:/var/lib/my_vpn my_vpn client
```

| 환경 변수 | 쪽 | 뜻 |
|---|---|---|
| `PORT` | 둘 다 | UDP 포트 (기본 9000) |
| `TUN_ADDR`, `MTU` | 서버 | 서버 tun 의 주소와 MTU (기본 `10.0.0.1/24`, 1400). prefix 와 MTU 는 `-n` / `-m` 으로 클라이언트에 그대로 내려간다 |
| `SERVER` | 클라이언트 | 서버 주소 (이름이면 entrypoint 가 IPv4 로 푼다) |
| `ENROLL_CODE` | 클라이언트 | 신원 파일이 없을 때만 쓴다 |

- **권한은 `CAP_NET_ADMIN` 과 `/dev/net/tun` 만 줍니다.** tun 을 만들고, 주소와 경로를 넣고, nft 를 읽는 데 필요한 것이 이것뿐입니다. `--privileged` 는 쓰지 않습니다.
- 서버 tun 에 주소를 붙이는 일(호스트에서 하던 `ip addr add 10.0.0.1/24 dev tun0`)은 `docker/entrypoint.sh` 가 합니다.
- `/etc/my_vpn` 에 `acl.conf` 가 있으면 서버가 뜰 때 읽습니다. ACL 을 쓸지는 시작할 때만 정해지므로, 처음 넣었다면 컨테이너를 다시 띄웁니다. 그 뒤의 수정은 `docker exec vpn pkill -HUP -x vpn_server`.
- 설정 파일은 서버가 권한 `600` 이 아니면 읽지 않습니다. 호스트 디렉터리를 바인드 마운트할 때는 호스트 쪽 권한을 맞춰야 합니다 (entrypoint 가 고치기는 하지만 읽기 전용 마운트면 못 고친다).
- **서버 뒤의 네트워크로 내보내기(NAT)는 컨테이너 예시에 넣지 않았습니다.** 호스트의 forwarding 과 nftables 설정이 얽힙니다.
- **컨테이너의 포스처는 그 컨테이너의 것입니다.** 클라이언트 컨테이너가 보고하는 방화벽 상태는 컨테이너 네트워크 네임스페이스의 nft 규칙셋이고, 호스트의 방화벽이 아닙니다. 커널 버전은 호스트의 커널입니다.

---

## 기기 등록과 포스처 정책

서버는 기기 목록 파일에 있는 기기만 받습니다. 한 줄에 기기 하나입니다.

```
# id (32 hex)                      key (64 hex)   tunnel-ip   requirements
2bd5d6e0610d2b7c07f884d0c5afb043   a24d4c3d...    10.0.0.3    firewall=on antivirus=on min-build=22000
9c1e77a0b2d34f5e8a6b0c1d2e3f4a5b   77e0c1b2...    10.0.0.4
```

기기를 이 파일에 올리는 방법은 둘입니다. 보통은 [등록 코드](#등록-코드)를 씁니다. 아래는 손으로 옮기는 방법이고, 서버를 `-e` 없이 띄웠을 때 씁니다.

1. 기기에서 ID와 키를 만듭니다. Linux 클라이언트는 코드 없이 만드는 길이 없으므로 등록 코드를 씁니다. Windows 콘솔은 `vpn_client.exe enroll`, GUI는 `포스처` 탭의 `기기 등록...` → `코드 없이...` → `복사`.
2. 그 값을 이 파일에 옮겨 적고 터널 IP를 배정합니다. `test/devices.conf.example`을 복사해 시작하면 됩니다.
3. `chmod 600`. 모든 기기의 키가 평문으로 들어 있어, 그룹이나 다른 사용자가 읽을 수 있으면 서버가 읽기를 거부합니다.
4. 서버를 띄우거나, 이미 돌고 있으면 `sudo kill -HUP $(pidof vpn_server)`.

| 조건 | 뜻 |
|---|---|
| `firewall=on` | Windows: 방화벽이 켜져 있어야 함 (활성 프로필 기준). Linux: nftables 인바운드 기본 거부 또는 firewalld |
| `antivirus=on` | Windows 보안 센터가 백신을 정상으로 봐야 함 (Linux 는 수집하지 않아 늘 거절된다) |
| `min-build=N` | Windows: OS 빌드가 N 이상 (Windows 11은 22000부터). Linux: 커널 버전 코드가 N 이상 (위 표) |

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
같은 포스처 조건을 ACL 규칙에도 걸 수 있습니다. 기기 조건에 못 미치면 세션이 내려가고, 규칙의 조건에 못 미치면
세션은 유지된 채 그 목적지만 닫힙니다.

---

## ACL

규칙 파일은 한 줄에 하나씩, **위에서부터 처음 일치한 규칙**이 적용되며 일치하는 규칙이 없으면 거부합니다.

```
# src           dst            proto   dport        action   [requirements]
10.0.0.0/24     10.0.0.1       icmp    any          allow
10.0.0.0/24     10.0.0.1       tcp     22           allow    firewall=on antivirus=on
10.0.0.0/24     10.0.0.1       tcp     8000-8080    allow
any             any            any     any          deny
```

| 필드 | 형식 |
|---|---|
| src / dst | `a.b.c.d`, `a.b.c.d/n`, `any` |
| proto | `icmp`, `tcp`, `udp`, `any`, 또는 숫자 |
| dport | `n`, `lo-hi`, `any`, `-` (tcp/udp에만 적용) |
| action | `allow`, `deny` |
| requirements | `firewall=on`, `antivirus=on`, `min-build=N` — 생략 가능, `allow`에만 |

- 최대 32개 규칙, `#` 이후는 주석입니다.
- 클라이언트가 보낸 패킷(UDP → TUN)에 적용됩니다. 기기끼리의 통신도 여기서 걸립니다 — 한 기기가 다른 기기로 보낸 패킷은 서버를 거쳐 가기 때문입니다. 반대 방향(서버나 뒤쪽 네트워크가 기기로 먼저 거는 것)은 이 파일이 아니라 아래 "서버 뒤의 네트워크로 내보내기"의 전달 규칙이 막습니다.
- **규칙 파일을 주지 않으면 터널 대역 안까지만 통과합니다.** 서버의 터널 주소와 다른 기기까지는 가지만 그 밖(서버 뒤의 네트워크, 인터넷)은 막습니다. 전에는 "파일이 없으면 전부 허용"이었는데, 그때는 터널 대역 밖으로 가는 길 자체가 없어서 결과가 같았습니다. 이제 서버가 뒤쪽 네트워크로 전달할 수 있으므로, 규칙을 적지 않았다는 이유로 그 네트워크 전체가 열려서는 안 됩니다. 규칙 파일을 쓸지는 서버를 시작할 때만 정해집니다.
- **`allow` 규칙의 목적지는 경로이기도 합니다.** 서버는 각 기기에 "터널로 보낼 대역"을 내려 주는데, 그 목록이 이 파일에서 나옵니다 — 그 기기의 터널 주소가 `src`에 걸리는 `allow` 규칙의 `dst`입니다. 터널 대역 안의 목적지와 `any`는 내려가지 않습니다. `any any any any allow`는 터널로 들어온 패킷은 통과시키지만, 클라이언트가 모든 트래픽을 터널로 보내게 하지는 않습니다.
- `kill -HUP`으로 규칙을 다시 읽습니다. 다음 패킷부터 새 규칙이 적용되고 세션은 유지됩니다. 파일이 깨져 있으면 이전 규칙을 그대로 씁니다.
- 필드가 모자란 줄, 모르는 조건, `deny`에 붙은 조건이 있으면 서버가 시작하지 않습니다. 조용히 건너뛰면 `deny` 한 줄이 빠진 채 그 아래의 `allow`가 적용됩니다.

### 서버 뒤의 네트워크로 내보내기

`vpn_server`는 통과시킨 패킷을 tun에 쓰는 데까지만 합니다. 그 뒤 어디로 보낼지는 커널의 일입니다. 기기가 서버 뒤의 네트워크(예: `10.101.0.0/16`)에 닿게 하려면 세 가지가 필요합니다.

1. **규칙**: `10.0.0.3  10.101.0.0/16  tcp  443  allow` — 누가 어디로 갈 수 있는지. 이 줄이 그 기기에 경로로도 내려갑니다.
2. **전달**: `sysctl -w net.ipv4.ip_forward=1`
3. **주소 변환**: 터널 주소(`10.0.0.x`)는 서버만 아는 주소입니다. 그대로 내보내면 받은 쪽이 답을 돌려보낼 길이 없으므로, 나갈 때 서버의 주소로 바꿉니다.

```bash
sudo nft -f - <<'EOF'
table ip my_vpn {
	chain forward {
		type filter hook forward priority filter; policy accept;
		iifname "tun0" oifname "tun0" accept
		iifname "tun0" oifname "eth0" accept
		iifname "eth0" oifname "tun0" ct state established,related accept
		iifname "tun0" drop
		oifname "tun0" drop
	}
	chain postrouting {
		type nat hook postrouting priority srcnat; policy accept;
		ip saddr 10.0.0.0/24 oifname "eth0" masquerade
	}
}
EOF
```

- 바깥에서 터널 쪽으로는 **안에서 시작한 연결의 답만** 들입니다. 기기가 사내 자원에 닿게 하려는 것이지, 사내에서 기기로 들어오게 하려는 것이 아닙니다.
- 서비스로 설치했다면 직접 넣지 않아도 됩니다. `/etc/my_vpn/server.env`에 `FORWARD_OUT=eth0`을 적으면 서비스가 뜰 때 위의 것을 넣고 내려갈 때 뺍니다 ([MCP 서버](mcp.md)).
- firewalld 같은 다른 방화벽이 전달을 막고 있으면 그쪽에서도 열어야 합니다. 위의 표는 장비에 이미 있는 규칙을 고치지 않습니다.
- 규칙에 없는 목적지는 tun에 쓰이지도 않으므로 2 · 3번만 켠다고 열리는 것은 없습니다.

### 규칙별 포스처 조건

`allow` 규칙의 action 뒤에 [기기 목록과 같은 조건](#기기-등록과-포스처-정책)을 적으면, 그 규칙으로 허용받으려는 세션이
**지금** 그 포스처여야 합니다. 판정에 쓰는 값은 세션이 마지막으로 서명해서 보고한 포스처이고, 클라이언트는
keepalive(10초)마다 다시 보고합니다.

- 주소와 포트로 규칙이 정해지고, 조건은 그 규칙 안에서 봅니다. **조건에 못 미쳤다고 다음 규칙으로 넘어가지 않습니다.**
  넘어가면 아래쪽의 넓은 `allow`가 받아 줘서 조건을 건 줄이 아무것도 막지 못합니다.
- "모름"은 여기서도 통과가 아닙니다.
- 세션은 내려가지 않습니다. 방화벽을 다시 켜면 다음 keepalive부터 통합니다.

```
[deny] 10.0.0.3 -> 10.0.0.1 proto=6 (60 bytes) posture below the rule (build=26200 firewall=1 antivirus=2)
```

---

## 등록 코드

기기의 ID(32자)와 키(64자)를 손으로 옮기면 키가 화면과 클립보드와 메신저를 거치고, 한 글자만 틀려도 "인증 실패"만 돌아옵니다. `-e <path>`로 띄운 서버는 등록 코드를 받습니다.

1. 관리자가 코드를 만듭니다. 그 기기가 받을 터널 주소, 포스처 조건, 이름을 이때 정합니다. MCP의 `enroll_create`가 이 일을 합니다 — 코드에서 뽑은 임시 신원을 대기 목록(`-e`의 파일)에 한 줄 쓰고 `SIGHUP`을 보냅니다.
2. 기기에서 코드를 적습니다. Linux 는 `sudo vpn_client enroll <server-ip> <port> <code>`, Windows 는 `vpn_client.exe enroll <server-ip> <port> <code>` 또는 GUI 의 `포스처` 탭 `기기 등록...`.
3. 기기가 코드로 서버와 세션을 열고, 그 세션 안에서 자기가 만든 ID와 키를 보냅니다. 서버가 기기 목록에 한 줄을 더하고 대기 목록에서 그 코드를 지웁니다.

```
# 대기 목록: id (32 hex)  key (64 hex)  tunnel-ip  expires (unix)  [requirements]  # label
233e0973542fd3d09b3af964a36d1840  9f1c…  10.0.0.2  1791900000  firewall=on  # laptop-kim
```

- **새 핸드셰이크를 만들지 않았습니다.** 코드에서 임시 신원(ID와 키)을 뽑아 평소의 핸드셰이크를 그대로 돕니다. 그러면 이미 있는 것이 전부 따라옵니다 — 코드를 모르는 쪽은 세션을 열지 못하고, 서버인 척하는 쪽은 수락을 만들지 못하고, 기기의 진짜 키는 그 세션의 키로 감싸여 가므로 코드를 나중에 알게 된 사람도 녹화해 둔 등록에서 키를 꺼내지 못합니다.
- **코드는 한 번만 쓸 수 있고 만료됩니다.** 코드는 0-9와 헷갈리는 글자(I L O U)를 뺀 대문자 25자(125비트)입니다. 소문자, 줄표, 공백, O와 0 / I · L과 1을 섞어 적어도 같은 코드로 읽습니다.
- **등록용 세션으로는 등록 말고 아무것도 하지 못합니다.** 터널 패킷도 keepalive도 받지 않습니다. 코드만 가진 쪽이 그 세션으로 터널에 닿을 수 있으면 등록 코드가 곧 접속 권한이 됩니다.
- **기기가 정할 수 있는 것은 자기 신원뿐입니다.** 터널 주소, 포스처 조건, 이름은 관리자가 코드를 만들 때 정한 것이 그대로 기기 목록에 적힙니다.
- **포스처는 등록할 때 따지지 않습니다.** 그 기기가 등록을 마치고 자기 키로 터널을 열 때 겁니다. 등록까지 막으면 "방화벽을 켜고 다시 오라"는 말을 등록 실패로 듣게 됩니다.
- **답이 사라져 같은 요청이 다시 와도 한 번만 등록됩니다.** 같은 ID로 다시 오면 같은 답을 하고, 같은 세션으로 다른 ID를 올리려 하면 거절합니다.
- **신원은 서버가 받아 준 뒤에 기기에 저장됩니다.** 먼저 저장하면 서버에 닿지 못했을 때 예전 신원은 지워졌고 새 신원은 어디에도 등록되지 않은 채로 남습니다.
- **`-e`를 준 서버는 기기 목록 파일을 스스로 고칩니다.** 옆에 새 파일을 쓰고 이름을 바꾸는 식이라 반쯤 쓰인 파일이 보이지 않습니다. `-e` 없이 띄운 서버는 예전대로 그 파일을 읽기만 합니다.
- 대기 목록에 적힌 것은 코드가 아니라 코드에서 뽑은 값이지만, 그 값으로 등록용 세션은 열 수 있습니다. 기기 목록처럼 `600`이 아니면 서버가 읽지 않습니다.

막지 못하는 것: 코드를 가로챈 사람은 만료 전까지 그 자리에 자기 기기를 올릴 수 있습니다. 코드를 건네는 길(대화, 메신저)이 곧 신뢰의 근거입니다. 그 기기가 올라오면 감사 로그의 `enrolled`에 외부 주소와 함께 남습니다.

## 감사 로그

`-a <path>`로 띄우면 서버가 그 파일에 사건마다 한 줄씩 JSON을 덧붙입니다. 주지 않으면 남기지 않습니다.

서버가 stdout에 찍는 로그는 사람이 따라 읽으라고 쓴 것이라 패킷마다 한 줄이고, 기기 ID와 세션과 목적지가 서로 다른 줄에 흩어져 있습니다. "어제 이 기기가 어디에 접근했는가"에 답하려면 처음부터 따라가며 다시 맞춰야 합니다. 상태 소켓은 지금의 상태만 보여 주고 지나간 일은 모릅니다. 감사 로그에는 패킷이 아니라 사건을 적습니다.

```json
{"time":"2026-10-13T12:34:56Z","event":"session_open","device":"86a5…53a3","tunnel_ip":"10.0.0.3","session":"9862b000","endpoint":"218.145.242.230:62975","posture":{"os_build":19045,"firewall":"on","antivirus":"on"}}
{"time":"2026-10-13T12:35:02Z","event":"access","device":"86a5…53a3","tunnel_ip":"10.0.0.3","session":"9862b000","endpoint":"218.145.242.230:62975","dst":"10.101.0.1","proto":1,"dport":0,"verdict":"allow"}
{"time":"2026-10-13T12:35:09Z","event":"access","device":"86a5…53a3","tunnel_ip":"10.0.0.3","session":"9862b000","endpoint":"218.145.242.230:62975","dst":"10.101.0.7","proto":6,"dport":22,"verdict":"deny","why":"posture"}
{"time":"2026-10-13T12:41:30Z","event":"session_close","device":"86a5…53a3","tunnel_ip":"10.0.0.3","session":"9862b000","endpoint":"218.145.242.230:62975","reason":"expired","duration_sec":394,"rx_packets":41,"rx_bytes":3120,"tx_packets":38,"tx_bytes":2980,"acl_denied":3}
```

| 사건 | 언제 | 덧붙는 값 |
|---|---|---|
| `session_open` | 기기가 인증을 통과해 세션이 열렸다 | 기기, 터널 주소, 세션, 외부 주소, 포스처 |
| `session_close` | 세션이 닫혔다 | `reason`: `expired`(60초 무응답) / `posture` / `device_removed` / `replaced`(같은 기기가 다시 인증) / `evicted`(표가 가득 참) / `error`, 열려 있던 시간, 오간 패킷과 바이트, 막힌 횟수 |
| `auth_denied` | 핸드셰이크가 거절됐다 | 상대가 댄 기기 ID, 외부 주소, `reason`: `unknown_device` / `bad_auth` / `posture` |
| `access` | 세션이 어떤 목적지에 닿았다 / 막혔다 | 목적지 주소, 프로토콜, 포트, `verdict`, 막혔으면 `why`: `acl` / `posture` / `no_acl_file` |
| `reload` | `SIGHUP`으로 기기 목록이나 ACL을 다시 읽었다 | `what`, `ok`, 읽은 개수 |
| `enrolled` | 등록 코드로 기기가 목록에 올랐다 | 기기, 터널 주소, 외부 주소, 코드에 붙인 이름 |

- **`access`는 패킷마다 적지 않습니다.** 같은 세션의 같은 목적지(주소, 프로토콜, 포트)와 같은 판정은 60초에 한 번입니다. 기억하는 자리는 세션마다 8개라, 목적지를 많이 오가는 세션은 60초 안에 같은 목적지가 다시 적힐 수 있습니다 — 덜 적는 쪽이 아니라 더 적는 쪽으로 틀립니다.
- **거절된 핸드셰이크는 1초에 20줄까지만 적습니다.** 기기 ID는 공개된 이름표라 누구나 틀린 요청을 쏟아부을 수 있고, 그때마다 한 줄씩 쓰면 디스크가 먼저 찹니다. 넘친 만큼은 세어 두었다가 다음 줄의 `suppressed`에 적습니다.
- 파일은 `600`으로 만들어집니다. `SIGHUP`을 받으면 같은 경로를 다시 엽니다 — 로그를 돌리는 쪽이 파일 이름을 바꾼 뒤 `SIGHUP`을 보내면 새 파일에 이어 씁니다.
- 값은 전부 주소, 숫자, 16진수, 서버가 정한 단어입니다. 바깥에서 온 문자열은 적지 않습니다.
- 서비스로 설치하면 `/var/log/my_vpn/audit.log`에 쌓이고, MCP의 `audit_log` 도구로 걸러 읽을 수 있습니다.

## 상태 소켓

`-s <path>`로 띄우면 서버가 그 경로에 유닉스 소켓을 엽니다. 접속하면 JSON 한 덩어리를 쓰고 닫습니다.

```bash
sudo ./server/bin/vpn_server -s /run/vpn_status.sock 9000 devices.conf acl.conf
sudo python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); print(s.makefile().read())' /run/vpn_status.sock
```

```json
{"uptime_sec":5,"devices":1,"acl":{"enabled":true,"rules":2},
 "counters":{"handshake_ok":1,"deny_unknown_device":1,"deny_bad_auth":1,"deny_tun_ip":0,"deny_posture":0,
             "keepalive_dropped":0,"unknown_session":0,"spoofed_src":0,"acl_denied":2,"acl_denied_posture":1},
 "sessions":[{"tunnel_ip":"10.0.0.3","device_id":"1011...1e1f","endpoint":"203.0.113.7:55425","session_idx":"a22a0400",
              "age_sec":5,"idle_sec":5,"posture":{"os_build":26200,"firewall":"off","antivirus":"on"},
              "rx_packets":3,"rx_bytes":135,"tx_packets":3,"tx_bytes":135,"acl_denied":2}]}
```

- **읽기 전용입니다.** 접속한 쪽이 보낸 것은 읽지 않으므로 이 소켓으로 서버에 시킬 수 있는 일이 없습니다.
- 소켓 파일은 `600`으로 만들어집니다. 서버를 띄운 계정(root)만 읽습니다.
- `posture`는 그 세션이 마지막으로 보고한 값, `idle_sec`는 마지막 수신 뒤 지난 시간입니다. `rx`는 ACL을 통과한 것만 셉니다.
- 숫자는 서버가 뜬 뒤부터 센 값입니다. 키는 싣지 않습니다.

