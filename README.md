# my_vpn

[![ci](https://github.com/k1msatgat/my_vpn/actions/workflows/ci.yml/badge.svg)](https://github.com/k1msatgat/my_vpn/actions/workflows/ci.yml)

> C로 처음부터 만드는 L3 VPN — TUN 인터페이스 + UDP 터널 + 기기 인증 + 포스처 기반 접근 제어 + ACL

리눅스 TUN 디바이스로 IP 패킷을 가로채고, 직접 설계한 헤더를 붙여 UDP로 터널링하는 VPN입니다.
외부 VPN 라이브러리 없이 **패킷 포맷, 핸드셰이크, 세션 관리, 접근 제어**를 단계별로 구현하며
Zero Trust Network Access(ZTNA)의 기반 구조를 학습하는 것을 목표로 합니다.

```
 ┌──────────── Client ────────────┐                 ┌──────────── Server ────────────┐
 │  app ──▶ kernel ──▶ tun0       │                 │       tun ──▶ kernel ──▶ app   │
 │                      │         │   UDP :9000     │        ▲                        │
 │              [hdr 16B | IP] ───┼────────────────▶│ verify ┼─ peer table ─ ACL     │
 │                                │◀────────────────┼── [hdr 16B | IP]               │
 └────────────────────────────────┘                 └────────────────────────────────┘
```

---

## 문서

| | 내용 |
|---|---|
| 이 문서 | 설계 — 와이어 프로토콜, 패킷 흐름, 보안 모델, 로드맵 |
| [docs/linux.md](docs/linux.md) | Linux 서버·클라이언트 빌드와 실행, 시나리오 테스트, ACL |
| [docs/mcp.md](docs/mcp.md) | MCP 서버 — AI 클라이언트가 Linux 서버를 올리고 내리고, 기기·ACL을 바꾸고, 상태를 본다 |
| [windows/README.md](windows/README.md) | Windows 클라이언트 — Wintun, 이벤트 루프, MFC GUI와 패킷 분석기 |

프로토콜 코드(`lib/common`의 `proto.h` / `proto.c`)는 Linux와 Windows가 같은 소스를 빌드합니다.
플랫폼별로 갈라지는 것만 위 두 문서로 내렸습니다.

---

## Features

- **TUN 기반 L3 터널링** — `IFF_TUN | IFF_NO_PI`로 순수 IPv4 패킷만 송수신
- **자체 와이어 프로토콜** — 버전/타입/세션 idx를 담은 고정 길이 헤더, 빅엔디언 직렬화
- **핸드셰이크 & 세션 idx** — 서버가 클라이언트별로 추측하기 어려운 32bit 세션 식별자 발급
- **기기 인증** — 등록된 기기만 세션을 받는다. 기기별 키로 서버의 nonce에 HMAC-SHA256 서명 (챌린지-응답)
- **포스처 기반 접근 제어** — 클라이언트가 OS 빌드 / 방화벽 / 백신 상태를 보고하고, 서버가 기기별 정책에 대 본다
- **지속 검증** — keepalive마다 포스처를 다시 보고·평가하고, 정책에 못 미치면 세션 중에도 차단
- **정책 무중단 변경** — `kill -HUP`으로 기기 목록과 ACL을 다시 읽는다. 지워진 기기는 다음 keepalive에서 끊기고, 새 ACL은 다음 패킷부터 적용된다
- **피어 테이블** — O(1) 세션 조회, 테이블이 가득 차면 LRU 방식으로 교체
- **ACL** — `src / dst / proto / dport` 기반 first-match 정책, 기본 거부(default deny)
- **리소스별 포스처 조건** — ACL 규칙마다 요구하는 포스처를 따로 건다. 터널에 들어온 기기라도 방화벽을 끄면 SSH 같은 민감한 목적지만 닫힌다
- **상태 소켓** — 지금 붙어 있는 세션(기기, 포스처, 트래픽)과 사유별 거부 횟수를 읽기 전용 유닉스 소켓으로 내보낸다
- **MCP 제어** — AI 클라이언트가 서버를 올리고 내리고, 기기와 ACL을 바꾸고, 상태를 본다 ([docs/mcp.md](docs/mcp.md))
- **위조 방어** — 서버는 inner src가 세션에 등록된 터널 IP와 같은지, 클라이언트는 출발지가 서버인지 확인
- **Keepalive & 세션 만료** — 유휴 시 10초마다 keepalive, 서버는 60초 무응답 세션을 정리
- **재핸드셰이크** — 클라이언트가 30초간 서버 응답이 없으면 스스로 세션을 다시 맺음
- **epoll 단일 스레드 이벤트 루프** — TUN fd와 UDP 소켓을 하나의 루프에서 처리
- **헤드룸 기반 버퍼 설계** — 헤더 자리를 비워두고 읽어 송신 경로에서 추가 memcpy 없음

---

## Protocol

### Wire format

모든 UDP 페이로드는 `msg_header`로 시작하며, 데이터 메시지는 그 뒤에 `data_header`와 원본 IP 패킷이 붙습니다.

```
 0        1        2                 4                                   8
 +--------+--------+-----------------+-----------------------------------+
 | version|  type  |    reserved     |         session_idx (BE)          |  msg_header  (8B)
 +--------+--------+-----------------+-----------------------------------+
 |                         counter (BE, 64bit)                           |  data_header (8B)
 +-----------------------------------------------------------------------+
 |                        inner IPv4 packet ...                          |  payload
 +-----------------------------------------------------------------------+
 \__________________________ PKT_HDR_LEN = 16 __________________________/
```

| type | 값 | 페이로드 |
|---|---|---|
| `MSG_TYPE_REQ_HANDSHAKE` | 1 | `msg_header` + 터널 IP (4B) + 기기 ID (16B) |
| `MSG_TYPE_RES_HANDSHAKE` | 2 | `msg_header` (발급된 `session_idx` 포함) |
| `MSG_TYPE_DATA` | 3 | `msg_header` + `data_header` + IP 패킷 |
| `MSG_TYPE_KEEPALIVE` | 4 | C→S: `msg_header` + counter (8B) + 포스처 (8B) + mac (32B) / S→C: `msg_header`만 |
| `MSG_TYPE_CHALLENGE` | 5 | `msg_header` + nonce (16B) |
| `MSG_TYPE_AUTH` | 6 | `msg_header` + 터널 IP (4B) + 기기 ID (16B) + nonce (16B) + 포스처 (8B) + mac (32B) |
| `MSG_TYPE_DENY` | 7 | `msg_header` + 사유 (1B) |

포스처 보고는 8바이트 고정입니다: `os_build`(4B, BE) + `firewall`(1B) + `antivirus`(1B) + 예약(2B).
상태 값은 0 = 모름, 1 = 꺼짐, 2 = 켜짐이며, 채우지 않은 필드가 통과로 읽히지 않도록 0을 "모름"으로 뒀습니다.

- 헤더 크기는 `_Static_assert`로 컴파일 타임에 8바이트임을 보장합니다.
- 정수 필드는 `msg_encode` / `data_encode`에서 네트워크 바이트 순서로 변환합니다.

### Handshake

```mermaid
sequenceDiagram
    participant C as Client
    participant S as Server
    C->>S: REQ_HANDSHAKE (tun_ip, device_id)
    Note over S: 등록된 기기인가<br/>nonce 발급 (1회용, 5초)
    S->>C: CHALLENGE (nonce)
    Note over C: 포스처 수집<br/>mac = HMAC(기기 키, tun_ip|id|nonce|posture)
    C->>S: AUTH (tun_ip, device_id, nonce, posture, mac)
    Note over S: nonce / mac → 터널 IP → 포스처 정책<br/>통과하면 peer_register()
    S->>C: RES_HANDSHAKE (idx=0x5F3A9C02)
    Note over C: session.idx 저장
    C->>S: DATA (idx=0x5F3A9C02, counter=0, IP...)
    S->>C: DATA (idx=0x5F3A9C02, counter=0, IP...)
```

어느 단계에서든 서버는 `RES_HANDSHAKE` 대신 `DENY`(사유)로 답할 수 있습니다.

| 사유 | 값 | 뜻 |
|---|---|---|
| `DENY_UNKNOWN_DEVICE` | 1 | 기기 목록에 없는 ID |
| `DENY_BAD_AUTH` | 2 | nonce가 다르거나 만료됐거나 mac이 틀림 |
| `DENY_TUN_IP` | 3 | 그 기기에 배정된 터널 IP가 아님 |
| `DENY_POSTURE` | 4 | 인증은 됐지만 포스처가 그 기기의 정책에 못 미침 |

- **챌린지-응답**이라 키 자체는 와이어에 나오지 않습니다. nonce는 서명이 맞는 `AUTH`를 받는 순간 버리므로(뒤의 검사에서 거절되더라도) 엿들은 `AUTH`를 다시 보내도 통과하지 못합니다.
- **확인되지 않은 패킷은 진행 중인 핸드셰이크를 건드리지 못합니다.** 살아 있는 nonce가 있는 동안 다시 온 `REQ`에는 같은 nonce를 내주고, 서명이 틀린 `AUTH`는 nonce를 버리지 않습니다. 기기 ID만 아는 제3자가 `REQ`나 엉터리 `AUTH`를 계속 보내 진짜 기기의 핸드셰이크를 깨뜨릴 수 없습니다.
- **포스처는 mac의 서명 범위 안**이라 전달 도중에 "방화벽 꺼짐"을 "켜짐"으로 고칠 수 없습니다.
- **검사 순서**는 기기 → nonce / mac → 터널 IP → 포스처입니다. 키를 가졌다는 것을 확인하기 전에는 구체적인 거절 이유를 알려 주지 않습니다.
- 클라이언트는 `AUTH`를 보내기 전에 온 `RES_HANDSHAKE`를 받지 않습니다. 받아 주면 서버인 척하는 쪽이 `REQ`에 곧바로 `RES`를 돌려주는 것만으로 인증 단계가 건너뛰어집니다.
- 클라이언트는 1초 타임아웃으로 최대 5회 `REQ`부터 다시 하며, 서버 주소에서 온 응답만 받습니다. `DENY`를 받으면 재시도하지 않습니다.
- 새로 인증하면 새 세션입니다. 서버는 그 터널 IP의 기존 세션을 내리고 새 idx를 발급합니다.
- HMAC-SHA256은 직접 구현하지 않았습니다. 서버는 OpenSSL, Windows 클라이언트는 CNG를 쓰고, 양쪽 모두 `selftest`로 RFC 4231 벡터를 확인합니다.

> **Linux 클라이언트(`client/`)는 인증 이전의 프로토콜에 머물러 있습니다.** 예전 형식의 `REQ_HANDSHAKE`를 보내므로 지금 서버에는 붙지 못하며, 더 발전시킬 계획이 없습니다. 이 프로젝트의 클라이언트는 Windows 쪽이고, 인증 핸드셰이크는 Windows 클라이언트와 `test/auth_check.py`로 확인됩니다.

### Keepalive & session lifetime

```mermaid
sequenceDiagram
    participant C as Client
    participant S as Server
    Note over C: 10초마다, 포스처가 바뀌면 즉시
    C->>S: KEEPALIVE (idx, counter, posture, mac)
    Note over S: mac / counter 검증<br/>포스처를 정책에 다시 대 봄
    S->>C: KEEPALIVE (idx)
    Note over C: last_rx 갱신
    Note over S: 포스처가 못 미치면
    S->>C: DENY (posture) — 세션을 내린다
    Note over S: 60초간 수신 없음 → 세션 만료
    Note over C: 30초간 수신 없음 → 재핸드셰이크
    C->>S: REQ_HANDSHAKE
    S->>C: RES_HANDSHAKE (새 idx)
```

| 상수 (`proto.h`) | 값 | 의미 |
|---|---|---|
| `KEEPALIVE_INTERVAL_SEC` | 10 | 클라이언트의 keepalive(포스처 보고) 주기 |
| `SERVER_TIMEOUT_SEC` | 30 | 클라이언트가 이 시간 동안 서버에서 아무것도 받지 못하면 재핸드셰이크 |
| `SESSION_TIMEOUT_SEC` | 60 | 서버가 이 시간 동안 수신이 없는 세션을 만료 |

- keepalive는 NAT 매핑 유지, 생존 확인, **포스처 재보고**를 겸합니다. 핸드셰이크에서 한 번 통과했다고 세션이 끝날 때까지 믿지 않습니다.
- 클라이언트의 keepalive는 mac이 헤더부터 포스처까지를 덮습니다. 헤더의 `session_idx`가 들어가 다른 세션의 keepalive를 끼워 넣을 수 없고, `counter`가 직전 값보다 커야 해서 방화벽이 켜져 있던 때의 keepalive를 녹화했다가 다시 보낼 수 없습니다.
- 서버는 **버리는 것과 끊는 것을 구분**합니다. mac이 틀렸거나 counter가 예전 값이면 조용히 버리고 세션은 그대로 둡니다 (위조 패킷 하나로 남의 세션을 끊을 수 있어서는 안 됩니다). mac이 맞는데 포스처가 못 미치면 `DENY`로 알리고 세션을 내립니다.
- 거절된 클라이언트는 터널을 닫지 않고 통과만 막은 채, 포스처가 바뀌면 바로, 그렇지 않아도 30초마다 다시 핸드셰이크합니다.
- `kill -HUP`으로 서버가 기기 목록을 다시 읽으면, 조건이 강해졌거나 목록에서 지워진 기기는 다음 keepalive에서 걸러집니다.
- 서버는 세션이 마지막으로 보고한 포스처를 기억해 두고, 포스처 조건이 걸린 ACL 규칙을 그 값으로 판정합니다. 기기 정책은 통과하지만 규칙의 조건에는 못 미치는 경우 세션은 유지되고 그 목적지만 닫힙니다.
- 서버가 재시작했거나 세션이 만료된 경우 서버는 옛 idx의 패킷을 버리고, 클라이언트는 응답이 끊긴 것을 감지해 새 세션을 받습니다.
- 재핸드셰이크로 idx가 바뀌면 클라이언트는 `tx_counter`를 0으로 되돌립니다. idx가 그대로면 세션이 이어지는 것이므로 유지합니다.

### Session index

```
 31                                   8 7          0
 +-------------------------------------+------------+
 |        random (getrandom, 24bit)    | slot (8bit)|
 +-------------------------------------+------------+
```

- 하위 8비트가 피어 테이블 slot이므로 서버는 **배열 인덱싱 한 번**으로 세션을 찾습니다.
- 상위 24비트 난수와 전체 값 비교로, 같은 slot을 재사용해도 이전 세션 idx는 무효가 됩니다.
- `MAX_PEERS <= 2^8`은 `_Static_assert`로 강제합니다.

---

## Packet flow

### 송신: TUN → UDP

TUN에서 읽을 때 버퍼 앞 16바이트를 **비워두고** 읽은 뒤, 그 자리에 헤더만 채워 그대로 전송합니다.

```
read(tun, buf + 16)     [ ........ 16B ........ ][ IP packet (n) ]
msg_encode(buf)         [ msg_hdr ][ ... 8B ... ][ IP packet (n) ]
data_encode(buf + 8)    [ msg_hdr ][ data_hdr  ][ IP packet (n) ]
sendto(buf, 16 + n)     └──────────── 한 번에 전송 ─────────────┘
```

- 서버는 inner IP의 **목적지 주소**로 피어를 찾아 해당 피어의 외부 주소로 보냅니다.

### 수신: UDP → TUN

```
recvfrom(buf)           [ msg_hdr ][ data_hdr  ][ IP packet ]
msg_decode   → off = 8      │ version / type / session_idx 검증
data_decode  → off = 16                │ counter
write(tun, buf + 16, n - 16)                       └─ 커널로 전달
```

서버 측 검증 순서:

1. 헤더 길이 / 프로토콜 버전
2. 핸드셰이크 요청이면 `handle_handshake`로 분기
3. `session_idx`로 피어 조회 (`peer_find_idx`)
4. 메시지 타입 분기 (keepalive → `last_seen` 갱신 후 응답)
5. `data_header` 및 IPv4 형식 검사
6. inner src가 피어의 터널 IP와 같은지 검사
7. ACL 정책 검사 (활성화 시)
8. 모든 검사를 통과한 뒤에만 피어의 외부 주소와 `last_seen` 갱신 (`peer_touch`)

클라이언트 측 검증 순서:

1. 출발지 주소/포트가 서버와 같은지
2. 헤더 길이 / 프로토콜 버전
3. `session_idx`가 자신의 세션과 같은지
4. 메시지 타입 분기, `data_header` 및 IPv4 형식 검사

---

## Project structure

```
my_vpn/
├── lib/common/            # 클라이언트/서버 공용
│   ├── include/proto.h    #   와이어 헤더 정의, IP 오프셋 매크로 (Linux / Windows 공용)
│   ├── include/common.h   #   Linux 전용 선언 (proto.h 포함)
│   └── src/
│       ├── proto.c        #   encode/decode (Linux / Windows 공용)
│       └── common.c       #   tun_alloc, hex_dump
├── client/
│   └── src/client.c       # 핸드셰이크, keepalive, 재핸드셰이크 + epoll 터널 루프
├── server/
│   ├── include/{peer,acl,device,auth,policy,status}.h
│   └── src/
│       ├── server.c       # epoll 루프, 인증 핸드셰이크 / keepalive 검증, 패킷 검증
│       ├── peer.c         # 피어 테이블, 세션 idx 발급/조회, 세션 만료
│       ├── device.c       # 등록된 기기 목록, 기기별 포스처 정책
│       ├── auth.c         # HMAC-SHA256 (OpenSSL), 상수 시간 비교, 난수
│       ├── policy.c       # 포스처 조건의 문법과 판정 (기기 목록과 ACL이 함께 쓴다)
│       ├── status.c       # 상태 소켓 — 세션과 카운터를 JSON으로
│       └── alc.c          # ACL 파싱 및 매칭, 규칙별 포스처 조건
├── windows/               # Windows 클라이언트 (Visual Studio 솔루션)
│   ├── README.md          #   Windows 클라이언트, GUI, 패킷 분석기
│   ├── my_vpn.sln
│   ├── core/              #   UI 프레임워크에 의존하지 않는 순수 Win32/C
│   │   ├── tunnel.{h,c}       # Wintun + Winsock 터널 루프 (블로킹 tunnel_run)
│   │   ├── posture.{h,c}      # 단말 포스처 수집 (OS 빌드 / 방화벽 / 백신)
│   │   ├── device.{h,c}       # 기기 ID · 키 생성, DPAPI 보관
│   │   ├── hmac.{h,c}         # HMAC-SHA256 (CNG)
│   │   └── vpn_session.{h,c}  # 워커 스레드 + 패킷 링버퍼 + PostMessage 마셜링
│   ├── cli/               #   콘솔 클라이언트 (main.c) — 자동 테스트 대상
│   ├── gui/               #   MFC 다이얼로그
│   │   ├── MainDlg.{h,cpp}      # 가상 리스트, 상세창, 포스처 탭, 레이아웃, 바탕 그리기
│   │   ├── EnrollDlg.{h,cpp}    # 기기 등록 창 (ID · 키 1회 표시)
│   │   ├── ui_theme.{h,cpp}     # 색 · 글꼴 · 둥근 사각형 (순수 Win32 + GDI+)
│   │   ├── ui_controls.{h,cpp}  # 직접 그리는 버튼과 탭 줄 (순수 Win32)
│   │   ├── PacketStore.{h,cpp}  # 패킷 보관 / 필터 / 집계
│   │   └── vpn_gui.rc
│   └── test/
│       ├── client.ps1         # Windows 시나리오 테스트 (run / idle / restart)
│       └── mock_server.py     # 인증 핸드셰이크 모의 서버 (서버 구현 전 확인용)
├── test/
│   ├── common.sh          # 공용 설정 및 함수
│   ├── server.sh          # 서버 실행 / 재시작 시나리오
│   ├── client.sh          # 클라이언트 실행 / idle·stop·restart 시나리오
│   ├── auth_check.py      # 서버의 기기 인증 / 포스처 / 지속 검증을 패킷 수준에서 확인
│   └── devices.conf.example  # 기기 목록 형식 (실제 devices.conf 는 커밋하지 않는다)
├── mcp/                   # Linux 서버를 제어하는 MCP 서버 (Node.js)
│   ├── server.js          #   MCP(stdio) — 도구 선언, 권한 없음
│   ├── ctl.cjs            #   root 헬퍼 — 서비스 제어, 기기 목록 / ACL 편집, 로그에서 세션 계산
│   ├── my_vpn.service     #   systemd 유닛
│   ├── service.sh         #   유닛이 부르는 실행 / tun 설정 스크립트
│   └── install.sh, uninstall.sh
├── .github/workflows/ci.yml  # 푸시마다 서버 빌드 + 인증 검사, Windows 클라이언트 MSVC 빌드
├── docs/
│   ├── linux.md           # Linux 빌드·실행·테스트, ACL
│   └── mcp.md             # MCP 서버의 구조, 설치, 도구
└── Makefile               # 컴포넌트별 bin/ 분리 빌드, 의존성 자동 추적
```

---

## Security model

현재 단계에서 무엇을 막고, 무엇을 아직 막지 못하는지 명시합니다.

| 위협 | 상태 | 비고 |
|---|---|---|
| 등록되지 않은 클라이언트의 데이터 주입 | 방어 | 유효한 `session_idx` 필요 |
| 오래된 세션 idx 재사용 | 방어 | slot 재사용 시 난수 부분 변경 |
| 정책 외 목적지 접근 | 방어 | ACL default deny |
| 세션 내 inner src IP 위조 | 방어 | inner src가 피어의 터널 IP와 다르면 폐기 |
| 클라이언트의 비서버 출발지 패킷 수신 | 방어 | 출발지 주소/포트가 서버와 다르면 폐기 |
| 죽은 세션이 테이블을 계속 점유 | 방어 | 60초 무응답 세션 만료 |
| 핸드셰이크 위장 (세션 탈취) | 방어 | 등록된 기기의 키로 서버의 nonce에 서명해야 세션을 받음 |
| 엿들은 `AUTH` 재전송 | 방어 | nonce는 1회용이고 5초 뒤 만료 |
| 전달 중 포스처 변조 | 방어 | 포스처가 mac의 서명 범위 안 |
| 예전 포스처 보고 재전송 | 방어 | keepalive의 `counter`가 직전 값보다 커야 하고, mac이 `session_idx`에 묶임 |
| 위조 keepalive로 남의 세션 끊기 / 주소 돌리기 | 방어 | mac 검증을 통과한 뒤에만 세션을 내리거나 외부 주소를 갱신 |
| 정책에 못 미치는 단말의 접근 | 방어 | 핸드셰이크와 매 keepalive에서 기기별 포스처 정책 평가 |
| 분실 / 폐기된 기기의 접근 | 방어 | 목록에서 지우고 `kill -HUP` — 다음 keepalive에서 차단 |
| 들어온 뒤 포스처가 나빠진 단말의 민감 리소스 접근 | 방어 | ACL 규칙별 포스처 조건 — 마지막으로 보고한 포스처로 패킷마다 판정 |
| 핸드셰이크 방해 | 방어 | 살아 있는 nonce는 다시 온 `REQ`에 덮어쓰지 않고, 서명이 틀린 `AUTH`는 nonce를 버리지 않음. 경로 위에서 패킷을 지우는 공격자는 여전히 막을 수 있음 |
| 도청 / idx 탈취 | **미방어** | 데이터 패킷은 평문이고 인증되지 않음 — idx를 아는 공격자는 패킷 주입과 외부 주소 변경이 가능. 암호화 단계에서 해결 |
| 데이터 패킷 재전송 | **미방어** | 데이터의 `counter`는 송신만 하고 수신 측 검증 없음 |
| 서버 위장 | **미방어** | 서버는 클라이언트에게 자신을 증명하지 않음. 서버 주소를 위조할 수 있으면 가짜 `RES` / `DENY`를 보낼 수 있음 |
| 포스처 거짓 보고 | **미방어** | 포스처는 클라이언트의 자기 보고. 키를 가진 변조된 클라이언트는 거짓으로 보고할 수 있음 (막으려면 TPM 기반 원격 증명 필요) |

> 학습 목적의 프로젝트이며, 프로덕션 환경에서 사용하기 위한 보안 수준이 아닙니다.

### Known limitations

- **재핸드셰이크는 블로킹입니다.** 메인 루프 안에서 응답을 최대 5초 기다리며, 그동안 터널 패킷은 처리되지 않습니다. 실패하면 30초 뒤에 다시 시도합니다.
- **종료 통지가 없습니다.** 클라이언트가 종료해도 서버는 알지 못하고, 세션은 60초 뒤 만료로 정리됩니다.
- **Linux 클라이언트의 재핸드셰이크 판정은 벽시계(`time()`) 기준입니다.** 시스템 시간이 크게 바뀌면 재핸드셰이크가 일찍 일어날 수 있습니다. 서버의 세션 만료와 nonce 수명, Windows 클라이언트는 단조 시계를 씁니다.
- **ACL이 막은 패킷은 클라이언트에게 알리지 않습니다.** 조용히 버리므로 사용자에게는 타임아웃으로 보입니다. 포스처 때문에 막힌 것인지 알려 주려면 와이어에 메시지가 하나 더 필요합니다.
- **ACL 규칙의 출발지는 터널 IP입니다.** 터널 IP는 기기에 묶여 있고 위조할 수 없어 기기를 가리키는 셈이지만, 여러 기기를 역할로 묶어 쓰는 방법은 없습니다.
- **서버의 카운터는 쌓아 두지 않습니다.** 상태 소켓의 숫자는 서버가 뜬 뒤부터 센 값이고 재시작하면 0으로 돌아갑니다.
- **Linux 클라이언트는 인증 이전의 프로토콜에 머물러 있고, 더 발전시키지 않습니다.** 지금 서버에 붙지 못하며 `test/client.sh`의 시나리오도 통과하지 못합니다. 초기 단계(터널, 핸드셰이크, keepalive, 재핸드셰이크)를 구현하고 검증한 기록으로 남겨 둡니다. 포스처 수집은 Windows 전용입니다.
- **기기 키는 서버에 평문으로 있습니다.** 기기 목록 파일의 권한(600)에 기대며, 서버가 뚫리면 모든 기기의 키가 드러납니다. 공개키 방식이면 서버에는 공개키만 두면 됩니다.
- **IPv4만 터널링합니다.** IPv6 패킷은 건너뜁니다.
- **멀티캐스트 / 브로드캐스트는 터널을 통과하지 못합니다.** 서버가 inner dst로 피어를 1:1로 찾는 구조라
  전달할 상대가 없습니다. Windows 클라이언트는 이를 송신 경로에서 명시적으로 버립니다.

---

## Roadmap

- [x] TUN 디바이스 + UDP 기본 터널
- [x] 와이어 헤더 설계 및 encode/decode
- [x] 핸드셰이크와 세션 idx 기반 피어 관리
- [x] ACL (default deny)
- [x] 위조 방어 — inner src 검증, 검증 후 NAT 외부 주소 갱신, 클라이언트 출발지 확인
- [x] Keepalive 및 세션 만료
- [x] 클라이언트 재핸드셰이크 (서버 재시작 / 세션 만료 복구)
- [x] 시나리오 테스트 스크립트
- [ ] 암호화 & 키 교환 — X25519 + ChaCha20-Poly1305, `counter`를 nonce 및 재전송 방지에 사용
- [x] 신원 기반 인증 — 기기 키 챌린지-응답, 포스처 기반 정책 (ZTNA)
  - [x] 와이어 포맷 정의 (`CHALLENGE` / `AUTH` / `DENY`, 포스처 보고)
  - [x] Windows 클라이언트 — 포스처 수집, 기기 등록, 인증 핸드셰이크
  - [x] 서버 — 기기 목록, nonce 발급과 mac 검증, 기기별 포스처 정책 (`test/auth_check.py` 53개 통과, 실제 Windows 클라이언트로 수락 / 거절 확인)
  - [x] 지속 검증 (Windows 클라이언트) — keepalive 로 포스처 재보고, 세션 중 거절 처리와 복구 (모의 서버로 확인)
  - [x] 지속 검증 (서버) — keepalive 의 mac / counter 검증, 위반 시 `DENY` 와 세션 차단, `SIGHUP` 으로 정책 변경 / 기기 폐기
  - [x] Windows GUI — 포스처 탭, 거부 사유 표시, 기기 등록 (모의 서버로 허용 / 세션 중 거부 / 복구 확인)
  - [x] Windows GUI와 실제 서버의 전체 경로 — 터널을 열고 ping 왕복, `SIGHUP`으로 정책을 조이면 차단, 풀면 복구
- [x] 리소스별 포스처 조건 — ACL 규칙에 포스처 조건을 붙여 접근 대상마다 요구 수준을 다르게
- [x] 서버 운영 — 핸드셰이크 방해 방어, 단조 시계 세션 만료, `SIGHUP`으로 ACL 다시 읽기, 읽기 전용 상태 소켓 (`test/auth_check.py` 101개 통과)
- [x] MCP 서버 — AI 클라이언트가 Linux 서버를 제어 (systemd 서비스 + root 헬퍼)
- [ ] 기기 그룹 — ACL 규칙의 출발지를 터널 IP가 아니라 역할로 지정
- [ ] 세션 최대 수명 — 오래 열린 세션에 재인증 요구
- [ ] ACL 거부 통지 — 포스처 때문에 막힌 것을 클라이언트가 사용자에게 알릴 수 있게
- [ ] 비블로킹 재핸드셰이크 (상태 머신), 종료 통지 메시지
- [x] Windows 클라이언트 — Wintun 콘솔 클라이언트 (ping 왕복 / 유휴 120초 / 서버 재시작 복구 확인)
- [x] Windows 클라이언트 — MFC GUI (연결·해제 상태 전이 / 패킷 분석 탭 / 필터·통계 / ping 왕복 확인)
- [x] CI — 푸시마다 서버 빌드와 `test/auth_check.py`, Windows 클라이언트(콘솔 + MFC GUI) MSVC 빌드
- [ ] Windows 클라이언트 — Windows 서비스 + Named Pipe IPC

---

## AI 사용 내역

이 프로젝트는 학습이 목적이므로, AI(Claude Code)를 어디에 썼는지 구분해서 밝힙니다.

| 구분 | 내용 |
|---|---|
| 직접 구현 | 와이어 프로토콜, 핸드셰이크, 피어 테이블, 위조 방어, keepalive, 세션 만료, 재핸드셰이크 등 클라이언트/서버 기능 로직 |
| AI 리뷰 | 위 구현의 diff를 AI에게 검토받고, 지적받은 버그는 직접 수정 |
| AI 작성 | `test/` 테스트 스크립트 전체 |
| AI 작성 (일부) | `client.c`의 재핸드셰이크 로그, `do_handshake`의 `SO_RCVTIMEO` 원복과 `tx_counter` 리셋 |
| AI 작성 | `windows/` Windows 클라이언트 전체 — `core/tunnel.c`(Wintun 터널 루프), `core/vpn_session.c`(워커 스레드/메시지 마셜링), `cli/`(콘솔), `gui/`(MFC 다이얼로그), Visual Studio 프로젝트 파일. Linux 클라이언트의 프로토콜 동작을 기준으로 작성 |
| AI 작성 | `proto.h` / `proto.c` 분리 (기존 코드 이동) |
| AI 분석 | Windows 유휴 시 비유니캐스트 송신이 keepalive를 억제해 불필요한 재핸드셰이크가 생기는 문제를 테스트로 재현하고 `windows/` 쪽을 수정 |
| AI 초안 | 이 README의 keepalive/재핸드셰이크 이후 갱신분 |
| AI 작성 | ZTNA 확장의 Windows 쪽 — `core/posture.c`(포스처 수집), `core/device.c`(기기 신원), `core/hmac.c`, `core/tunnel.c`의 인증 핸드셰이크와 지속 검증, GUI의 포스처 탭과 기기 등록 창, `windows/test/mock_server.py` |
| AI 작성 | ZTNA 확장의 서버 쪽 — `server/src/auth.c`, `server/src/device.c`, `server.c`의 `handle_request` / `handle_auth` / `handle_keepalive` / `send_deny`와 SIGHUP 처리, `peer.c`의 `peer_drop`, `test/auth_check.py`. 목표 직무가 Windows 클라이언트라 서버 쪽 확장은 AI에게 맡겼고, 기존 서버 로직(피어 테이블, 데이터 경로, ACL)은 그대로 직접 구현한 것 |
| AI 작성 | `proto.h` / `proto.c`의 인증·포스처 메시지 정의와 `posture_encode` / `posture_decode`. 메시지 흐름은 AI가 제안하고 검토 후 채택 |
| AI 작성 | 서버의 ZTNA 확장 (2차) — `server/src/policy.c`, `server/src/status.c`, `alc.c`의 다시 읽기(임시 표)와 규칙별 포스처 조건 / 줄 형식 검사, `server.c`의 nonce 재사용과 상태 소켓 연결 / 카운터, `peer.c`의 단조 시계와 세션별 카운터. ACL의 매칭 로직과 피어 테이블의 구조는 직접 구현한 것 그대로 |
| AI 작성 | `mcp/` MCP 서버 전체 — `server.js`(도구 선언), `ctl.cjs`(root 헬퍼), systemd 유닛, 설치 스크립트, `docs/mcp.md` |
| AI 작성 | GUI 재설계 — `gui/ui_theme.cpp`, `gui/ui_controls.cpp`, `MainDlg`의 배치와 그리기, `EnrollDlg`의 그리기, `vpn_gui.rc`, 앱 아이콘(`windows/tools/mkicon.py`). Linux 장비에서 작성했고 MSVC 빌드는 CI로 확인, 실행은 아직 확인하지 못함 |
| AI 작성 | `.github/workflows/ci.yml`, CI가 찾은 `posture.c`의 `wscapi.dll` 동적 로드 |
