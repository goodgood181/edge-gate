// src/hal/posix_serial.cpp
// 职责: termios 串口实现(真实串口与 PTY 通用)。
// 设计要点:
//  1) raw 模式: 关闭 ICANON/ECHO/ISIG/OPOST/IXON 等行规程加工,字节原样进出。
//     这是 Modbus RTU 二进制帧的前提 —— 规范模式会把 0x0A 当"换行"处理、
//     会把 0x0D 转成 0x0A、还会在输入里插入回显,任何一个都会撕碎 RTU 帧;
//  2) VMIN/VTIME 取舍: VTIME 的单位是 0.1 秒(deci-second),无法表达毫秒级
//     超时;且若与 poll 超时并用,会出现"VTIME 等完又等 poll"的双重超时叠加。
//     故 VMIN=0/VTIME=0 关闭字符定时,统一由 poll() 提供毫秒级超时 ——
//     语义单一、精度可控(Modbus 主站按 3.5T 字符间隔算超时需要毫秒级精度);
//  3) O_NONBLOCK + poll: open 即置非阻塞,read/write 前先 poll 就绪,
//     保证任意超时语义都能精确实现,也避免某些驱动在 open 时被 DCD 载波阻塞;
//  4) RS485 方向控制: TIOCGRS485/TIOCSRS485 让内核在发送时自动拉高 RTS 驱动
//     收发器的 DE 脚,半双工方向切换由硬件完成,避免应用层掐 RTS 的时序抖动;
//     非 RS485 硬件/PTY 上 ioctl 失败,仅忽略(等价于降级为软件方向控制)。
#include "posix_serial.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/serial.h>  // struct serial_rs485 / TIOCGRS485 / TIOCSRS485
#endif

namespace es {
namespace {

// 波特率 → termios speed_t 常量映射。
// termios 不允许直接填数字,必须使用 Bxxxx 宏;B0 在 POSIX 里有"挂断线路"的特殊
// 含义,因此用作非法波特率的哨兵值。
speed_t baudToSpeed(int baud) {
    switch (baud) {
        case 50: return B50;          case 75: return B75;
        case 110: return B110;        case 134: return B134;
        case 150: return B150;        case 200: return B200;
        case 300: return B300;        case 600: return B600;
        case 1200: return B1200;      case 1800: return B1800;
        case 2400: return B2400;      case 4800: return B4800;
        case 9600: return B9600;      case 19200: return B19200;
        case 38400: return B38400;    case 57600: return B57600;
        case 115200: return B115200;  case 230400: return B230400;
        case 460800: return B460800;  case 500000: return B500000;
        case 576000: return B576000;  case 921600: return B921600;
        case 1000000: return B1000000; case 1152000: return B1152000;
        case 1500000: return B1500000; case 2000000: return B2000000;
        case 2500000: return B2500000; case 3000000: return B3000000;
        case 3500000: return B3500000; case 4000000: return B4000000;
        default: return B0;
    }
}

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

PosixSerial::PosixSerial(const SerialConfig& cfg) : m_cfg_(cfg) {}

PosixSerial::PosixSerial(const SerialConfig& cfg, int preopenedFd)
    : m_cfg_(cfg), m_fd_(preopenedFd), m_wrapFd_(true) {}

PosixSerial::~PosixSerial() { close(); }

bool PosixSerial::open(std::string* err) {
    if (m_wrapFd_) {
        // 包装外部 fd(PTY 主端): 不再 open,只做 termios 配置。
        // PTY 主从端共享同一终端状态,tcsetattr(master) 即配置整对虚拟串口。
        if (m_fd_ < 0) {
            if (err) *err = "包装的 fd 非法";
            return false;
        }
        if (!configureTermios(m_fd_, err)) return false;
        return true;
    }
    if (m_fd_ >= 0) {
        if (err) *err = "串口已打开";
        return false;
    }
    if (m_cfg_.device.empty()) {
        if (err) *err = "设备路径为空";
        return false;
    }
    // O_NONBLOCK: 避免 open 时被调制解调器状态线阻塞;O_NOCTTY: 防止误成为控制终端
    int fd = ::open(m_cfg_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = "打开串口 " + m_cfg_.device + " 失败: " + std::strerror(errno);
        return false;
    }
    if (!configureTermios(fd, err)) {
        ::close(fd);
        return false;
    }
    m_fd_ = fd;
    return true;
}

bool PosixSerial::configureTermios(int fd, std::string* err) const {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        if (err) *err = std::string("tcgetattr 失败: ") + std::strerror(errno);
        return false;
    }

    const speed_t speed = baudToSpeed(m_cfg_.baud);
    if (speed == B0) {
        if (err) *err = "不支持的波特率 " + std::to_string(m_cfg_.baud);
        return false;
    }
    // 收发分别设置波特率(多数驱动等价,分开写更严谨)
    if (::cfsetispeed(&tio, speed) != 0 || ::cfsetospeed(&tio, speed) != 0) {
        if (err) *err = "cfsetispeed/cfsetospeed 失败";
        return false;
    }

