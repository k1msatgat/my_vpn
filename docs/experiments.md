# 실험 기록

코드의 대부분은 AI(Claude Code)가 구현했습니다. 이 문서는 그 코드가 설명한 대로 동작하는지 **직접 돌려 보고 깨뜨려 본 기록**입니다.
실험마다 세 줄로 남깁니다.

- **한 일** — 실행한 명령과 조건
- **본 것** — 출력, 캡처, 숫자 (그대로)
- **그 이유** — 왜 그렇게 되는지, 관련 함수와 파일

실험 환경(실험 1~8): WSL2 클라이언트 → IWINV Rocky Linux 9 서버, UDP 9000. 실제 장비의 준비 절차는 [README](../README.md)의 "장비 두 대"에 있습니다.
캡처 파일은 `docs/captures/` 에 둡니다 (실험 1부터).

| # | 실험 | 상태 |
|---|---|---|
| 0 | 서버 데이터 경로의 처리량과 perf | 기록 |
| 1 | tcpdump 로 등록 · 인증 핸드셰이크 캡처 | 예정 |
| 2 | 캡처한 데이터 패킷 재전송 | 예정 |
| 3 | 암호문 1바이트 변조 | 예정 |
| 4 | 서버 재시작 / 장시간 유휴 | 예정 |
| 5 | 연결 중 방화벽 끄기 | 예정 |
| 6 | ACL 변경 후 SIGHUP | 예정 |
| 7 | 큰 ping (-s 1400 이상) | 예정 |
| 8 | 같은 서버에 WireGuard 를 올려 핸드셰이크 비교 | 예정 |

---

## 0. 서버 데이터 경로의 처리량과 perf

> 이 실험은 CI 가 자동으로 돌린 측정이고, 해석은 AI 가 쓴 초안입니다. 1~8 은 직접 실행합니다.

**한 일**

- `tools/loadgen/bench.sh` 를 CI 러너에서 돌렸다 (커밋 `ce70b6d` 의 `bench` 잡). 부하 생성기는 TUN 없이 UDP 소켓으로 핸드셰이크를 하고, 터널 안에 ICMP echo 를 실어 서버의 tun 주소(10.0.0.1)로 최대 속도로 보낸다. 커널이 답하고 서버가 그 답을 돌려주므로, 돌아온 개수가 서버의 두 경로(udp→tun, tun→udp)를 모두 지난 왕복 처리량이다.
- 페이로드 56 / 512 / 1372 B, 각 10초. 서버 stdout 을 파일로 보낼 때와 `/dev/null` 로 보낼 때를 따로 쟀다 — 데이터 경로가 패킷마다 `printf` 를 하므로 "로그가 병목일 것"이라고 예상했다.
- 56 B 구간을 `perf record -g -p <서버>` 로 한 번 더 쟀다.
- 환경: GitHub Actions 러너, AMD EPYC 9V74 4 vCPU 공용 VM, 커널 6.17 (azure), 루프백, 세션 하나. 서버를 CPU 0 에, 부하 생성기(송신 + 수신 스레드)를 CPU 1-2 에 고정.

**본 것**

| 서버 로그 | 페이로드 | 보낸 pps | 받은 pps | 받은 Mbps | 손실 | 서버 CPU |
|---|---:|---:|---:|---:|---:|---:|
| 파일 | 56 B | 221,346 | 43,825 | 29.5 | 80.2% | 90% |
| 파일 | 512 B | 159,769 | 47,068 | 203.3 | 70.5% | 91% |
| 파일 | 1372 B | 202,588 | 42,793 | 479.3 | 78.9% | 91% |
| /dev/null | 56 B | 224,126 | 44,762 | 30.1 | 80.0% | 90% |
| /dev/null | 512 B | 213,887 | 42,980 | 185.7 | 79.9% | 91% |
| /dev/null | 1372 B | 203,256 | 42,408 | 475.0 | 79.1% | 90% |

```
perf report --no-children (56 B, 서버 로그는 파일, 1% 이상만)
11.88%  vpn_server  [kernel]  [k] _raw_spin_unlock_irqrestore
 4.05%  vpn_server  [kernel]  [k] do_syscall_64
 2.57%  vpn_server  [kernel]  [k] x64_sys_call
 1.37%  vpn_server  [kernel]  [k] srso_alias_safe_ret
 1.33%  vpn_server  [kernel]  [k] nf_hook_slow
 1.22%  vpn_server  [kernel]  [k] fib_table_lookup
 1.08%  vpn_server  [kernel]  [k] __alloc_skb
 1.05%  vpn_server  [kernel]  [k] kmem_cache_alloc_node_noprof
```

