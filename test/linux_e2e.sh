#!/usr/bin/env bash
# Linux 서버 <-> Linux 클라이언트 전체 경로를 한 장비에서 확인한다.
#
#   test/linux_e2e.sh            root 가 아니면 사용자 네임스페이스(unshare -rn) 안에서 돈다
#   sudo test/linux_e2e.sh       root 면 새 네트워크 네임스페이스(unshare -n) 안에서 돈다
#   E2E_LOGS=<dir> test/linux_e2e.sh   서버 / 클라이언트 로그를 <dir> 에 남긴다
#
# 네트워크 네임스페이스 두 개를 veth 로 잇는다. 장비의 네트워크는 건드리지 않는다.
#
#   [서버 ns] vpn_server  192.0.2.1 --veth-- 192.0.2.2  vpn_client [클라이언트 ns]
#             tun 10.0.0.1/24                              myvpn0 (서버가 주는 주소)
#
# 시나리오
#   1. 등록 코드로 기기 등록 -> 신원 파일(0600)과 서버 기기 목록에 한 줄
#   2. 연결 -> 서버가 준 주소 / MTU / 경로가 tun 에 붙고, 터널로 ping 이 오간다
#   3. ACL -> 허용하지 않은 목적지 / 포트는 서버가 버린다
#   4. 포스처 -> 기기 정책에 firewall=on 을 걸고 SIGHUP. 다음 keepalive 에서 거절된다.
#      클라이언트 ns 에 nft 로 인바운드 기본 거부를 걸면 스스로 다시 붙는다.
#
# 사용자 네임스페이스와 실제 장비가 다른 점은 docs/linux.md 에 적어 두었다.

set -u

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(dirname "$TEST_DIR")
SERVER="$ROOT_DIR/server/bin/vpn_server"
CLIENT="$ROOT_DIR/client/bin/vpn_client"

PORT=9000
SRV_VETH=192.0.2.1
CLI_VETH=192.0.2.2
SRV_TUN=10.0.0.1
CLI_TUN=10.0.0.2
ROUTED_NET=198.51.100.0/24	# ACL 의 allow 목적지 -> 서버가 경로로 내려 준다
CLI_IF=myvpn0
# 0-9 와 I L O U 를 뺀 대문자 25자 (proto.h 의 ENROLL_CODE_LEN). 테스트용 고정 값.
CODE=7K2QF-9XWM4-ABCDE-FGHJK-MNPQR
KEEPALIVE_WAIT=15		# keepalive 주기(10초) + 여유

if [ -z "${MY_VPN_E2E_NS:-}" ]; then
	[ -x "$SERVER" ] && [ -x "$CLIENT" ] || { echo "build first: make server client" >&2; exit 1; }
	export MY_VPN_E2E_NS=1
	if [ "$(id -u)" -eq 0 ]; then
		exec unshare -n "$0" "$@"
	fi
	exec unshare -rn "$0" "$@"
fi

WORK=$(mktemp -d)
DEVICES=$WORK/devices.conf
ENROLL=$WORK/enroll.conf
ACL=$WORK/acl.conf
IDENT=$WORK/device
SLOG=$WORK/server.log
CLOG=$WORK/client.log
SRV_PID=
CLI_PID=
NS_PID=
failed=0

