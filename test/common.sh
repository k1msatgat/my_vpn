#!/usr/bin/env bash
# server.sh / client.sh 가 source 해서 쓰는 공용 설정 및 함수.
# 아래 값들은 환경변수로 덮어쓸 수 있다. 예) SERVER_IP=1.2.3.4 test/client.sh

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(dirname "$TEST_DIR")
LOG_DIR=$TEST_DIR/logs

: "${SERVER_IP:=49.247.139.39}"   # 클라이언트가 접속할 서버의 외부 주소
: "${PORT:=9000}"
: "${SERVER_TUN_IP:=10.0.0.1}"
: "${CLIENT_TUN_IP:=10.0.0.2}"
: "${PREFIX:=24}"
: "${CLIENT_IF:=tun0}"
: "${MTU:=1400}"
: "${ACL_FILE:=}"                 # 서버 전용, 비우면 ACL 비활성

CFG_VARS=(SERVER_IP PORT SERVER_TUN_IP CLIENT_TUN_IP PREFIX CLIENT_IF MTU ACL_FILE)

VPN_PID=
TAIL_PID=
PING_PID=
LOG=

say() { printf '\n[test] %s\n' "$*"; }
die() { printf '\n[test] ERROR: %s\n' "$*" >&2; exit 1; }

# 빌드는 일반 사용자로 하고(bin/ 이 root 소유가 되지 않도록) 그 뒤 sudo 로 재실행한다.
become_root() {
	local envs=() v

	if [ "$(id -u)" -ne 0 ]; then
		make -C "$ROOT_DIR" --no-print-directory || exit 1
		for v in "${CFG_VARS[@]}"; do
			envs+=("$v=${!v}")
		done
		exec sudo env "${envs[@]}" VPN_TEST_BUILT=1 bash "$0" "$@"
	fi

	if [ -z "${VPN_TEST_BUILT:-}" ]; then
		make -C "$ROOT_DIR" --no-print-directory || exit 1
	fi
}

log_open() {
	mkdir -p "$LOG_DIR"
	LOG=$LOG_DIR/$1
	: > "$LOG"
	tail -n +1 -f "$LOG" &
	TAIL_PID=$!
}

log_count() {
	grep -cF -- "$1" "$LOG" || true
}

# wait_log <문자열> <timeout초> [최소 등장 횟수]
# 0: 찾음, 1: 타임아웃, 2: 프로세스가 먼저 종료됨
wait_log() {
	local pat=$1 want=${3:-1}
	local end=$((SECONDS + $2))

	while [ "$(log_count "$pat")" -lt "$want" ]; do
		if [ "$SECONDS" -ge "$end" ]; then
			return 1
		fi
		if [ -n "$VPN_PID" ] && ! kill -0 "$VPN_PID" 2>/dev/null; then
			return 2
		fi
		sleep 0.5
	done
	return 0
}

# stdout 이 파일이면 printf 가 블록 버퍼링되므로 stdbuf 로 줄 단위로 바꾼다.
start_proc() {
	stdbuf -oL -eL "$@" >> "$LOG" 2>&1 &
	VPN_PID=$!
}

setup_if() {
	ip addr add "$2/$PREFIX" dev "$1" || die "ip addr add failed on $1"
	ip link set "$1" mtu "$MTU" up || die "ip link set failed on $1"
	say "$1 up: $2/$PREFIX mtu $MTU"
}

countdown() {
	local left=$1

	while [ "$left" -gt 0 ]; do
		printf '\r[test] %s: %3ds left ' "$2" "$left"
		sleep 1
		left=$((left - 1))
	done
	printf '\n'
}

ping_check() {
	ping -c 3 -W 2 "$1" > /dev/null 2>&1
}

cleanup() {
	trap - EXIT INT TERM
	if [ -n "$PING_PID" ]; then
		kill "$PING_PID" 2>/dev/null
	fi
	if [ -n "$VPN_PID" ]; then
		kill -CONT "$VPN_PID" 2>/dev/null
		kill -TERM "$VPN_PID" 2>/dev/null
		wait "$VPN_PID" 2>/dev/null
	fi
	sleep 0.3
	if [ -n "$TAIL_PID" ]; then
		kill "$TAIL_PID" 2>/dev/null
	fi
}

install_traps() {
	trap cleanup EXIT
	trap 'exit 130' INT TERM
}
