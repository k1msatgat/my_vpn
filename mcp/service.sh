#!/usr/bin/env bash
# my_vpn.service 가 부른다. PORT / TUN_IP / PREFIX / MTU 는 유닛의 EnvironmentFile
# (/etc/my_vpn/server.env) 에서 온다.
#
#   service.sh run            vpn_server 로 exec (ExecStart)
#   service.sh tun-up <pid>   서버가 만든 tun 에 주소를 붙이고 올린다 (ExecStartPost)

set -eu

ETC=/etc/my_vpn
SERVER=/opt/my_vpn/server/bin/vpn_server

case "${1:-}" in
run)
	# 상태 소켓은 RuntimeDirectory(/run/my_vpn, root 전용) 안에 둔다. my_vpn_ctl 이 읽는다.
	# -n / -m 은 아래 tun-up 이 tun 에 붙이는 값과 같아야 한다. 서버가 그대로 클라이언트에
	# 내려 준다.
	args=(-s /run/my_vpn/status.sock -n "$PREFIX" -m "$MTU" "$PORT" "$ETC/devices.conf")

	# ACL 은 파일이 있으면 켜진 것이다. 켜고 끄는 것은 시작할 때만 정해지므로, 어떤
	# 내용으로 떴는지 적어 둔다 — my_vpn_ctl 이 지금 파일과 비교해 재시작이 필요한지 안다.
	if [ -f "$ETC/acl.conf" ]; then
		args+=("$ETC/acl.conf")
		sha256sum < "$ETC/acl.conf" | cut -d' ' -f1 > /run/my_vpn/acl.applied
	else
		echo none > /run/my_vpn/acl.applied
	fi

	# stdout 이 journal 로 가면 printf 가 블록 버퍼링된다. 줄 단위로 바꾼다.
	exec stdbuf -oL -eL "$SERVER" "${args[@]}"
	;;

tun-up)
	pid=$2
	dev=

	# tun 이름은 커널이 정한다. 서버 프로세스가 연 fd 에서 직접 읽는다.
	for _ in $(seq 50); do
		kill -0 "$pid" 2>/dev/null || { echo "vpn_server exited during startup" >&2; exit 1; }
		dev=$(awk '/^iff:/ { print $2; exit }' /proc/"$pid"/fdinfo/* 2>/dev/null || true)
		[ -n "$dev" ] && break
		sleep 0.1
	done
	[ -n "$dev" ] || { echo "vpn_server did not open a tun device" >&2; exit 1; }

	ip addr add "$TUN_IP/$PREFIX" dev "$dev"
	ip link set "$dev" mtu "$MTU" up
	echo "tun up: $dev $TUN_IP/$PREFIX mtu $MTU"
	;;

*)
	echo "usage: $0 run | tun-up <pid>" >&2
	exit 1
	;;
esac
