# MCP 서버 (Linux 서버 제어)

AI 클라이언트가 이 장비의 `vpn_server`를 올리고 내리고, 기기와 ACL을 바꾸고, 상태와 로그를 볼 수 있게 하는
[MCP](https://modelcontextprotocol.io) 서버입니다. 제어 대상은 **Linux 서버뿐**입니다. Windows 클라이언트는
다른 장비에서 돌므로 여기서 직접 다루지 않고, 기기 목록의 정책을 바꾸거나 기기를 지워서 들이고 내보냅니다.

---

## 구조

```
AI 클라이언트 ──ssh──▶ my_vpn_mcp (mcp/server.js, 일반 계정)
                          │  sudo -n, 요청은 stdin 의 JSON
                          ▼
                       my_vpn_ctl (mcp/ctl.cjs, root)
                          │
          ┌───────────────┼──────────────────┐
     systemctl        /etc/my_vpn/        journalctl
   my_vpn.service   devices.conf, acl.conf   (로그 → 세션)
```

| 조각 | 설치 위치 | 하는 일 |
|---|---|---|
| `mcp/server.js` | `/opt/my_vpn/mcp/`, 실행 이름 `my_vpn_mcp` | MCP(stdio). 도구의 입력 형식만 선언하고 요청을 헬퍼로 넘긴다. 권한 없음 |
| `mcp/ctl.cjs` | `/usr/local/sbin/my_vpn_ctl` | root 헬퍼. 입력을 다시 검증하고 실제 일을 한다. npm 의존성 없음 |
| `mcp/my_vpn.service`, `mcp/service.sh` | `/etc/systemd/system/`, `/opt/my_vpn/libexec/` | `vpn_server` 실행, 서버가 만든 tun에 주소 부여 |
| 설정 | `/etc/my_vpn/` (root 전용) | `server.env`(포트, 터널 주소, MTU), `devices.conf`, `acl.conf` |

- **셸을 열어 주지 않습니다.** 할 수 있는 일은 아래 도구가 전부입니다. sudoers는 `my_vpn_ctl`을 인자 없이 부르는 것 하나만 허용합니다.
- **기기 키는 들어가기만 하고 나오지 않습니다.** `device_add`로 받은 키는 `devices.conf`에만 쓰이고 어떤 응답에도 실리지 않습니다. 요청을 argv가 아니라 stdin으로 넘기는 것도 키가 `ps`나 sudo 로그에 남지 않게 하기 위해서입니다.
- **포트를 새로 열지 않습니다.** 전송은 SSH 위의 stdio라서 인증은 SSH 키가 맡습니다.
- 설치되는 파일은 전부 root 소유의 복사본입니다. 서비스가 저장소의 바이너리를 직접 실행하면 저장소를 고칠 수 있는 계정이 곧 root가 됩니다.

---

## 설치

```bash
mcp/install.sh            # 지금 계정이 MCP 를 돌린다
mcp/install.sh vpnmcp     # 전용 계정으로 (없으면 만든다)
mcp/uninstall.sh          # 제거. --purge 를 붙이면 /etc/my_vpn (기기 키) 까지
```

- 요구사항: systemd, Node.js 18 이상, 그리고 [서버 빌드](linux.md#build)에 필요한 것
- 서버나 MCP 소스를 고친 뒤에는 다시 실행해야 설치본이 바뀝니다. 설정과 기기 목록은 건드리지 않습니다.
- 서비스는 부팅 시 자동 시작으로 등록하지 않습니다. 원하면 `sudo systemctl enable my_vpn`.
- `test/server.sh`와 같은 포트·터널 대역을 쓰므로 둘을 동시에 띄울 수 없습니다.

## 연결

AI 클라이언트 쪽 설정입니다. 서버 장비에서 `my_vpn_mcp`를 띄우는 명령을 주면 됩니다.

```bash
claude mcp add my_vpn -- ssh -T woo@<server-ip> my_vpn_mcp      # Claude Code
```

```json
{ "mcpServers": { "my_vpn": { "command": "ssh", "args": ["-T", "woo@<server-ip>", "my_vpn_mcp"] } } }
```

AI 클라이언트에 준 SSH 키가 MCP 말고는 아무것도 못 하게 하려면 서버의 `authorized_keys`에서 그 키를 묶습니다.
이렇게 하면 그 키로는 셸이 열리지 않고, 계정에 sudo 권한이 있어도 쓸 수 있는 것은 도구뿐입니다.

```
command="/usr/local/bin/my_vpn_mcp",restrict ssh-ed25519 AAAA... ai-client
```

---

## 도구

| 도구 | 하는 일 |
|---|---|
| `server_status` | 실행 여부, 포트, tun 상태, 현재 세션, 최근 거부 사유, ACL 재시작 필요 여부 |
| `server_start` / `server_stop` / `server_restart` | 서비스 제어. 재시작하면 모든 세션이 끊기고 클라이언트가 스스로 다시 붙는다 |
| `server_reload` | `SIGHUP` — 기기 목록 다시 읽기 (세션 유지). 파일을 손으로 고쳤을 때만 필요 |
| `server_logs` | 서버 로그. `lines`, `contains`, `since_seconds` |
| `device_list` | 등록된 기기 — ID, 이름, 터널 IP, 포스처 조건 (키 제외) |
| `device_add` | 기기 등록. ID와 키는 기기의 `vpn_client.exe enroll`에서 나온 값 |
| `device_update` | 포스처 조건 / 터널 IP / 이름 변경 |
| `device_remove` | 기기 폐기 |
| `acl_get` / `acl_set` / `acl_clear` | ACL 조회 / 전체 교체 / 끄기 |
| `auth_check` | `test/auth_check.py` 실행 (서버가 내려가 있을 때만) |

- **기기 변경은 바로 적용됩니다.** `device_*`는 파일을 고친 뒤 `SIGHUP`을 보내고, 서버가 새 목록을 실제로 받아들였는지 로그로 확인한 다음에 답합니다. 붙어 있던 기기는 다음 keepalive(10초 이내)에서 걸러집니다.
- **ACL 변경은 재시작해야 적용됩니다.** 서버가 ACL을 시작할 때 한 번만 읽기 때문입니다. 파일은 바뀌었는데 서버는 예전 규칙으로 도는 동안 `restart_required`가 `true`입니다.
- **세션 목록은 로그에서 계산한 값입니다.** 서버에 상태를 물어볼 창구가 없어, 이번 실행의 로그에서 `[peer] register / drop / expire`를 차례로 적용해 피어 테이블을 다시 만듭니다. 마지막 keepalive 시각 같은 것은 알 수 없습니다.

```
> 방화벽 꺼진 기기는 못 붙게 해 줘
device_update { id: "b26a7f68...", requirements: { firewall: true, antivirus: true, min_build: 22000 } }
  → reloaded: true
server_status
  → recent_rejections: [ "[ka] device b26a7f68... posture rejected (build=26200 firewall=1 antivirus=2)" ]
```
