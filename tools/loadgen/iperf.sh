#!/usr/bin/env bash
# 터널을 거친 TCP / UDP 처리량을 같은 길의 직결 값과 나란히 잰다.
#
#   tools/loadgen/iperf.sh [out-dir]        root 가 아니면 사용자 네임스페이스 안에서 돈다
#   sudo tools/loadgen/iperf.sh [out-dir]   root 면 새 네트워크 네임스페이스 안에서 돈다
#
# bench.sh 는 서버 혼자의 패킷 처리 한계를 본다 (부하 생성기가 TUN 없이 UDP 로 직접
# 보낸다). 사용자가 느끼는 속도는 그게 아니라 "클라이언트의 TCP 가 터널을 지나 얼마나
# 나오는가"이고, 거기에는 클라이언트의 tun 과 암호화, 두 번의 MTU 축소, TCP 의 혼잡
# 제어가 함께 들어간다. 그래서 실제 vpn_server 와 vpn_client 를 띄우고 그 사이로 iperf3
# 를 돌린다. 같은 veth 위를 터널 없이 지난 값을 함께 재야 "터널이 얼마를 깎았는가"가 된다.
#
#   [서버 ns] vpn_server, iperf3 -s   192.0.2.1 --veth-- 192.0.2.2   vpn_client, iperf3 -c [클라이언트 ns]
#             tun 10.0.0.1                                          myvpn0 10.0.0.2
#
#   direct   iperf3 -c 192.0.2.1   veth 만 지난다 (MTU 1500)
#   tunnel   iperf3 -c 10.0.0.1    클라이언트 tun -> 암호화 -> UDP -> 서버 -> tun (MTU 1400)
#
# 환경 변수
#   DUR=10        한 번 재는 시간(초)
#   UDP_RATE=0    UDP 송신 속도 (iperf3 -b). 0 이면 제한 없이
#   PARALLEL=1    TCP 스트림 수 (iperf3 -P)
#
# CPU 가 4개 이상이면 vpn_server, vpn_client, iperf3 서버, iperf3 클라이언트를 CPU 0-3 에
# 하나씩 묶는다.
# iperf3 가 VPN 프로세스와 CPU 를 다투면 터널이 느린 것인지 측정기가 느린 것인지 갈라
# 낼 수 없다.

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SERVER="$ROOT/server/bin/vpn_server"
CLIENT="$ROOT/client/bin/vpn_client"
OUT=${1:-$ROOT/tools/loadgen/out}
DUR=${DUR:-10}
UDP_RATE=${UDP_RATE:-0}
PARALLEL=${PARALLEL:-1}

PORT=9000
SRV_VETH=192.0.2.1
CLI_VETH=192.0.2.2
SRV_TUN=10.0.0.1
CLI_TUN=10.0.0.2
CLI_IF=myvpn0

# 측정 전용 기기. 키를 공개해도 된다.
DEV_ID=c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1
DEV_KEY=5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a

die() { echo "iperf: $*" >&2; exit 1; }

if [ -z "${MY_VPN_IPERF_NS:-}" ]; then
	[ -x "$SERVER" ] && [ -x "$CLIENT" ] || die "build first: make server client"
	command -v iperf3 >/dev/null || die "iperf3 not found"
	mkdir -p "$OUT"
	OUT=$(cd "$OUT" && pwd)
	export MY_VPN_IPERF_NS=1
	if [ "$(id -u)" -eq 0 ]; then
		exec unshare -n "$0" "$OUT"
	fi
	exec unshare -rn "$0" "$OUT"
fi

if [ "$(nproc)" -ge 4 ]; then
	PIN_SERVER=(taskset -c 0)
	PIN_CLIENT=(taskset -c 1)
	PIN_IPERF_S=(taskset -c 2)
	PIN_IPERF=(taskset -c 3)
else
	PIN_SERVER=()
	PIN_CLIENT=()
	PIN_IPERF_S=()
	PIN_IPERF=()
	echo "iperf: only $(nproc) CPU(s) — VPN processes and iperf3 share CPUs" >&2
