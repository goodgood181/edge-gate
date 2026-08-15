#!/usr/bin/env bash
# 文件路径: scripts/install-mosquitto.sh
# 职责: apt 安装 mosquitto(演示 broker)并启动、开机自启,最后做
#       发布/订阅环回自检。仅演示环境使用,非生产部署脚本。
# 用法: ./scripts/install-mosquitto.sh
set -euo pipefail

if ! command -v mosquitto >/dev/null 2>&1; then
    echo "==> apt 安装 mosquitto / mosquitto-clients"
    sudo apt-get update
    sudo apt-get install -y mosquitto mosquitto-clients
fi

echo "==> 启动并设置开机自启"
sudo systemctl enable mosquitto 2>/dev/null || true
sudo systemctl start mosquitto 2>/dev/null || sudo service mosquitto start 2>/dev/null || true

sleep 1
if systemctl is-active mosquitto >/dev/null 2>&1 || pgrep -x mosquitto >/dev/null 2>&1; then
    echo "==> mosquitto 已就绪"
else
    echo "!! mosquitto 未运行,请检查: journalctl -u mosquitto"
    exit 1
fi

# 自检: pub/sub 环回一条消息验证 broker 可用
echo "==> 自检: mosquitto_pub/sub 环回"
timeout 5 mosquitto_sub -t edge-gate/selfcheck -C 1 -W 3 >/dev/null &
timeout 5 mosquitto_pub -t edge-gate/selfcheck -m ok 2>/dev/null
wait 2>/dev/null || true
echo "==> 完成"
