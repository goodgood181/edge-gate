// src/hal/serial_device.h
// 职责: 串口设备抽象接口(依赖倒置: 上层 Modbus 协议栈只依赖本接口,
//       不关心底层是真实串口(/dev/ttyUSB0)还是 PTY 虚拟串口(/dev/pts/N))
// 设计要点:
//  - ISerialDevice 抽象是"x86 无硬件闭环演示"的前提: 主站/从站协议栈面向接口编程,
//    同一份协议代码在真实串口与 PTY 虚拟串口上运行完全相同的时序逻辑;
//  - read/write 带毫秒超时: 协议层需要"读若干字节、超时算失败"的语义,
//    裸 read() 不具备超时能力,必须由驱动层提供;
//  - read 返回值语义: >0 实际字节数 / 0 超时 / -1 错误,恰好对应 Modbus
//    事务的三种结局(收到数据/等待超时/链路错误),协议层据此分类统计。
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>  // ssize_t

namespace es {

// 串口配置: 与嵌入式串口初始化参数一一对应(波特率/数据位/校验/停止位)
struct SerialConfig {
    std::string device;  // "/dev/ttymxc2" / "/dev/ttyUSB0" / "pty-sim"(x86 演示,由应用层解析成 PTY)
    int baud = 9600;
    int dataBits = 8;    // 7 或 8
    char parity = 'N';   // 'N' 无校验 / 'E' 偶校验 / 'O' 奇校验
    int stopBits = 1;    // 1 或 2
};

class ISerialDevice {
public:
    virtual ~ISerialDevice() = default;

    virtual bool open(std::string* err) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    // 读取: >0 实际字节数(允许少于 len,上层按协议自行拼帧);
    //       0 表示超时(或对端关闭,PTY 下语义等价);-1 表示错误
    virtual ssize_t read(uint8_t* buf, size_t len, std::chrono::milliseconds timeout) = 0;
    // 写入: 返回实际写入字节数,调用方应检查是否等于 len
    virtual ssize_t write(const uint8_t* buf, size_t len, std::chrono::milliseconds timeout) = 0;
    // 丢弃收发缓冲: RS485 半双工总线方向切换前清掉残留字节,
    // 避免上一轮应答/噪声被误当成下一帧的数据
    virtual bool flush() = 0;

    [[nodiscard]] virtual const std::string& deviceName() const = 0;
    [[nodiscard]] virtual int fd() const = 0;
};

}  // namespace es
