#!/usr/bin/env bash
# 文件路径: scripts/build.sh
# 职责: 一键构建 + 测试(x86 原生)。
# 用法: ./scripts/build.sh [cmake 附加参数...]
#   交叉编译: ./scripts/build.sh -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake
# 环境变量: BUILD_DIR(默认 build)
set -euo pipefail
cd "$(dirname "$0")/.."   # 回到项目根: 脚本可被任意 cwd 调用

BUILD_DIR="${BUILD_DIR:-build}"
echo "==> cmake configure: ${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "$@"

echo "==> build"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "==> test"
if [ -f "${BUILD_DIR}/tests/CTestTestfile.cmake" ]; then
    (cd "${BUILD_DIR}" && ctest --output-on-failure)
else
    echo "    (tests 未加入构建: 交叉编译或 tests/ 尚未交付)"
fi
