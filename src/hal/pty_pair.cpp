// src/hal/pty_pair.cpp
// 职责: 用 POSIX 标准接口(posix_openpt/grantpt/unlockpt/ptsname)创建 PTY 对。
// 设计要点:
//  1) PTY 内核原理: PTY 是内核提供的"虚拟终端对"。主端(master fd)是内核侧的
//     匿名端点,从端(slave)注册为字符设备 /dev/pts/N,带完整的终端语义
//     (termios 行规程、行缓冲、回显等)。主端写入的数据出现在从端的输入队列,
//     从端写入的数据出现在主端的读侧 —— 数据通路类似管道,但多了终端层。
//     xterm 等终端模拟器就是"主端进程 + 从端挂 shell"的经典用法;
//  2) 为什么用它做无硬件集成测试: Modbus 主站 ↔ 从站的真实物理链路是
//     RS485 总线(半双工、共享介质)。x86 上买不到串口硬件时,PTY 提供了
//     一对"真实存在的终端端点": 主站连 master、软件从站连 slave,两侧都走
//     完全真实的 termios 配置 + poll 超时 + 字节流 IO —— 协议栈的
//     组帧/拆帧/CRC/3.5T 时序逻辑与真实串口完全同路径执行,只差物理电平;
//     因此"PTY 闭环通过"能证明协议栈正确性,而无需硬件;
//  3) 为何不用 socketpair/管道: 管道没有终端语义,无法验证 termios 路径
//     (raw 模式、波特率配置、tcflush 等驱动级代码);PTY 才走完整串口驱动栈。
//  4) 实现选型: glibc 的 openpty() 更简洁但依赖 _GNU_SOURCE 与 -lutil,
//     posix_openpt 链是 POSIX.1-2001 标准接口,零额外依赖,可移植性更好。
#include "pty_pair.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace es {

bool createPtyPair(PtyPair* out, std::string* err) {
    if (out == nullptr) {
        if (err) *err = "输出参数为空";
        return false;
    }
    out->master = -1;
    out->slave = -1;
    out->slaveName.clear();

    // 1) 打开主端。O_NOCTTY: 防止后续成为进程控制终端;O_CLOEXEC: 防 fd 泄漏到子进程
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        if (err) *err = std::string("posix_openpt 失败: ") + std::strerror(errno);
        return false;
    }
    // 2) grantpt: 把从端设备属主改为当前用户、权限设为 0620(仅属主可读写)
    if (::grantpt(master) != 0) {
        if (err) *err = std::string("grantpt 失败: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    // 3) unlockpt: 清除"从端未解锁"标志,否则从端 open 会被拒绝
    if (::unlockpt(master) != 0) {
        if (err) *err = std::string("unlockpt 失败: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    // 4) ptsname: 取从端路径(返回静态缓冲,立即拷贝);也可用 ptsname_r 避免竞争
    const char* name = ::ptsname(master);
    if (name == nullptr) {
        if (err) *err = std::string("ptsname 失败: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    // 5) 打开从端(O_NONBLOCK: 与 PosixSerial 的 poll 模型一致)
    const int slave = ::open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (slave < 0) {
        if (err) *err = std::string("打开从端 ") + name + " 失败: " + std::strerror(errno);
        ::close(master);
        return false;
    }
    // 6) 主端也统一为非阻塞(主端默认阻塞模式)
    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }

    out->master = master;
    out->slave = slave;
    out->slaveName = name;
    return true;
}

}  // namespace es