cleanup() {
	[ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null
	[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
	[ -n "$NS_PID" ] && kill "$NS_PID" 2>/dev/null
	wait 2>/dev/null
	if [ "$failed" -ne 0 ]; then
		echo
		echo "---- server log (last 30) ----"; tail -n 30 "$SLOG"
		echo "---- client log (last 30) ----"; tail -n 30 "$CLOG"
	fi
	if [ -n "${E2E_LOGS:-}" ]; then
		mkdir -p "$E2E_LOGS" && cp "$SLOG" "$CLOG" "$E2E_LOGS"/
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

# 클라이언트 ns 에서 명령을 돌린다. 사용자 네임스페이스 안이면 자격을 그대로 들고 들어가야 한다.
in_cli() { nsenter -t "$NS_PID" -n --preserve-credentials "$@"; }

check() {	# check <이름> <조건 명령...>
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s\n' "$name"
		failed=$((failed + 1))
	fi
}

wait_for() {	# wait_for <초> <조건 명령...>
	local i=$(( $1 * 10 )); shift
	while [ "$i" -gt 0 ]; do
		"$@" >/dev/null 2>&1 && return 0
		sleep 0.1
		i=$((i - 1))
	done
	return 1
}

ping_ok() { in_cli ping -c 1 -W 1 "$1"; }
ping_fails() { ! in_cli ping -c 1 -W 1 "$1"; }
log_has() { grep -qF -- "$2" "$1"; }

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

# ---- 서버 ---------------------------------------------------------------------
# 등록 대기 줄: 코드에서 뽑은 임시 신원 (secure.h 의 secure_enroll_identity) + 줄 터널 주소.
read -r EID EKEY < <(python3 - "$CODE" <<'EOF'
import hashlib, hmac, sys
code = sys.argv[1].replace("-", "").upper()
mac = lambda label: hmac.new(label, code.encode(), hashlib.sha256).digest()
print(mac(b"my_vpn enroll id")[:16].hex(), mac(b"my_vpn enroll key").hex())
EOF
)
: > "$DEVICES"
printf '%s %s %s %d  # e2e\n' "$EID" "$EKEY" "$CLI_TUN" $(( $(date +%s) + 600 )) > "$ENROLL"
cat > "$ACL" <<EOF
10.0.0.0/24  $SRV_TUN       icmp  any  allow
10.0.0.0/24  $ROUTED_NET    icmp  any  allow
any          any            any   any  deny
EOF
chmod 600 "$DEVICES" "$ENROLL" "$ACL"

before=$(ip -o link show type tun | awk -F': ' '{print $2}')
stdbuf -oL "$SERVER" -e "$ENROLL" "$PORT" "$DEVICES" "$ACL" > "$SLOG" 2>&1 &
SRV_PID=$!
wait_for 5 sh -c "ip -o link show type tun | grep -q ." || { echo "server did not start"; cat "$SLOG"; exit 1; }
SRV_IF=$(ip -o link show type tun | awk -F': ' '{print $2}' | grep -vxF "$before" | head -n 1)
ip addr add "$SRV_TUN/24" dev "$SRV_IF"
ip link set "$SRV_IF" mtu 1400 up

echo "== 1. enrollment"
in_cli "$CLIENT" -k "$IDENT" enroll "$SRV_VETH" "$PORT" "$CODE" > "$CLOG" 2>&1
check "enroll: client reports success" log_has "$CLOG" "enrolled. identity saved"
check "enroll: identity file is mode 600" test "$(stat -c %a "$IDENT" 2>/dev/null)" = 600
check "enroll: server added the device to its list" grep -q "$(cut -c1-32 "$IDENT" 2>/dev/null)" "$DEVICES"
check "enroll: the code cannot be used twice" sh -c "! nsenter -t $NS_PID -n --preserve-credentials \
	'$CLIENT' -f -k '$WORK/other' enroll '$SRV_VETH' '$PORT' '$CODE'"

echo "== 2. connect"
in_cli "$CLIENT" -k "$IDENT" -i "$CLI_IF" run "$SRV_VETH" "$PORT" > "$CLOG" 2>&1 &
CLI_PID=$!
check "connect: session established" wait_for 10 log_has "$CLOG" "] connected"
check "connect: tun has the address the server assigned" \
	sh -c "nsenter -t $NS_PID -n --preserve-credentials ip -o -4 addr show $CLI_IF | grep -q '$CLI_TUN/24'"
check "connect: tun mtu is the server's (1400)" \
	sh -c "nsenter -t $NS_PID -n --preserve-credentials ip -o link show $CLI_IF | grep -q 'mtu 1400'"
check "connect: route from the ACL allow rule ($ROUTED_NET)" \
	sh -c "nsenter -t $NS_PID -n --preserve-credentials ip route show $ROUTED_NET | grep -q $CLI_IF"
check "connect: ping $SRV_TUN through the tunnel" ping_ok "$SRV_TUN"

echo "== 3. acl"
denied_before=$(grep -c '^\[deny\]' "$SLOG")
in_cli bash -c "echo probe > /dev/udp/$SRV_TUN/22" 2>/dev/null
check "acl: udp to $SRV_TUN:22 is dropped by the server" \
	wait_for 3 sh -c "[ \$(grep -c '^\[deny\]' '$SLOG') -gt $denied_before ]"
check "acl: the session survives a denied packet" ping_ok "$SRV_TUN"

echo "== 4. posture"
check "posture: client ns has no inbound filter (firewall=off)" \
	sh -c "nsenter -t $NS_PID -n --preserve-credentials '$CLIENT' posture | grep -q 'firewall  off'"
# 기기 정책을 조인다: 이 기기 줄의 터널 주소 뒤에 firewall=on.
sed -i -E "s/^([0-9a-f]{32}[[:space:]]+[0-9a-f]{64}[[:space:]]+$CLI_TUN)/\1 firewall=on/" "$DEVICES"
kill -HUP "$SRV_PID"
t0=$(date +%s)
check "posture: policy tightened -> denied at the next keepalive" \
	wait_for "$KEEPALIVE_WAIT" log_has "$CLOG" "[deny] posture does not meet policy"
echo "     (denied after $(( $(date +%s) - t0 ))s)"
check "posture: no traffic while denied" ping_fails "$SRV_TUN"
check "posture: tun stays up while denied" in_cli ip link show "$CLI_IF"

# 인바운드 기본 거부. 터널(서버의 UDP 와 tun)과 lo 만 받는다. conntrack 을 쓰지 않는 것은
# 사용자 네임스페이스에서 모듈을 올릴 수 없어서다.
in_cli nft -f - <<EOF
table inet e2e {
	chain input {
		type filter hook input priority 0; policy drop;
		iif lo accept
		udp sport $PORT accept
		iifname "$CLI_IF" accept
	}
}
EOF
t0=$(date +%s)
check "posture: firewall on -> client re-handshakes by itself" \
	wait_for 10 sh -c "[ \$(grep -c '] connected' '$CLOG') -ge 2 ]"
echo "     (reconnected after $(( $(date +%s) - t0 ))s)"
check "posture: traffic flows again" ping_ok "$SRV_TUN"

echo
if [ "$failed" -eq 0 ]; then
	echo "all passed"
else
	echo "$failed failed"
fi
exit "$failed"
