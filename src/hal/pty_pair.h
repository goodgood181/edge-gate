// src/hal/pty_pair.h
// 职责: 创建 PTY(伪终端)虚拟串口对 —— x86 无硬件闭环演示的基石。
// 设计要点: PTY 内核原理与"为什么用它做无硬件集成测试"见 pty_pair.cpp。
#pragma once

#include <string>

namespace es {

// 一对虚拟串口端点: master 是内核侧 fd(应用侧视角),slave 表现为 /dev/pts/N
struct PtyPair {
    int master = -1;        // 主端 fd(供主站使用;无路径,需按 fd 包装)
    int slave = -1;         // 从端 fd(已打开,可直接使用,或关闭后按 slaveName 重开)
    std::string slaveName;  // 从端设备路径(/dev/pts/N),可交给 PosixSerial 打开
};

// 创建 PTY 对;成功返回 true 并填充 out。调用方负责在退出时 close 两个 fd,
// 否则内核侧端口不会释放(演示进程生命周期内不关闭亦可)。
bool createPtyPair(PtyPair* out, std::string* err);

}  // namespace es
