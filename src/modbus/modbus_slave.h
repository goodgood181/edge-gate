// src/modbus/modbus_slave.h
// 职责: 软件 Modbus RTU 从站 —— 内存寄存器堆 + 独立线程按 RTU 时序应答,
//       用于 PTY 无硬件闭环演示与故障注入测试(demo 亮点)。
// 设计要点:
//  - 真实从站行为还原: 按 3.5T 静默间隔切帧(不是按固定字节数猜帧)、
//    CRC 错帧静默丢弃、地址不符静默丢弃、非法请求回异常应答 ——
//    主站侧的"超时/CRC 错/异常/错地址"统计全靠这些行为才能被真实触发;
//  - 故障注入: none / crc / no_response / exception / wrong_slave 五种模式,
//    一键让链路"坏掉",观察主站 Stats 变化与恢复 —— 演示 30 秒出效果;
//  - 线程可退出: 读轮询以 3.5T 为粒度,stop() 置位后线程最迟一个轮询周期内
//    自行退出并 join,无阻塞读,不依赖关闭 fd 来打断。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../hal/serial_device.h"

namespace es::modbus {

class ModbusSlave {
public:
    // 与主站共享同一 ISerialDevice(PTY 演示: 主站连 master 端、从站连 slave 端)
    ModbusSlave(std::shared_ptr<ISerialDevice> port, uint8_t slaveId);
    ~ModbusSlave();

    ModbusSlave(const ModbusSlave&) = delete;
    ModbusSlave& operator=(const ModbusSlave&) = delete;

    bool start(std::string* err);   // 启动应答线程
    void stop();                    // 置退出标志并 join,可重复调用
    [[nodiscard]] bool running() const;

    void setRegisterCount(uint16_t count);        // 保持寄存器堆大小(默认 64)
    bool setRegister(uint16_t addr, uint16_t value);  // 测试/演示注入寄存器值
    [[nodiscard]] bool getRegister(uint16_t addr, uint16_t* value) const;

    // 故障注入: none | crc(应答带错 CRC) | no_response(不应答)
    //          | exception(回异常码 0x02) | wrong_slave(用错误从站地址应答)
    void injectFault(const std::string& fault);
    [[nodiscard]] std::string fault() const;

    [[nodiscard]] uint64_t requestCount() const;  // 已处理的合法请求数

    // 契约扩展: 设置 1 字符时间(µs)用于 3.5T 静默间隔检测(默认 9600 波特对应值),
    // 由网关按配置波特率注入;PTY 上无真实波特率,只能由应用层指定。
    void setCharTimeUs(double us);

private:
    enum class FaultType { None, CrcError, NoResponse, Exception, WrongSlave };

    void threadLoop();
    void handleFrame(const std::vector<uint8_t>& frame);
    void sendFrame(uint8_t slaveId, uint8_t func, const std::vector<uint8_t>& data);
    void sendFrame(const std::vector<uint8_t>& frame);

    std::shared_ptr<ISerialDevice> m_port_;
    uint8_t m_slaveId_;
    double m_charTimeUs_ = 11.0 * 1e6 / 9600.0;  // 默认 9600 波特

    mutable std::mutex m_stateMutex_;  // 保护 m_running_
    bool m_running_ = false;
    std::thread m_thread_;

    mutable std::mutex m_regsMutex_;   // 保护寄存器堆
    std::vector<uint16_t> m_regs_;     // 保持寄存器堆(默认 64)

    std::atomic<FaultType> m_fault_{FaultType::None};
    std::atomic<uint64_t> m_requestCount_{0};
};

}  // namespace es::modbus