fi

WORK=$(mktemp -d)
SRV_PID=
CLI_PID=
IPERF_PID=
NS_PID=

cleanup() {
	for p in $IPERF_PID $CLI_PID $SRV_PID $NS_PID; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

in_cli() { nsenter -t "$NS_PID" -n --preserve-credentials "$@"; }

wait_for() {	# wait_for <초> <조건 명령...>
	local i=$(( $1 * 10 )); shift
	while [ "$i" -gt 0 ]; do
		"$@" >/dev/null 2>&1 && return 0
		sleep 0.1
		i=$((i - 1))
	done
	return 1
}

# 프로세스가 지금까지 쓴 CPU 시간(틱). /proc/<pid>/stat 의 utime + stime.
cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat"; }
HZ=$(getconf CLK_TCK)

# ---- 네트워크 -----------------------------------------------------------------
ip link set lo up
unshare -n sleep infinity &
NS_PID=$!
sleep 0.2
ip link add v-srv type veth peer name v-cli
ip link set v-cli netns "$NS_PID"
ip addr add "$SRV_VETH/24" dev v-srv
ip link set v-srv up
in_cli ip link set lo up
in_cli ip addr add "$CLI_VETH/24" dev v-cli
in_cli ip link set v-cli up

# ---- 서버와 클라이언트 --------------------------------------------------------
echo "$DEV_ID $DEV_KEY $CLI_TUN" > "$WORK/devices.conf"
# iperf3 의 제어 연결(TCP 5201)과 데이터(TCP / UDP 5201)만 연다.
cat > "$WORK/acl.conf" <<EOF
10.0.0.0/24  $SRV_TUN/32  tcp  5201  allow
10.0.0.0/24  $SRV_TUN/32  udp  5201  allow
any          any          any  any   deny
EOF
printf '%s %s\n' "$DEV_ID" "$DEV_KEY" > "$WORK/device"
chmod 600 "$WORK/devices.conf" "$WORK/acl.conf" "$WORK/device"

before=$(ip -o link show type tun | awk -F': ' '{print $2}')
"${PIN_SERVER[@]}" "$SERVER" "$PORT" "$WORK/devices.conf" "$WORK/acl.conf" > "$WORK/server.log" 2>&1 &
SRV_PID=$!
wait_for 5 sh -c "ip -o link show type tun | grep -q ." || { cat "$WORK/server.log"; die "server did not start"; }
SRV_IF=$(ip -o link show type tun | awk -F': ' '{print $2}' | grep -vxF "$before" | head -n 1)
ip addr add "$SRV_TUN/24" dev "$SRV_IF"
ip link set "$SRV_IF" mtu 1400 up

in_cli "${PIN_CLIENT[@]}" "$CLIENT" -k "$WORK/device" -i "$CLI_IF" run "$SRV_VETH" "$PORT" \
	> "$WORK/client.log" 2>&1 &
CLI_PID=$!
wait_for 10 grep -qF "] connected" "$WORK/client.log" \
	|| { cat "$WORK/server.log" "$WORK/client.log"; die "client did not connect"; }
# nsenter 를 거쳐 띄웠으므로 $! 는 nsenter 다. CPU 를 재려면 그 아래의 vpn_client 가 필요하다.
CLI_REAL=$(pgrep -P "$CLI_PID" -x vpn_client || echo "$CLI_PID")

"${PIN_IPERF_S[@]}" iperf3 -s -B 0.0.0.0 > "$WORK/iperf-server.log" 2>&1 &
IPERF_PID=$!
sleep 0.5

# ---- 측정 ---------------------------------------------------------------------
# run <이름> <목적지> <iperf3 인자...>  ->  결과 한 줄 (Mbps, 재전송 또는 손실, CPU)
# iperf3 -J 의 JSON 에서 숫자만 뽑는다. jq 가 없는 장비도 있어 python3 으로 읽는다.
TABLE="$OUT/iperf.md"
{
	echo "| 경로 | 프로토콜 | 받은 처리량 | 재전송 / 손실 | vpn_server CPU | vpn_client CPU |"
	echo "|---|---|---:|---:|---:|---:|"
} > "$TABLE"

run() {
	local name=$1 dst=$2 proto=$3; shift 3
	local json="$OUT/iperf-$name-$proto.json" s0 c0 t0 s1 c1 t1 summary
	s0=$(cpu_ticks "$SRV_PID"); c0=$(cpu_ticks "$CLI_REAL"); t0=$(date +%s%N)
	in_cli "${PIN_IPERF[@]}" iperf3 -c "$dst" -t "$DUR" -J "$@" > "$json" 2> "$json.err" \
		|| { cat "$json" "$json.err"; die "iperf3 to $dst ($proto) failed"; }
	s1=$(cpu_ticks "$SRV_PID"); c1=$(cpu_ticks "$CLI_REAL"); t1=$(date +%s%N)

	summary=$(python3 - "$json" "$proto" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))["end"]
if sys.argv[2] == "tcp":
    s = r["sum_received"]
    print(f'{s["bits_per_second"] / 1e6:.0f} Mbps|{r["sum_sent"].get("retransmits", 0)} 재전송')
else:
    # 클라이언트 쪽 JSON 의 sum 은 보낸 속도다. 받은 쪽 값이 없는 버전(3.9 등)도 있어
    # 손실률로 받은 속도를 계산한다.
    s = r["sum"]
    sent = s["bits_per_second"] / 1e6
    got = r["sum_received"]["bits_per_second"] / 1e6 if "sum_received" in r \
        else sent * (1 - s["lost_percent"] / 100)
    print(f'{got:.0f} Mbps (보낸 {sent:.0f})|{s["lost_percent"]:.1f}% 손실')
EOF
)
	cpu() { awk -v t="$1" -v hz="$HZ" -v ns="$((t1 - t0))" 'BEGIN { printf "%.0f%%", 100 * t / hz / (ns / 1e9) }'; }
	if [ "$name" = direct ]; then
		printf '| %s | %s | %s | %s | - | - |\n' "$name" "$proto" "${summary%%|*}" "${summary#*|}" >> "$TABLE"
	else
		printf '| %s | %s | %s | %s | %s | %s |\n' "$name" "$proto" "${summary%%|*}" "${summary#*|}" \
			"$(cpu $((s1 - s0)))" "$(cpu $((c1 - c0)))" >> "$TABLE"
	fi
}

