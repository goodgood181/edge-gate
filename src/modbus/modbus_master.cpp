// src/modbus/modbus_master.cpp
// 职责: 主站事务实现。关键点:
//  - 半双工方向纪律: 每笔事务先 flush 清残留,再发请求 —— RS485 是共享介质,
//    上一轮的应答残留若不清掉,会被当成下一帧首字节;
//  - 收帧策略: 首字节等待 = 3.5T + 1000ms 兜底;后续字节等待 = 3.5T,
//    超过即判"帧不完整"(RTU 帧内字符间隔不允许超过 3.5T,见 modbus_rtu.h);
//  - 异常应答 5 字节即可提前收尾,不必等满正常帧长 —— 快速失败;
//  - 错误五分类: 超时 / CRC 错 / 异常应答 / 从站地址不匹配 / 应答格式不符,
//    前四类计入 Stats,格式不符返回 false(由 err 描述)。
#include "modbus_master.h"

#include <cstring>
#include <utility>

#include "modbus_crc.h"
#include "modbus_rtu.h"

namespace es::modbus {
namespace {

constexpr int kWriteTimeoutMs = 1000;   // 请求帧发送超时
constexpr int kMaxAdus = 260;           // RTU 最大 ADU 256 字节,留 4 字节余量
constexpr uint64_t kFirstByteExtraMs = 1000;  // 首字节兜底等待(见 doTransaction)

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 帧尾 2 字节 CRC 校验(线路序: 低字节在前)
bool crcOk(const std::vector<uint8_t>& frame) {
    if (frame.size() < 5) return false;
    const uint16_t expect = static_cast<uint16_t>((frame[frame.size() - 1] << 8) | frame[frame.size() - 2]);
    return crc16(frame.data(), frame.size() - 2) == expect;
}

// 1 字节转两位十六进制(仅用于错误信息可读性)
std::string byteHex(uint8_t v) {
    const char* hex = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    return s;
}

}  // namespace

ModbusMaster::ModbusMaster(std::shared_ptr<ISerialDevice> port) : m_port_(std::move(port)) {}

void ModbusMaster::setCharTimeUs(double us) {
    if (us > 0.0) m_charTimeUs_ = us;
}

bool ModbusMaster::configure(const std::vector<ModbusPoint>& points, std::string* err) {
    if (!m_port_) {
        if (err) *err = "串口对象为空";
        return false;
    }
    for (size_t i = 0; i < points.size(); ++i) {
        const ModbusPoint& p = points[i];
        if (p.id.empty()) {
            if (err) *err = "点表第 " + std::to_string(i) + " 项 id 为空";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (points[j].id == p.id) {
                if (err) *err = "点 id 重复: " + p.id;
                return false;
            }
        }
        if (p.slaveId == 0 || p.slaveId > 247) {  // 0 为广播地址,本项目不支持
            if (err) *err = "点 " + p.id + " 从站地址非法(1..247)";
            return false;
        }
        if (p.func != static_cast<uint8_t>(FuncCode::ReadHoldingRegisters) &&
            p.func != static_cast<uint8_t>(FuncCode::ReadInputRegisters) &&
            p.func != static_cast<uint8_t>(FuncCode::WriteSingleRegister)) {
            if (err) *err = "点 " + p.id + " 功能码不支持(仅 03/04/06)";
            return false;
        }
        if (p.func == static_cast<uint8_t>(FuncCode::WriteSingleRegister)) {
            if (p.count != 1) {
                if (err) *err = "点 " + p.id + " 为写单寄存器,count 必须为 1";
                return false;
            }
        } else if (p.count < 1 || p.count > 125) {  // 单帧读上限 125 寄存器
            if (err) *err = "点 " + p.id + " 读寄存器数量须在 1..125";
            return false;
        }
        if (p.is32Bit && p.count < 2) {
            if (err) *err = "点 " + p.id + " 为 32 位数据类型,count 须 >= 2";
            return false;
        }
        if (p.dataType != "u16" && p.dataType != "i16" && p.dataType != "u32" &&
            p.dataType != "i32" && p.dataType != "f32") {
            if (err) *err = "点 " + p.id + " dataType 未知: " + p.dataType;
            return false;
        }
        // S1: 32 位类型必须 is32Bit && count==2;16 位类型必须非 is32Bit(组合错误前置到配置期)
        const bool is32Type = (p.dataType == "u32" || p.dataType == "i32" || p.dataType == "f32");
        if (is32Type && (!p.is32Bit || p.count != 2)) {
            if (err) *err = "点 " + p.id + " 32 位数据类型要求 is32Bit=true 且 count==2";
            return false;
        }
        if (!is32Type && p.is32Bit) {
            if (err) *err = "点 " + p.id + " 16 位数据类型不允许 is32Bit=true";
            return false;
        }
        if (p.pollPeriodMs == 0) {
            if (err) *err = "点 " + p.id + " pollPeriodMs 必须 > 0";
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(m_mutex_);
    m_points_ = points;
    m_lastPollMs_.assign(m_points_.size(), 0);  // 全 0 → 首次 duePoints 全部到期
    m_stats_ = Stats{};                         // 重配置即重置统计
    return true;
}

bool ModbusMaster::doTransaction(const ModbusPoint& p, const std::vector<uint8_t>& request,
                                 size_t expectRespLen, std::vector<uint8_t>* resp,
                                 std::string* err) {
    if (!m_port_ || !m_port_->isOpen()) {
        if (err) *err = "串口未打开";
        return false;
    }
    // 事务级互斥: 采集线程与命令线程(writeRegister)可能并发进入,
    // 不加锁会让两笔请求的字节在 RS485 半双工总线上交错(撕裂帧)。
    // 事务最坏耗时 = 超时 3.5T+1s,阻塞可接受。
    std::lock_guard<std::mutex> txLk(m_txMutex_);
    // 1) 清缓冲: 丢弃上一轮残留(RS485 半双工方向切换纪律);失败不致命
    (void)m_port_->flush();

    // 2) 发请求
    const ssize_t wr = m_port_->write(request.data(), request.size(),
                                      std::chrono::milliseconds(kWriteTimeoutMs));
    if (wr != static_cast<ssize_t>(request.size())) {
        if (err) *err = "发送请求失败(已发 " + std::to_string(wr) + "/" +
                        std::to_string(request.size()) + " 字节)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex_);
        ++m_stats_.txFrames;
    }

    // 3) 收齐应答。
    //    3.5T 字符间隔: 帧内相邻字节间隔不得超过 3.5 个字符时间;
    //    首字节等待 = 3.5T + 1000ms 兜底(从站掉线/根本没上线时快速失败)。
    const uint64_t gapMs = static_cast<uint64_t>(3.5 * m_charTimeUs_ / 1000.0) + 1;
    const uint64_t firstByteDeadline = nowMs() + gapMs + kFirstByteExtraMs;

    std::vector<uint8_t> buf;
    buf.reserve(expectRespLen + 4);
    uint64_t lastByteMs = 0;
    while (buf.size() < expectRespLen) {
        // 提前识别异常应答: 5 字节且功能码带 0x80,无需等满正常帧长
        if (buf.size() >= 5 && (buf[1] & 0x80)) break;

        const uint64_t now = nowMs();
        uint64_t remain = 0;
        if (buf.empty()) {
            remain = (firstByteDeadline > now) ? (firstByteDeadline - now) : 0;
        } else {
            remain = (lastByteMs + gapMs > now) ? (lastByteMs + gapMs - now) : 0;
        }
        if (remain == 0) {
            std::lock_guard<std::mutex> lk(m_mutex_);
            ++m_stats_.timeouts;
            if (err) *err = buf.empty() ? "等待应答超时(首字节未到)"
                                        : "应答帧不完整(字符间隔超过 3.5T)";
            return false;
        }
        uint8_t tmp[64];
        const ssize_t n = m_port_->read(tmp, sizeof(tmp), std::chrono::milliseconds(remain));
        if (n < 0) {
            if (err) *err = "串口读失败";
            return false;
        }
        if (n == 0) continue;  // 本轮超时无数据,循环内按 deadline 重新计算
        lastByteMs = nowMs();
        buf.insert(buf.end(), tmp, tmp + n);
        if (buf.size() > kMaxAdus) {  // 防御: 数据粘连超出协议上限
            if (err) *err = "应答数据异常超长";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex_);
        ++m_stats_.rxFrames;
    }

    // 4) CRC 校验(帧尾 2 字节,低字节在前)
    if (!crcOk(buf)) {
        std::lock_guard<std::mutex> lk(m_mutex_);
        ++m_stats_.crcErrors;
        if (err) *err = "应答 CRC 校验失败";
        return false;
    }

    // 5) 分类解析: 从站地址 → 异常应答 → 功能码 → 应答格式
    if (buf[0] != p.slaveId) {
        if (err) *err = "从站地址不匹配(收到 0x" + byteHex(buf[0]) + ",期望 0x" +
                        byteHex(p.slaveId) + ")";
        return false;
    }
    if (buf[1] & 0x80) {
        if (buf[1] != (request[1] | 0x80)) {  // 异常功能码必须 = 请求功能码 | 0x80(防损坏/恶意从站伪装)
            if (err) *err = "异常应答功能码与请求不符(0x" + byteHex(buf[1]) + ")";
            return false;
        }
        std::lock_guard<std::mutex> lk(m_mutex_);
        ++m_stats_.exceptions;
        if (err) *err = "从站异常应答: 功能码 0x" + byteHex(buf[1]) + " 异常码 0x" +
                        byteHex(buf[2]);
        return false;
    }
    if (buf[1] != request[1]) {
        if (err) *err = "应答功能码与请求不符";
        return false;
    }
    if (buf.size() != expectRespLen) {
        if (err) *err = "应答长度不符(期望 " + std::to_string(expectRespLen) +
                        ",实际 " + std::to_string(buf.size()) + ")";
        return false;
    }
    if (p.func == static_cast<uint8_t>(FuncCode::WriteSingleRegister)) {
        // 06 应答 = 请求回显: 逐字节核对地址与数值
        for (size_t i = 2; i < 6; ++i) {
            if (buf[i] != request[i]) {
                if (err) *err = "写应答回显不符(第 " + std::to_string(i) + " 字节)";
                return false;
            }
        }
    } else {
        // 03/04 应答: 字节数字段必须等于 2*count
        if (buf[2] != static_cast<uint8_t>(2 * p.count)) {
            if (err) *err = "应答 byteCount 字段不符(期望 " +
                            std::to_string(2 * p.count) + ",实际 " + std::to_string(buf[2]) + ")";
            return false;
        }
    }
    if (resp != nullptr) *resp = std::move(buf);
    return true;
}

bool ModbusMaster::readPoint(const ModbusPoint& p, std::vector<uint16_t>* regs,
                             std::string* err) {
    if (p.func != static_cast<uint8_t>(FuncCode::ReadHoldingRegisters) &&
        p.func != static_cast<uint8_t>(FuncCode::ReadInputRegisters)) {
        if (err) *err = "readPoint 仅支持 03/04";
        return false;
    }
    if (regs == nullptr) {
        if (err) *err = "输出参数为空";
        return false;
    }
    // 请求数据段: 起始地址(高字节在前) + 寄存器数量(高字节在前)
    const std::vector<uint8_t> reqData = {
        static_cast<uint8_t>(p.startAddr >> 8), static_cast<uint8_t>(p.startAddr & 0xFF),
        static_cast<uint8_t>(p.count >> 8), static_cast<uint8_t>(p.count & 0xFF)};
    std::vector<uint8_t> request;
    if (!encodeFrame(p.slaveId, p.func, reqData, &request)) {
        if (err) *err = "组帧失败";
        return false;
    }
    std::vector<uint8_t> resp;
    if (!doTransaction(p, request, 5 + 2u * p.count, &resp, err)) return false;

    // 解析: 应答数据从偏移 3 起(1 地址 + 1 功能码 + 1 字节数),每寄存器 2 字节高前低后
    regs->clear();
    for (uint16_t i = 0; i < p.count; ++i) {
        regs->push_back(static_cast<uint16_t>((resp[3 + 2u * i] << 8) | resp[4 + 2u * i]));
    }
    return true;
}

bool ModbusMaster::writeRegister(const ModbusPoint& p, uint16_t value, std::string* err) {
    if (p.func != static_cast<uint8_t>(FuncCode::WriteSingleRegister)) {
        if (err) *err = "writeRegister 仅支持 06 写点";
        return false;
    }
    const std::vector<uint8_t> reqData = {
        static_cast<uint8_t>(p.startAddr >> 8), static_cast<uint8_t>(p.startAddr & 0xFF),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
    std::vector<uint8_t> request;
    if (!encodeFrame(p.slaveId, p.func, reqData, &request)) {
        if (err) *err = "组帧失败";
        return false;
    }
    std::vector<uint8_t> resp;
    if (!doTransaction(p, request, 8, &resp, err)) return false;
    return true;  // 回显核对已在 doTransaction 完成
}

double ModbusMaster::convertRaw(const ModbusPoint& p, const std::vector<uint16_t>& regs,
                                bool* ok) const {
    if (ok != nullptr) *ok = false;
    double raw = 0.0;
    if (p.is32Bit) {
        // 32 位组合: bigEndian = 高字在前(Modbus 惯例),否则低字在前
        if (regs.size() < 2) return 0.0;
        const uint32_t word = p.bigEndian ? (static_cast<uint32_t>(regs[0]) << 16 | regs[1])
                                          : (static_cast<uint32_t>(regs[1]) << 16 | regs[0]);
        if (p.dataType == "u32") {
            raw = static_cast<double>(word);
        } else if (p.dataType == "i32") {
            raw = static_cast<double>(static_cast<int32_t>(word));
        } else if (p.dataType == "f32") {
            // IEEE754 单精度: 按位搬移,memcpy 保证与主机字节序无关
            float f = 0.0f;
            std::memcpy(&f, &word, sizeof(f));
            raw = static_cast<double>(f);
        } else {
            return 0.0;  // 16 位类型却配 is32Bit: 配置错误,ok 保持 false
        }
    } else {
        if (regs.empty()) return 0.0;
        if (p.dataType == "u16") {
            raw = static_cast<double>(regs[0]);
        } else if (p.dataType == "i16") {
            raw = static_cast<double>(static_cast<int16_t>(regs[0]));
        } else {
            return 0.0;  // 32 位类型却未配 is32Bit: 配置错误
        }
    }
    if (ok != nullptr) *ok = true;
    return raw * p.scale + p.offset;  // 标度变换: 原始值 → 物理量
}

std::vector<size_t> ModbusMaster::duePoints() {
    std::vector<size_t> due;
    const uint64_t now = nowMs();
    std::lock_guard<std::mutex> lk(m_mutex_);
    for (size_t i = 0; i < m_points_.size(); ++i) {
        if (now - m_lastPollMs_[i] >= m_points_[i].pollPeriodMs) {
            m_lastPollMs_[i] = now;  // 本次到期立即记时,避免下一轮重复返回
            due.push_back(i);
        }
    }
    return due;
}

ModbusMaster::Stats ModbusMaster::stats() const {
    std::lock_guard<std::mutex> lk(m_mutex_);
    return m_stats_;
}

void ModbusMaster::resetStats() {
    std::lock_guard<std::mutex> lk(m_mutex_);
    m_stats_ = Stats{};
}

}  // namespace es::modbus
