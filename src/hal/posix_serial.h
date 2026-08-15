// src/hal/posix_serial.h
// 职责: 基于 termios 的 POSIX 串口实现,支持真实串口与 PTY 从端(/dev/pts/N),
//       并提供"包装已打开 fd(PTY 主端)"的扩展构造,支撑无硬件闭环演示。
// 设计要点: raw 模式配置、波特率映射、VMIN/VTIME 与 poll 超时取舍、
//       RS485 硬件方向控制 ioctl —— 详见 posix_serial.cpp 中逐项注释。
#pragma once

#include "serial_device.h"

namespace es {

class PosixSerial : public ISerialDevice {
public:
    // 标准构造: 按设备路径打开(真实串口 /dev/ttyUSB0,或 PTY 从端 /dev/pts/N)
    explicit PosixSerial(const SerialConfig& cfg);

    // 契约扩展(供 pty-sim 演示): 包装一个已打开的 fd(openpty 产生的 PTY 主端)。
    // PTY 主端没有设备路径,演示链路"主站连 master、从站连 slave"需要直接包装 fd;
    // 对 master fd 执行 tcsetattr 同样生效 —— PTY 主从端共享同一套终端行规程设置。
    PosixSerial(const SerialConfig& cfg, int preopenedFd);

    ~PosixSerial() override;

    PosixSerial(const PosixSerial&) = delete;
    PosixSerial& operator=(const PosixSerial&) = delete;

    bool open(std::string* err) override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;
    ssize_t read(uint8_t* buf, size_t len, std::chrono::milliseconds timeout) override;
    ssize_t write(const uint8_t* buf, size_t len, std::chrono::milliseconds timeout) override;
    bool flush() override;
    [[nodiscard]] const std::string& deviceName() const override;
    [[nodiscard]] int fd() const override;

private:
    bool configureTermios(int fd, std::string* err) const;

    SerialConfig m_cfg_;
    int m_fd_ = -1;        // -1 = 未打开
    bool m_wrapFd_ = false;  // true: 包装外部 fd(PTY 主端),close() 时同样关闭
};

}  // namespace es