    // ---- raw 模式(逐位设置,等价于 cfmakeraw();手动设置便于逐项讲解) ----
    // 输入加工: 关掉奇偶错标记(PARMRK)、8 位剥离(ISTRIP)、换行/回车转换
    // (INLCR/IGNCR/ICRNL)、信号字符(BRKINT/IGNBRK)、软件流控(IXON —— 否则
    // 收到 0x11/0x13 会被当成 XON/XOFF 吞掉,直接破坏 Modbus 帧)
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    // 输出加工: 关闭 OPOST(否则内核会把 \n 展开成 \r\n,帧字节被改写)
    tio.c_oflag &= ~OPOST;
    // 行规程: 关闭规范模式(ICANON,否则要等换行才交数据)、回显(ECHO/ECHONL)、
    // 信号键(ISIG,否则 Ctrl-C 之类会触发信号)、扩展处理(IEXTEN)
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    // 控制位: 数据位/校验位/停止位/硬件流控全部重设
    tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    if (m_cfg_.dataBits == 7) {
        tio.c_cflag |= CS7;
    } else if (m_cfg_.dataBits == 8) {
        tio.c_cflag |= CS8;
    } else {
        if (err) *err = "数据位仅支持 7/8";
        return false;
    }
    if (m_cfg_.parity == 'E') {
        tio.c_cflag |= PARENB;                    // 偶校验: PARENB=1, PARODD=0
    } else if (m_cfg_.parity == 'O') {
        tio.c_cflag |= PARENB | PARODD;           // 奇校验: 两者均置 1
    } else if (m_cfg_.parity != 'N') {
        if (err) *err = "校验位仅支持 N/E/O";
        return false;
    }
    if (m_cfg_.stopBits == 2) {
        tio.c_cflag |= CSTOPB;
    } else if (m_cfg_.stopBits != 1) {
        if (err) *err = "停止位仅支持 1/2";
        return false;
    }
    // CREAD 使能接收;CLOCAL 忽略调制解调器状态线(工业串口常不接 DCD)
    tio.c_cflag |= CREAD | CLOCAL;

    // VMIN/VTIME 取舍: VTIME 单位是 0.1s,无法表达毫秒级超时;与 poll 并用还会
    // 形成"VTIME 计时 + poll 计时"的双重等待。因此 VMIN=0/VTIME=0 关闭字符定时,
    // 统一由 poll() 提供毫秒级超时 —— 精度更高、语义单一(见 read/write)。
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    // TCSANOW: 立即生效,不等待收发缓冲排空(启动阶段无在途数据)
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (err) *err = std::string("tcsetattr 失败: ") + std::strerror(errno);
        return false;
    }

#ifdef __linux__
    // RS485 硬件方向控制(可选): 内核在 TX 期间自动拉高 RTS 驱动收发器 DE 脚,
    // 半双工方向切换无需应用层参与,消除软件掐 RTS 的时序抖动。
    // 非 RS485 硬件(普通 USB 转串口)/PTY 上 ioctl 返回 -1,仅忽略。
    struct serial_rs485 rs485;
    if (::ioctl(fd, TIOCGRS485, &rs485) == 0) {
        rs485.flags |= SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
        rs485.delay_rts_before_send = 0;
        rs485.delay_rts_after_send = 0;
        (void)::ioctl(fd, TIOCSRS485, &rs485);  // 失败仅警告(静默忽略)
    }
#endif
    return true;
}

void PosixSerial::close() {
    if (m_fd_ >= 0) {
        ::close(m_fd_);
        m_fd_ = -1;
    }
}

bool PosixSerial::isOpen() const { return m_fd_ >= 0; }

ssize_t PosixSerial::read(uint8_t* buf, size_t len, std::chrono::milliseconds timeout) {
    if (m_fd_ < 0 || buf == nullptr || len == 0) return -1;
    // 超时预算按截止时刻递减: 反复 EINTR/EAGAIN 时总等待时间有界(M4)
    const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeout.count());
    while (true) {
        const uint64_t now = nowMs();
        const int remain = (deadline > now) ? static_cast<int>(deadline - now) : 0;
        pollfd pfd{m_fd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, remain);
        if (rc < 0) {
            if (errno == EINTR) continue;  // 信号打断: 按剩余预算重新等待
            return -1;
        }
        if (rc == 0) return 0;  // 超时: 无数据
        if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
        // POLLHUP(对端关闭)不在此处判定: 先尝试把缓冲残留读尽,read 返回 0 即 EOF
        ssize_t n = ::read(m_fd_, buf, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        return n;  // n==0 表示对端关闭(PTY 从端已关);协议层按"无数据"处理
    }
}

ssize_t PosixSerial::write(const uint8_t* buf, size_t len, std::chrono::milliseconds timeout) {
    if (m_fd_ < 0 || buf == nullptr) return -1;
    if (len == 0) return 0;
    const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeout.count());
    size_t sent = 0;
    while (sent < len) {
        const uint64_t now = nowMs();
        const int remain = (deadline > now) ? static_cast<int>(deadline - now) : 0;
        pollfd pfd{m_fd_, POLLOUT, 0};
        const int rc = ::poll(&pfd, 1, remain);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return sent ? static_cast<ssize_t>(sent) : -1;
        }
        if (rc == 0) return static_cast<ssize_t>(sent);  // 超时: 返回已发送部分
        if (pfd.revents & (POLLERR | POLLNVAL)) return sent ? static_cast<ssize_t>(sent) : -1;
        const ssize_t n = ::write(m_fd_, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return sent ? static_cast<ssize_t>(sent) : -1;
        }
        if (n == 0) return static_cast<ssize_t>(sent);  // 写 0 字节属异常,防死循环
        sent += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

bool PosixSerial::flush() {
    if (m_fd_ < 0) return false;
    return ::tcflush(m_fd_, TCIOFLUSH) == 0;  // TCIOFLUSH: 收发缓冲一并丢弃
}

const std::string& PosixSerial::deviceName() const { return m_cfg_.device; }

int PosixSerial::fd() const { return m_fd_; }

}  // namespace es
