# Linux (서버 + 클라이언트)

와이어 프로토콜과 설계는 [루트 README](../README.md)에 있습니다. 이 문서는 Linux 쪽 빌드·실행·테스트와 ACL만 다룹니다.

---

## Build

```bash
make            # lib + client + server
make clean      # 오브젝트/산출물 삭제
make distclean  # bin/ 디렉터리 전체 삭제
```

- 요구사항: Linux, GCC, `/dev/net/tun`, OpenSSL 3.0 이상의 개발 헤더 (`openssl-devel` / `libssl-dev`) — 서버가 기기 인증과 세션 암호화에 씁니다
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
sudo ./server/bin/vpn_server [-s status-socket] [-n prefix-len] [-m mtu] <port> <device-file> [acl-file]

# 다른 터미널에서 인터페이스 설정 (생성된 이름은 로그의 "tun device:" 확인)
sudo ip addr add 10.0.0.1/24 dev tun0
sudo ip link set tun0 mtu 1400 up

./server/bin/vpn_server selftest      # 암호 도구와 세션 암호화가 표준 벡터와 맞는지 (root 불필요)
sudo kill -HUP $(pidof vpn_server)    # 기기 목록과 ACL 을 다시 읽는다 (세션은 유지)
```

- `<device-file>`은 필수입니다. 등록된 기기만 세션을 받으므로, 목록이 없으면 누구도 받아 줄 수 없습니다.
  아래 [기기 등록과 포스처 정책](#기기-등록과-포스처-정책)을 보세요.
- `-s <path>`를 주면 그 경로에 [상태 소켓](#상태-소켓)을 엽니다. 주지 않으면 열지 않습니다.
- `-n` / `-m`은 터널 대역의 prefix 길이와 MTU입니다 (기본 `24`, `1400`). 서버 자신의 tun에 붙인 값(`ip addr add ...`, `ip link set ... mtu`)과 같게 주세요. 서버는 이 값과 기기 목록의 터널 주소, ACL에서 뽑은 경로를 클라이언트에 내려 줍니다 — 클라이언트 쪽에는 터널 주소를 적지 않습니다.
- 서비스로 띄우고 AI 클라이언트로 제어하려면 [MCP 서버](mcp.md)를 보세요.

### Client

```bash
sudo ./client/bin/vpn_client <ifname> <server-ip> <port> <tunnel-ip>
# 예) sudo ./client/bin/vpn_client tun0 203.0.113.10 9000 10.0.0.2

sudo ip addr add 10.0.0.2/24 dev tun0
sudo ip link set tun0 mtu 1400 up
```

> **Linux 클라이언트는 인증 이전의 프로토콜에 머물러 있고, 더 발전시키지 않습니다.** 예전 형식의
> 핸드셰이크를 보내므로 지금 서버에는 붙지 못합니다(`[HS] short request`로 버려집니다). 아래 내용과
> `test/client.sh`의 시나리오는 기기 인증을 넣기 전 단계의 기록입니다. 지금 서버에 붙는 클라이언트는
> [Windows 클라이언트](../windows/README.md)입니다.

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

