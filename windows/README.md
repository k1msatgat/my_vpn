# Windows 클라이언트

와이어 프로토콜과 설계는 [루트 README](../README.md)에 있습니다. 이 문서는 Windows 전용 구현을 다룹니다.

---

`windows/`에 같은 와이어 프로토콜을 쓰는 Windows용 클라이언트가 있습니다. 서버는 Linux 그대로이며,
프로토콜 코드(`lib/common`의 `proto.h` / `proto.c`)는 Linux와 Windows가 같은 소스를 빌드합니다.

| | Linux (`client/`) | Windows (`windows/`) |
|---|---|---|
| 가상 인터페이스 | `/dev/net/tun` fd, `read` / `write` | [Wintun](https://www.wintun.net/) 링버퍼, `WintunReceivePacket` / `WintunSendPacket` |
| 이벤트 루프 | `epoll_wait` | `WaitForMultipleObjects` (종료 이벤트 + Wintun read 이벤트 + `WSAEventSelect` 소켓 이벤트) |
| 핸드셰이크 대기 | `SO_RCVTIMEO` 블로킹 `recvfrom` | 같은 이벤트 대기 — 대기 중에도 종료 요청에 바로 반응 |
| 종료 | `SIGINT` / `SIGTERM` | manual-reset 이벤트 (콘솔은 Ctrl+C, GUI는 해제 버튼) |
| 시간 | `clock_gettime`, `time` | `GetTickCount64` |
| IP / MTU 설정 | `ip addr add`, `ip link set mtu` | 프로그램이 직접 설정 (`CreateUnicastIpAddressEntry`, `SetIpInterfaceEntry`) |
| keepalive 트리거 | 송신 유휴 10초 | 송신 유휴 10초 **또는** 수신 유휴 10초 (아래 참고) |
| 터널에 넣지 않는 패킷 | — (tun0이 조용함) | 멀티캐스트 / 브로드캐스트 목적지 |

- 터널 로직은 `core/tunnel.c`의 블로킹 함수 `tunnel_run()` 하나이고, 로그와 상태 변화는 콜백으로 알립니다.
  콘솔 클라이언트는 이를 메인 스레드에서, GUI는 `core/vpn_session.c`를 통해 워커 스레드에서 호출합니다.
- Windows의 UDP 소켓은 ICMP port unreachable을 받으면 다음 `recvfrom`이 `WSAECONNRESET`으로 실패합니다.
  서버가 내려가 있는 동안에도 재핸드셰이크를 계속 시도해야 하므로 `SIO_UDP_CONNRESET`으로 이 동작을 끕니다.
- Wintun은 패킷을 링버퍼 안에서 직접 넘겨주므로 Linux의 헤드룸 방식을 쓸 수 없어, 송신 경로에서 한 번 복사합니다.

## Windows는 터널 어댑터에도 계속 말을 건다

Linux의 `tun0`은 아무도 쓰지 않으면 조용하지만, Windows는 어댑터가 올라오는 순간부터 그 인터페이스로
mDNS / LLMNR / SSDP / IGMP를 꾸준히 내보냅니다. 서버는 inner dst로 피어를 1:1로 찾으므로
멀티캐스트·브로드캐스트는 애초에 전달할 상대가 없는데, 이를 그대로 터널에 넣으면

1. 서버는 받아서 자기 tun에 쓰고 커널이 버리므로 **응답이 돌아오지 않고**,
2. 송신이 10초 안에 계속 일어나 `last_tx` 기준 keepalive가 **영구히 억제**되어,
3. 수신이 끊긴 것으로 판정되어 서버가 살아 있는데도 **30초마다 재핸드셰이크**가 돕니다.

실제로 120초 유휴 테스트에서 재현했습니다. 두 군데를 고쳤습니다.

- 송신 경로에서 목적지가 멀티캐스트(`224.0.0.0/4` 이상)이거나 터널 대역의 브로드캐스트면 버립니다.
  서버가 버릴 패킷을 보내지 않으니 대역과 서버 처리도 함께 아낍니다.
- keepalive가 맡은 두 가지를 분리했습니다. **NAT 매핑 유지**는 송신 유휴 10초 기준 그대로 두고,
  **생존 확인**은 "서버가 10초간 조용하면 보낸다"를 따로 둡니다. 데이터 패킷은 서버가 버릴 수 있어
  응답을 보장하지 못하지만 keepalive는 서버가 반드시 되돌려주므로, 응답 없는 송신이 이어져도
  재핸드셰이크 전에 생존 여부를 확인할 수 있습니다.

`windows/test/client.ps1 idle`의 120초 유휴 구간 실측입니다.

| | 수정 전 | 수정 후 |
|---|---|---|
| 재핸드셰이크 | 1회 | **0회** |
| keepalive 송신 / 수신 | 0 / 0 (억제됨) | **11 / 11** |
| `tun->udp` | 58 (대부분 전달 불가) | **6** (ping 왕복만) |
| `udp->tun` | 0 | **6** |
| 비유니캐스트 폐기 | — | 73 |

## GUI (MFC)

`gui/`는 `core/vpn_session.c` 위에 올린 MFC 다이얼로그입니다. `tunnel_run()`이 블로킹 함수이므로
UI 스레드에서 부를 수 없고, 워커 스레드에서 돌리면서 스레드 경계를 넘겨야 합니다.

| 문제 | 처리 |
|---|---|
| 콜백이 워커 스레드에서 불린다 | 컨트롤을 직접 건드리지 않고 `PostMessage`로 UI 스레드에 넘김 |
| `SendMessage`를 쓰면? | UI 스레드가 워커 종료를 기다리는 중이면 서로를 기다리는 교착. `PostMessage`는 큐에 넣고 바로 복귀 |
| 로그 문자열 수명 | 워커가 힙에 복사해 소유권을 메시지에 실어 넘기고, UI가 처리 후 해제. `PostMessage` 실패 시 보낸 쪽이 해제 |
| 창을 닫을 때 | 종료 요청 → 워커 join → **그 다음** 큐에 남은 로그 버퍼 해제 (순서가 바뀌면 샘) |
| 해제 버튼 | 요청만 하고 기다리지 않음. 워커가 끝나면 오는 `WM_VPN_DONE`에서 join (UI가 멈추지 않음) |
| 강제 종료 | `TerminateThread`를 쓰지 않음. 정리 구간이 돌지 못하면 어댑터와 터널 IP가 남음 |

## 패킷 분석 화면

터널이 무엇을 주고받는지 텍스트 로그로만 보면 알 수 없습니다. 그런데 원인은 UI가 아니라
**인터페이스**였습니다. 콜백이 `on_log(const char *line)` 하나뿐이어서, `tunnel.c`가 이미 디코드해
알고 있는 세션 idx·counter·inner src/dst를 **문자열로 만들어 버린 뒤** 넘겼고 UI는 그 정보를
되찾을 수 없었습니다.

그래서 구조화된 이벤트를 따로 올립니다.

```c
void (*on_packet)(void *ctx, const tunnel_packet_t *pkt);
```

`tunnel_packet_t`는 방향·시각·메시지 타입·세션 idx·counter·와이어 길이·inner src/dst·프로토콜·
L4 식별 정보(ICMP type/code 또는 포트)·앞 64바이트 스냅샷을 담습니다. 검증 단계마다 `verdict`만
바꿔 같은 이벤트를 내보내므로 **폐기된 패킷도 이유와 함께** 올라옵니다.

| `verdict` | 언제 |
|---|---|
| `PASS` | 전달됨 |
| `NON_UNICAST` | 멀티캐스트 / 브로드캐스트 — 전달할 피어가 없다 |
| `NOT_SERVER` | 출발지가 서버가 아니다 |
| `WRONG_SESSION` | `session_idx`가 내 세션과 다르다 |
| `BAD_HEADER` | 길이 부족 / 버전 불일치 |
| `UNKNOWN_TYPE`, `NON_IPV4`, `TOO_LONG`, `IO_ERROR` | 그 외 |

코어는 **표시 문자열을 갖지 않습니다.** `tunnel_verdict_name()`이 한국어를 돌려주면 `char*`라서
UI가 ANSI 코드페이지로 변환하며 깨집니다. 영문만 돌려주고 화면 라벨은 UI가 enum을 보고 와이드
문자열로 고릅니다.

### 워커와 UI 사이 — 알림 합치기

패킷마다 `PostMessage`하면 초당 수천 건에서 메시지 큐가 넘칩니다. `vpn_session`이 `CRITICAL_SECTION`
으로 보호한 링버퍼(4096)에 쌓고, **처리되지 않은 알림이 없을 때만** 한 번 깨웁니다.

```c
if (InterlockedExchange(&s->notify_pending, 1) == 0) {
        PostMessageW(s->notify, WM_VPN_PACKETS, 0, 0);
}
```

패킷 수와 무관하게 큐에 떠 있는 알림은 최대 하나입니다. 꺼내는 쪽은 **비우기 전에** 플래그를
내립니다 — 꺼내는 중 들어온 패킷이 새 알림을 보낼 수 있어야 하고, 순서를 바꾸면 그 깨우기가
억제되어 링에 묻힙니다. 링이 꽉 차면 오래된 것부터 버리고 개수를 세어 UI가 유실을 표시합니다.

### 화면

| 영역 | 내용 |
|---|---|
| 패킷 리스트 | `#`, 시간, 방향, 타입, 세션, counter, 요약, 길이, 결과 |
| 상세창 | `msg_header` / `data_header`를 필드별로 되짚고 inner IPv4·L4 요약과 hex 덤프 |
| 필터 | 송신 / 수신, `keepalive 숨김`, `폐기만` |
| 통계 | 송수신 패킷·바이트, keepalive 왕복, 폐기 이유별, 재핸드셰이크, UI 유실 |

MFC 래퍼만 얹지 않고 Win32 수준에서 처리한 부분입니다.

| 기법 | 왜 |
|---|---|
| `LVS_OWNERDATA` + `LVN_GETDISPINFO` | 행을 컨트롤에 복사하지 않고 그릴 때마다 물어본다. 2만 행이어도 컨트롤 쪽 메모리와 삽입 비용이 늘지 않는다 |
| `NM_CUSTOMDRAW` | 송신 / 수신 / 폐기 행 색 구분 |
| `LVS_EX_DOUBLEBUFFER` | 빠르게 추가될 때의 깜빡임 제거 |
| `BeginDeferWindowPos` | 리사이즈 때 컨트롤을 한 번에 이동 — 중간 상태가 그려지지 않는다 |
| `MapDialogRect` | 레이아웃 상수를 다이얼로그 단위로 쓰고 픽셀로 변환. 고정 픽셀은 고DPI에서 잘린다 |
| `WM_GETMINMAXINFO` | 최소 창 크기 |
| `WM_CTLCOLOR` | 상태 텍스트를 상태별 색으로 |
| 매니페스트 `Common-Controls v6` | 없으면 컨트롤이 비주얼 스타일 없이 Win95 모양으로 그려진다 |

- 연결 설정은 `HKCU\Software\my_vpn`에 저장해 다음 실행에 복원합니다.
- CSV 저장은 UTF-8 BOM을 앞에 써서 엑셀이 한글을 알아보게 합니다.

## 빌드와 실행

1. [wintun.net](https://www.wintun.net/)에서 Wintun zip을 받아 `windows/third_party/`에 풉니다.
   (`windows/third_party/wintun/include/wintun.h`, `windows/third_party/wintun/bin/amd64/wintun.dll`)
2. Visual Studio 2022 이상으로 `windows/my_vpn.sln`을 열어 x64로 빌드합니다. `wintun.dll`은 빌드 후 실행 파일 옆으로 복사됩니다.
   GUI(`vpn_gui`)는 Visual Studio 설치 관리자의 개별 구성 요소에서 **"최신 v143(또는 v145) 빌드 도구용 C++ MFC"** 가 필요합니다
   (없으면 `error MSB8041`). 콘솔 클라이언트(`vpn_client`)는 MFC 없이 빌드됩니다.
3. **관리자 권한**으로 실행합니다. 어댑터 생성과 IP / MTU 설정까지 프로그램이 합니다.
   GUI는 매니페스트에 `requireAdministrator`가 박혀 있어 실행할 때 Windows가 권한 상승을 먼저 묻습니다.

```
:: 콘솔
windows\build\Debug\vpn_client.exe <adapter-name> <server-ip> <port> <tunnel-ip>
:: 예) vpn_client.exe my_vpn 203.0.113.10 9000 10.0.0.3

:: GUI — 같은 값을 창에서 입력하고 [연결]
windows\build\Debug\vpn_gui.exe
```

- 터널 대역은 `/24`로 설정합니다. 온링크 라우트(`10.0.0.0/24`)는 주소를 붙이면 Windows가 직접 넣어주므로 따로 추가하지 않습니다.
- 플랫폼 툴셋은 하드코딩하지 않고 설치된 VS의 기본값(`$(DefaultPlatformToolset)` — VS2022는 v143, VS2026은 v145)을 따릅니다.
- 소스는 Linux와 함께 쓰므로 UTF-8(BOM 없음)입니다. MSVC는 기본적으로 시스템 코드페이지로 읽어 C4819가 나고
  한글 와이드 문자열 리터럴도 깨지므로 `/utf-8`로 컴파일합니다. `.rc`는 `#pragma code_page(65001)`로 같은 문제를 막습니다.
- 경로 기준은 `$(MSBuildProjectDirectory)`입니다. `$(SolutionDir)`은 `.sln`을 통해 빌드할 때만 정의되므로,
  `msbuild windows\cli\vpn_client.vcxproj`처럼 프로젝트를 직접 빌드해도 되도록 쓰지 않습니다.
- 서버에서 Windows 클라이언트로 ping을 보내려면 Windows 방화벽에서 ICMPv4 인바운드를 허용해야 합니다.

---

## 시나리오 테스트

Windows 클라이언트는 `windows/test/client.ps1`이 같은 시나리오를 돌립니다. 빌드 → 권한 상승(UAC) →
실행 → 판정까지 한 번에 하고, 결과는 `windows/test/logs/`에 남습니다.

| 시나리오 | 서버 (Linux) | 클라이언트 (Windows) |
|---|---|---|
| 기본 연결 | `test/server.sh` | `windows\test\client.ps1` |
| 유휴 120초 | `test/server.sh` | `windows\test\client.ps1 idle` |
| 서버 재시작 | `test/server.sh restart` | `windows\test\client.ps1 restart` |

```powershell
$env:SERVER_IP='203.0.113.10'; windows\test\client.ps1 idle -Sec 180
```

- 터널 IP 기본값은 `10.0.0.3`입니다 (`10.0.0.2`는 Linux 클라이언트가 씁니다).
- 유휴 시나리오는 재핸드셰이크가 없었는지만 보지 않고, keepalive가 실제로 나가고 **서버 응답이 돌아왔는지**까지 확인합니다.
- 종료는 Ctrl-C 경로(`GenerateConsoleCtrlEvent`)로 보내 어댑터와 터널 IP가 스스로 정리되는지도 함께 봅니다.
  Ctrl-C는 콘솔 그룹 전체에 가므로 스크립트 자신도 같이 죽을 수 있어, 판정 결과를 종료 **전에** 먼저 파일로 남깁니다.
- 스크립트는 UTF-8 **BOM**으로 저장합니다. Windows PowerShell 5.1은 BOM이 없으면 `.ps1`을 시스템
  ANSI 코드페이지로 읽어, 한글이 든 문자열 리터럴의 닫는 따옴표가 멀티바이트 문자에 먹혀 파싱이 깨집니다.

서버 재시작 복구는 양쪽 로그가 맞물리는 것까지 확인했습니다. 서버가 죽은 뒤에도 클라이언트의
UDP 소켓은 그대로이므로 외부 주소/포트가 유지되고, 바뀌는 것은 세션 idx뿐입니다.

```
서버   [peer] register 10.0.0.3 <- 115.21.23.172:53559 idx=6bd26700
       (kill -9 → 재시작)
서버   [drop] unknown session_idx[6bd26700]        ← 새 서버가 옛 세션을 거부
클라   [HS] no rx from server for 30s, re-handshake
서버   [peer] register 10.0.0.3 <- 115.21.23.172:53559 idx=2ae3b200
```