UDP_ARGS=(-u -b "$UDP_RATE")
run direct "$SRV_VETH" tcp -P "$PARALLEL"
run tunnel "$SRV_TUN" tcp -P "$PARALLEL"
# UDP 데이터그램은 각 경로의 MTU 에 맞춘다 (IP 20 + UDP 8 을 뺀 값). 기본값(1460)으로
# 보내면 터널 쪽에서는 IP 단편화가 일어나 터널이 아니라 단편화를 재게 된다.
run direct "$SRV_VETH" udp "${UDP_ARGS[@]}" -l 1472
run tunnel "$SRV_TUN" udp "${UDP_ARGS[@]}" -l 1372

{
	echo
	echo "- CPU: $(lscpu | sed -n 's/^Model name: *//p'), $(nproc) cores, kernel $(uname -r), $(iperf3 --version | head -n 1)"
	echo "- ${DUR}s per row, veth between two network namespaces, one session, TCP streams: $PARALLEL, UDP rate: $([ "$UDP_RATE" = 0 ] && echo unlimited || echo "$UDP_RATE")"
	if [ ${#PIN_SERVER[@]} -gt 0 ]; then
		echo "- vpn_server on CPU 0, vpn_client on CPU 1, iperf3 server on CPU 2, iperf3 client on CPU 3"
	else
		echo "- not pinned"
	fi
	echo "- CPU columns: share of one core over the iperf3 run (100% = one core busy)"
} >> "$TABLE"

cat "$TABLE"
