# 文件路径: cmake/toolchain-arm-linux-gnueabihf.cmake
# 职责: 交叉编译工具链文件 —— 在 x86 开发机上编译 ARM Cortex-A7(IMX6ULL)
#       目标二进制。用法:
#     cmake -S . -B build-arm \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake
#     cmake --build build-arm
# 设计要点(面试可讲):
# 1) CMAKE_SYSTEM_NAME 必须显式设为 Linux: 否则 CMake 认为在编译本机,
#    不会按目标系统搜索库/头文件 —— 这是交叉编译的"分界开关";
# 2) FIND_ROOT_PATH 三件套: PROGRAM 保持 NEVER(编译期工具如 ar/ranlib 在
#    开发机),LIBRARY/INCLUDE/PACKAGE 限定到目标根 —— 防止误链开发机 x86
#    的 .so/.h(架构不匹配要到链接期才暴露,错误信息难懂);
# 3) CMAKE_CROSSCOMPILING 由 CMake 自动置位 → es_options.cmake 据此
#    默认关闭测试(目标板无 ctest 环境)。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# 目标系统根(Debian/Ubuntu 交叉工具链包的标准路径:
#   apt install g++-arm-linux-gnueabihf 后即存在)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
