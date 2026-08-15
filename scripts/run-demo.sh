#!/usr/bin/env bash
# 文件路径: scripts/run-demo.sh
# 职责: 一键演示 —— 确保 mosquitto 在跑 → 构建 → 前台运行网关
#       (pty-sim 闭环: 主站 ↔ 软件从站走真实 termios 时序,零硬件)。
# 用法: ./scripts/run-demo.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# 1) mosquitto(演示上云;缺失时提示安装脚本)
if ! pgrep -x mosquitto >/dev/null 2>&1; then
    echo "==> 未检测到 mosquitto,尝试启动..."
    if command -v mosquitto >/dev/null 2>&1; then
        mosquitto -d 2>/dev/null || sudo systemctl start mosquitto 2>/dev/null || true
        sleep 1
    else
        echo "!! 未安装 mosquitto,请先运行: ./scripts/install-mosquitto.sh"
        exit 1
    fi
fi
echo "==> mosquitto 运行中"

# 2) 构建(二进制不存在时)
if [ ! -f build/edge-gate ]; then
    ./scripts/build.sh
fi

# 3) 前台运行(exec: 让 Ctrl-C 信号直达网关进程,优雅停止)
echo "==> 启动 EdgeGate(pty-sim 演示;Ctrl-C 优雅退出)"
exec ./build/edge-gate --config config/edge-gate.json "$@"
