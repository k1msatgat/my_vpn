#!/usr/bin/env bash
# 클라이언트 장비에서 실행. 빌드 -> vpn_client 실행 -> tun 인터페이스 설정까지 한 번에 한다.
# 서버에서 test/server.sh 가 먼저 떠 있어야 한다.
#
#   test/client.sh                 일반 실행 (Ctrl-C 로 종료)
#   test/client.sh idle [sec]      sec 초(기본 120) 방치: 재핸드셰이크가 없어야 하고 세션이 살아 있어야 함
#   test/client.sh restart         서버 재시작 복구 확인. 서버는 'test/server.sh restart' 로 실행
#   test/client.sh stop [sec]      클라이언트를 sec 초(기본 70) 멈췄다 깨움: 서버 만료 후 재핸드셰이크 확인

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MODE=${1:-run}

case "$MODE" in
	run|idle|restart|stop) ;;
	*) echo "usage: $0 [run | idle [sec] | restart | stop [sec]]" >&2; exit 1 ;;
esac

become_root "$@"
install_traps
log_open client.log

REHS_LOG="no rx from server"
EST_LOG="[HS] established"

result() {
	if [ "$1" -eq 0 ]; then
		say "PASS: $2"
	else
		say "FAIL: $2"
	fi
	exit "$1"
}

show_idx() {
	grep -F "$EST_LOG" "$LOG" | sed 's/^/[test]   /'
}

say "client: $CLIENT_IF $CLIENT_TUN_IP -> server $SERVER_IP:$PORT (tunnel $SERVER_TUN_IP)"

start_proc "$ROOT_DIR/client/bin/vpn_client" "$CLIENT_IF" "$SERVER_IP" "$PORT" "$CLIENT_TUN_IP"
wait_log "tun device ready" 5 || die "client failed to start (see $LOG)"
setup_if "$CLIENT_IF" "$CLIENT_TUN_IP"
wait_log "$EST_LOG" 10 || die "handshake failed: is the server running at $SERVER_IP:$PORT?"

if ping_check "$SERVER_TUN_IP"; then
	say "ping $SERVER_TUN_IP ok"
else
	die "handshake ok but ping $SERVER_TUN_IP failed (server tun ip / ACL?)"
fi

case "$MODE" in
run)
	say "running (Ctrl-C to stop), log: $LOG"
	wait "$VPN_PID"
	;;

idle)
	SEC=${2:-120}

	countdown "$SEC" "idle"
	if [ "$(log_count "$REHS_LOG")" -ne 0 ]; then
		result 1 "re-handshake happened while the server was alive"
	fi
	if ! ping_check "$SERVER_TUN_IP"; then
		result 1 "session lost after ${SEC}s idle (keepalive not working?)"
	fi
	result 0 "no re-handshake in ${SEC}s idle and the session is still alive"
	;;

restart)
	ping -i 1 "$SERVER_TUN_IP" > /dev/null 2>&1 &
	PING_PID=$!

	say "ping running; waiting for the server to be restarted (up to 180s)"
	if ! wait_log "$EST_LOG" 180 2; then
		result 1 "no re-handshake (did the server run 'test/server.sh restart'?)"
	fi
	show_idx
	sleep 1
	if ! ping_check "$SERVER_TUN_IP"; then
		result 1 "re-handshake done but ping does not recover"
	fi
	result 0 "recovered from server restart with a new session"
	;;

stop)
	SEC=${2:-70}

	kill -STOP "$VPN_PID"
	countdown "$SEC" "client stopped (server should print '[peer] expire')"
	kill -CONT "$VPN_PID"

	if ! wait_log "$EST_LOG" 15 2; then
		result 1 "no re-handshake after resume"
	fi
	show_idx
	sleep 1
	if ! ping_check "$SERVER_TUN_IP"; then
		result 1 "re-handshake done but ping does not recover"
	fi
	result 0 "re-handshake after ${SEC}s stop, ping recovered"
	;;
esac
