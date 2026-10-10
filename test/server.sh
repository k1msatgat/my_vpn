#!/usr/bin/env bash
# 서버 장비에서 실행. 빌드 -> vpn_server 실행 -> tun 인터페이스 설정까지 한 번에 한다.
#
#   test/server.sh                      일반 실행 (Ctrl-C 로 종료)
#   test/server.sh restart [after] [down]
#                                       클라이언트 접속 after 초(기본 15) 뒤 kill -9,
#                                       down 초(기본 5) 뒤 재시작. client.sh restart 와 짝.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MODE=${1:-run}

case "$MODE" in
	run|restart) ;;
	*) echo "usage: $0 [run | restart [after-sec] [down-sec]]" >&2; exit 1 ;;
esac

become_root "$@"
install_traps
log_open server.log

start_server() {
	local started dev
	local args=("$PORT")

	started=$(log_count "listening on UDP")
	if [ -n "$ACL_FILE" ]; then
		args+=("$ACL_FILE")
	fi

	start_proc "$ROOT_DIR/server/bin/vpn_server" "${args[@]}"
	wait_log "listening on UDP" 5 $((started + 1)) || die "server failed to start (see $LOG)"

	dev=$(grep -F "tun device:" "$LOG" | tail -n 1 | sed -E 's/^tun device: ([^ ]+).*/\1/')
	setup_if "$dev" "$SERVER_TUN_IP"
}

# 클라이언트 터널 IP 가 이 장비의 로컬 주소로 남아 있으면 커널이 터널로 들어온 패킷을 버린다.
STALE_IF=$(ip -o -4 addr show | awk -v ip="$CLIENT_TUN_IP" '{split($4, a, "/"); if (a[1] == ip) print $2}' | head -n 1)
if [ -n "$STALE_IF" ]; then
	die "client tunnel ip $CLIENT_TUN_IP is assigned locally on $STALE_IF; remove it first: sudo ip tuntap del dev $STALE_IF mode tun"
fi

say "server: udp port $PORT, tunnel ip $SERVER_TUN_IP, acl ${ACL_FILE:-off}"
start_server

if [ "$MODE" = restart ]; then
	AFTER=${2:-15}
	DOWN=${3:-5}

	say "waiting for a client (run 'test/client.sh restart' on the client)"
	wait_log "[peer] register" 600 || die "no client registered"

	countdown "$AFTER" "kill -9 server in"
	kill -9 "$VPN_PID"
	wait "$VPN_PID" 2>/dev/null
	say "server killed"

	countdown "$DOWN" "server down"
	start_server
	say "server restarted: expect '[drop] unknown session_idx' and then a new '[peer] register'"
fi

say "running (Ctrl-C to stop), log: $LOG"
wait "$VPN_PID"
