#!/bin/bash
# EdgeGate 一键上传 GitHub(需先 gh auth login)
# 用法: ./scripts/publish-github.sh [仓库名]   # 默认 edge-gate,Public 可见
set -e
cd "$(dirname "$0")/.."

REPO="${1:-edge-gate}"

echo "=== 1) 检查 gh 认证 ==="
if ! gh auth status >/dev/null 2>&1; then
    echo "尚未登录 GitHub。请先执行:"
    echo "    gh auth login"
    echo "    (选择 GitHub.com → HTTPS → Login with a web browser → 浏览器授权)"
    exit 1
fi
echo "已登录: $(gh api user -q .login)"

echo "=== 2) 检查 git 状态 ==="
if [ -n "$(git status --porcelain)" ]; then
    echo "有未提交的修改,先提交或 stash:"
    git status --short
    exit 1
fi

echo "=== 3) 创建远程仓库并推送 ==="
if gh repo view "$REPO" >/dev/null 2>&1; then
    echo "仓库 $REPO 已存在,走 remote+push 路径"
    git remote remove origin 2>/dev/null || true
    git remote add origin "https://github.com/$(gh api user -q .login)/$REPO.git"
else
    echo "创建公开仓库 $REPO 并推送现有历史..."
    gh repo create "$REPO" --public --source . --push --description \
        "EdgeGate: 嵌入式 Linux 工业数据采集网关(从零实现 Modbus RTU + MQTT 3.1.1,PTY 无硬件闭环演示,C++17 / CMake 交叉编译 ARM&x86)"
fi

echo "=== 4) 确认推送结果 ==="
git push -u origin main
echo ""
echo "✅ 上传完成: https://github.com/$(gh api user -q .login)/$REPO"
