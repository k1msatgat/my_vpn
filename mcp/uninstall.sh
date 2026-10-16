#!/usr/bin/env bash
# mcp/install.sh 가 설치한 것을 지운다.
#
#   mcp/uninstall.sh            /etc/ztgate (기기 키, ACL, 설정) 은 남긴다
#   mcp/uninstall.sh --purge    그것까지 지운다
#
# install.sh 가 만든 계정은 지우지 않는다.

set -eu

sudo systemctl disable --now ztgate.service 2>/dev/null || true
sudo rm -f /etc/systemd/system/ztgate.service /etc/sudoers.d/ztgate_mcp
sudo rm -f /usr/local/sbin/ztgate_ctl /usr/local/bin/ztgate_mcp
sudo rm -rf /opt/ztgate
sudo systemctl daemon-reload

if [ "${1:-}" = --purge ]; then
	sudo rm -rf /etc/ztgate
	echo "[uninstall] removed, including /etc/ztgate"
else
	echo "[uninstall] removed; /etc/ztgate is kept (use --purge to delete the device keys too)"
fi
