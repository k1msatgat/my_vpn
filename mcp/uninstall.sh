#!/usr/bin/env bash
# mcp/install.sh 가 설치한 것을 지운다.
#
#   mcp/uninstall.sh            /etc/my_vpn (기기 키, ACL, 설정) 은 남긴다
#   mcp/uninstall.sh --purge    그것까지 지운다
#
# install.sh 가 만든 계정은 지우지 않는다.

set -eu

sudo systemctl disable --now my_vpn.service 2>/dev/null || true
sudo rm -f /etc/systemd/system/my_vpn.service /etc/sudoers.d/my_vpn_mcp
sudo rm -f /usr/local/sbin/my_vpn_ctl /usr/local/bin/my_vpn_mcp
sudo rm -rf /opt/my_vpn
sudo systemctl daemon-reload

if [ "${1:-}" = --purge ]; then
	sudo rm -rf /etc/my_vpn
	echo "[uninstall] removed, including /etc/my_vpn"
else
	echo "[uninstall] removed; /etc/my_vpn is kept (use --purge to delete the device keys too)"
fi
