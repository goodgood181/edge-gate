#!/usr/bin/env bash
# 文件路径: scripts/deploy-arm.sh
# 职责: 交叉编译并部署到 ARM 板(IMX6ULL):
#   1) 交叉编译 build-arm/edge-gate(工具链: cmake/toolchain-arm-linux-gnueabihf.cmake)
#   2) scp 二进制 + ARM 配置 + systemd unit 到板子
#   3) ssh 启用并重启服务
# 前置: 本机已装交叉工具链(apt install g++-arm-linux-gnueabihf);
#       板子可达且已配 SSH 免密。
# 环境变量: TARGET_HOST(默认 root@192.168.1.100)
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET_HOST="${TARGET_HOST:-root@192.168.1.100}"
REMOTE_BIN=/usr/local/bin/edge-gate
REMOTE_CFG=/etc/edge-gate/edge-gate.json
REMOTE_UNIT=/etc/systemd/system/edge-gate.service

echo "==> 交叉编译(build-arm)"
cmake -S . -B build-arm \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j"$(nproc)"

echo "==> 部署到 ${TARGET_HOST}"
ssh "${TARGET_HOST}" "mkdir -p /etc/edge-gate /var/lib/edge-gate"
scp build-arm/edge-gate "${TARGET_HOST}:${REMOTE_BIN}"
scp config/edge-gate-arm.json "${TARGET_HOST}:${REMOTE_CFG}"
scp scripts/edge-gate.service "${TARGET_HOST}:${REMOTE_UNIT}"

echo "==> 启用并重启服务"
ssh "${TARGET_HOST}" "systemctl daemon-reload && systemctl enable edge-gate && \
    systemctl restart edge-gate && systemctl status edge-gate --no-pager"
echo "==> 完成;查看日志: ssh ${TARGET_HOST} 'journalctl -u edge-gate -f'"
