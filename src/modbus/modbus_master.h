// src/modbus/modbus_master.h
// 职责: Modbus RTU 主站 —— 点表配置、轮询调度、单次读写事务、寄存器标度变换、
//       事务统计。每次事务 = 清缓冲 → 发请求 → 按帧长等待完整应答
//       (超时 = 3.5T 字符间隔 + 1000ms 兜底)→ CRC 校验 → 分类解析。
// 设计要点:
//  - 面向 ISerialDevice 接口编程: 真实串口与 PTY 虚拟串口无差别运行;
//  - 错误分类入 Stats: 超时(首字节未到/帧不完整)/ CRC 错 / 异常应答 /
//    从站地址不匹配 / 应答格式不符 —— demo 里注入故障后统计立即可见;
//  - 轮询调度: duePoints() 按各点 pollPeriodMs 返回到期索引,主循环只取到期点,
//    不同周期(500ms 温度 / 2000ms 泵速)天然交错,无需定时器;
//  - convertRaw: 16/32 位寄存器组合 + 符号扩展 + IEEE754 位搬移 + scale/offset,
//    把原始寄存器值变成带单位的物理量。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../hal/serial_device.h"

namespace es::modbus {

// 数据质量: 网关遥测上行时携带,Good 才参与告警判定
enum class Quality { Good, Bad, Stale };

// 一个采集/控制点(由 JSON 配置填充;运行时字段由网关/Master 更新)
struct ModbusPoint {
    std::string id;                  // 点 ID(配置唯一)
    std::string name;                // 中文名/描述
    uint8_t slaveId = 1;             // 从站地址(1..247)
    uint8_t func = 0x03;             // 03 读保持 / 04 读输入 / 06 写单寄存器
    uint16_t startAddr = 0;          // 起始寄存器地址(协议内 0 基)
    uint16_t count = 1;              // 寄存器个数(32 位类型需 >= 2)
    bool writable = false;           // func=06 的可写点(由网关选择读或写路径)
    bool is32Bit = false;            // true: 两个寄存器组合成一个 32 位量
    bool bigEndian = true;           // 32 位字序: true=高字在前(Modbus 惯例), false=低字在前
    std::string dataType = "u16";    // u16 / i16 / u32 / i32 / f32
    double scale = 1.0;              // 标度: value = raw * scale + offset
    double offset = 0.0;
    std::string unit;                // 工程单位,如 "C" / "MPa"
    uint32_t pollPeriodMs = 1000;    // 轮询周期
    // 告警阈值(交给 RuleEngine 使用;低限/高限可独立使能)
    double highAlarm = 0, lowAlarm = 0;
    bool highAlarmEnabled = false, lowAlarmEnabled = false;
    double hysteresis = 0;           // 迟滞带(防抖)
    // 运行时(由 Master/Gateway 更新,线程模型见 plan §11):
    double rawValue = 0;             // 原始寄存器换算前值
    double value = 0;                // 标度变换后物理量
    Quality quality = Quality::Bad;
    uint64_t lastUpdateMs = 0;       // 最近一次成功采集时间
    uint64_t errCount = 0;           // 连续/累计错误次数
};

class ModbusMaster {
public:
    // 主站不拥有串口,与从站共享同一 shared_ptr(PTY 演示时主站连 master、从站连 slave)
    explicit ModbusMaster(std::shared_ptr<ISerialDevice> port);

    // 建索引并校验点表(重复 id、非法功能码/数量/类型等在此拦截)
    bool configure(const std::vector<ModbusPoint>& points, std::string* err);

    // 单次读(03/04): 成功则 regs 填满 count 个寄存器
    bool readPoint(const ModbusPoint& p, std::vector<uint16_t>* regs, std::string* err);

    // 单次写(06): 应答为请求回显,逐一核对地址与数值
    bool writeRegister(const ModbusPoint& p, uint16_t value, std::string* err);

    // 寄存器 → 物理量: is32Bit 组合(字序按 bigEndian)、i16/i32 符号扩展、
    // f32 按 IEEE754 位搬移(memcpy,与主机字节序无关)、最后 scale/offset
    double convertRaw(const ModbusPoint& p, const std::vector<uint16_t>& regs, bool* ok) const;

    // 轮询调度: 返回所有"当前已到期"的点索引,并更新其内部上次轮询时间
    // (含 writable 点 —— 写点同样按 pollPeriodMs 参与调度)
    std::vector<size_t> duePoints();

    struct Stats {
        uint64_t txFrames = 0;       // 已发送请求帧
        uint64_t rxFrames = 0;       // 已收到完整应答帧(含异常/错地址)
        uint64_t timeouts = 0;       // 首字节超时 / 帧不完整(字符间隔 > 3.5T)
        uint64_t crcErrors = 0;      // 应答 CRC 校验失败
        uint64_t exceptions = 0;     // 收到异常应答(功能码 0x80|func)
    };
    [[nodiscard]] Stats stats() const;
    void resetStats();

    // 契约扩展: 设置 1 字符时间(µs),用于 3.5T 间隔与超时推导。
    // ISerialDevice 接口不暴露波特率,PTY 演示又无真实波特率,故由网关在
    // configure 后按配置波特率注入;默认按 9600 波特(11*1e6/9600 µs)。
    void setCharTimeUs(double us);

private:
    // 统一事务管线: flush → 发送 → 收齐应答(提前识别 5 字节异常应答) →
    // CRC → 分类校验。resp 收齐后才返回 true;错误原因写入 err。
    bool doTransaction(const ModbusPoint& p, const std::vector<uint8_t>& request,
                       size_t expectRespLen, std::vector<uint8_t>* resp, std::string* err);

    std::shared_ptr<ISerialDevice> m_port_;
    double m_charTimeUs_ = 11.0 * 1e6 / 9600.0;  // 默认 9600 波特
    std::vector<ModbusPoint> m_points_;
    std::vector<uint64_t> m_lastPollMs_;  // 与 m_points_ 对齐,单位 ms
    mutable std::mutex m_mutex_;          // 保护 m_points_/m_lastPollMs_/m_stats_
    std::mutex m_txMutex_;                // 串行化整笔事务(清缓冲→发请求→收应答):
                                          // 采集线程与命令线程(writeRegister)并发会撕裂 RS485 总线帧
    Stats m_stats_;
};

}  // namespace es::modbus