**그 이유**

- **패킷 크기와 상관없이 초당 약 4.2만~4.7만 개에서 막힌다.** 56 B 와 1372 B 의 pps 가 같고 Mbps 만 크기에 비례한다. 한계는 바이트당 비용(복사, AES-GCM)이 아니라 **패킷당 고정 비용**이다. 왕복 한 번에 서버는 시스템 콜 네 번(`recvfrom` → `write(tun)` → `read(tun)` → `sendto`)을 하고, 그 사이에 커널이 tun 에서 IP 스택으로 넘기고, 라우팅을 찾고(`fib_table_lookup`), netfilter 훅을 지나고(`nf_hook_slow`), 응답 패킷의 skb 를 할당한다(`__alloc_skb`, `kmem_cache_alloc_node`). 서버 CPU 가 90% 로 한 코어를 거의 다 쓰므로 단일 스레드 이벤트 루프(`server.c` 의 `main`)의 한계다. 보낸 것의 80% 가 사라진 것은 부하 생성기가 그보다 빨리 보내 서버의 소켓 수신 버퍼가 넘친 것이다.
- **"printf 가 병목"이라는 예상은 틀렸다.** 파일과 `/dev/null` 의 차이가 측정 오차 안이다. stdout 을 파일로 돌리면 stdio 가 블록 단위(4KB)로 모았다가 쓰므로 `write` 시스템 콜은 수십 줄에 한 번이고, perf 의 1% 이상 목록에도 `vfprintf` 같은 사용자 공간 함수가 없다. 다만 두 경우 모두 `printf` 의 포맷 계산은 일어나므로 포맷 비용 자체는 이 측정으로 따로 떼어 내지 못했다.
- **커널 쪽 상위 함수**
  - `_raw_spin_unlock_irqrestore` (12%): 스핀락을 풀면서 인터럽트를 다시 켜는 지점이다. 인터럽트가 꺼져 있는 동안의 샘플이 다시 켜지는 순간에 몰려 잡히므로, 이 함수 자체가 느리다기보다 **락을 잡고 인터럽트를 끈 채 하는 일**(소켓 / tun 큐에 패킷을 넣고 기다리는 쪽을 깨우는 일 등)의 비용이 여기에 모인 것으로 본다. VM 에서 하드웨어 PMU 대신 타이머로 샘플링하면 더 두드러진다. 호출 경로(`perf-callgraph.txt`)로 어느 큐인지까지는 아직 보지 않았다.
  - `do_syscall_64`, `x64_sys_call` (합 6.6%): 시스템 콜 진입. 패킷마다 네 번이라 그대로 비용이 된다 — `recvmmsg` / `sendmmsg` 로 묶으면 줄어드는 부분이다.
  - `srso_alias_safe_ret`: AMD Zen 의 SRSO(Speculative Return Stack Overflow) 취약점 완화 코드. 커널 함수가 돌아갈 때마다 거치므로, 짧은 커널 함수를 많이 부르는 패킷 경로에서 보인다.
  - `nf_hook_slow`, `fib_table_lookup`, `__alloc_skb`: tun 에 쓴 패킷이 일반 IP 패킷처럼 netfilter, 라우팅, 메모리 할당을 거친다는 뜻이다. 유저 공간 VPN 이 커널 VPN(WireGuard, IPsec xfrm)보다 느린 이유 중 하나다.
  - AES-GCM(OpenSSL, AES-NI)은 1% 목록에 없다. 1372 B 에서도 pps 가 같은 것과 맞는다.
- **이 측정의 한계**: 루프백이라 실제 NIC 와 드라이버를 거치지 않는다. 세션이 하나라 피어 테이블에 부하가 없다. 공용 VM 이라 다시 재면 값이 흔들린다. 부하 생성기와 서버가 같은 장비의 다른 코어를 쓴다.
- 관련: `tools/loadgen/loadgen.c`, `tools/loadgen/bench.sh`, `server/src/server.c` (데이터 경로), `.github/workflows/ci.yml` (`bench` 잡).
