// src/modbus/modbus_slave.cpp
// 职责: 软件从站实现。线程模型:
//   应答线程循环 = poll 读(粒度 3.5T)→ 字节累积 → 静默 >= 3.5T 判定帧结束 →
//   CRC 校验(失败静默丢弃)→ decodeFrame → 地址过滤 → 功能码分发(03/04 读堆、
//   06/16 写堆)→ 组帧应答(可叠加故障注入)→ 写回串口。
// 与真实从站的差异: 寄存器在内存(RAM)而非 EEPROM/物理 IO,协议行为完全一致。
#include "modbus_slave.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "modbus_crc.h"
#include "modbus_rtu.h"

namespace es::modbus {
namespace {

constexpr size_t kMaxFrame = 260;        // 最大 ADU 256,留余量
constexpr uint16_t kDefaultRegCount = 64;
constexpr int kSendTimeoutMs = 1000;     // 应答发送超时

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool crcOk(const uint8_t* frame, size_t len) {
    if (len < 5) return false;
    const uint16_t expect = static_cast<uint16_t>((frame[len - 1] << 8) | frame[len - 2]);
    return crc16(frame, len - 2) == expect;
}

}  // namespace

ModbusSlave::ModbusSlave(std::shared_ptr<ISerialDevice> port, uint8_t slaveId)
    : m_port_(std::move(port)), m_slaveId_(slaveId), m_regs_(kDefaultRegCount, 0) {}

ModbusSlave::~ModbusSlave() { stop(); }

void ModbusSlave::setCharTimeUs(double us) {
    if (us > 0.0) m_charTimeUs_ = us;
}

bool ModbusSlave::start(std::string* err) {
    if (!m_port_) {
        if (err) *err = "串口对象为空";
        return false;
    }
    if (!m_port_->isOpen()) {
        if (err) *err = "串口未打开";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_stateMutex_);
        if (m_running_) {
            if (err) *err = "从站已在运行";
            return false;
        }
        m_running_ = true;
    }
    // 说明: std::thread 构造仅在系统资源耗尽时抛 system_error,正常流程不会触发;
    // 契约"禁异常"约束的是业务逻辑不主动抛错,此处依赖标准库行为(与 std::mutex 同理)。
    m_thread_ = std::thread(&ModbusSlave::threadLoop, this);
    return true;
}

void ModbusSlave::stop() {
    {
        std::lock_guard<std::mutex> lk(m_stateMutex_);
        m_running_ = false;
    }
    if (m_thread_.joinable()) m_thread_.join();
}

bool ModbusSlave::running() const {
    std::lock_guard<std::mutex> lk(m_stateMutex_);
    return m_running_;
}

void ModbusSlave::setRegisterCount(uint16_t count) {
    std::lock_guard<std::mutex> lk(m_regsMutex_);
    if (count == 0) count = 1;
    m_regs_.resize(count, 0);  // 保留原有值,新增补 0
}

bool ModbusSlave::setRegister(uint16_t addr, uint16_t value) {
    std::lock_guard<std::mutex> lk(m_regsMutex_);
    if (addr >= m_regs_.size()) return false;
    m_regs_[addr] = value;
    return true;
}

bool ModbusSlave::getRegister(uint16_t addr, uint16_t* value) const {
    if (value == nullptr) return false;
    std::lock_guard<std::mutex> lk(m_regsMutex_);
    if (addr >= m_regs_.size()) return false;
    *value = m_regs_[addr];
    return true;
}

void ModbusSlave::injectFault(const std::string& fault) {
    if (fault == "none") m_fault_.store(FaultType::None, std::memory_order_relaxed);
    else if (fault == "crc") m_fault_.store(FaultType::CrcError, std::memory_order_relaxed);
    else if (fault == "no_response") m_fault_.store(FaultType::NoResponse, std::memory_order_relaxed);
    else if (fault == "exception") m_fault_.store(FaultType::Exception, std::memory_order_relaxed);
    else if (fault == "wrong_slave") m_fault_.store(FaultType::WrongSlave, std::memory_order_relaxed);
    // 未知模式: 忽略,保持现状(由调用方保证合法取值)
}

std::string ModbusSlave::fault() const {
    switch (m_fault_.load(std::memory_order_relaxed)) {
        case FaultType::None: return "none";
        case FaultType::CrcError: return "crc";
        case FaultType::NoResponse: return "no_response";
        case FaultType::Exception: return "exception";
        case FaultType::WrongSlave: return "wrong_slave";
    }
    return "none";
}

uint64_t ModbusSlave::requestCount() const {
    return m_requestCount_.load(std::memory_order_relaxed);
}

void ModbusSlave::threadLoop() {
    // 3.5T 静默间隔: 帧内字符间隔必须 < 3.5 字符时间,帧间静默必须 >= 3.5T。
    // 轮询粒度取 3.5T 向上取整 —— 一旦 poll 超时返回,即可断定静默 >= 3.5T。
    const uint64_t gapMs = static_cast<uint64_t>(3.5 * m_charTimeUs_ / 1000.0) + 1;
    int pollMs = static_cast<int>((gapMs + 999) / 1000);
    if (pollMs < 1) pollMs = 1;

    std::vector<uint8_t> buf;
    buf.reserve(kMaxFrame);
    uint64_t lastByteMs = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lk(m_stateMutex_);
            if (!m_running_) break;  // 退出标志: 最多一个轮询周期内响应
        }
        uint8_t tmp[64];
        const ssize_t n = m_port_->read(tmp, sizeof(tmp), std::chrono::milliseconds(pollMs));
        if (n > 0) {
            const uint64_t now = nowMs();
            if (!buf.empty() && now - lastByteMs > gapMs) {
                // 帧内字节间隔 > 3.5T: 前帧已结束,新字节属于下一帧 → 先处理前帧再开新帧
                handleFrame(buf);
                buf.clear();
            }
            buf.insert(buf.end(), tmp, tmp + n);
            // 锚点 = 最后收到字节(而非帧首): 真机逐字节到达(9600 波特 ≈ 1.15ms/字符),
            // 若按帧首计时会过早判定 3.5T 静默,把整帧误切成半帧 → CRC 失败 → 全丢。
            lastByteMs = now;
            if (buf.size() > kMaxFrame) buf.clear();  // 超长丢弃: 协议已错乱,等 3.5T 重同步
        } else if (n == 0) {
            // poll 超时 = 静默 >= 3.5T: 若手头有半帧,判定为一帧结束
            if (!buf.empty() && nowMs() - lastByteMs >= gapMs) {
                handleFrame(buf);
                buf.clear();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 读错误: 退避重试
        }
    }
}

void ModbusSlave::handleFrame(const std::vector<uint8_t>& frame) {
    // 1) 最小帧长 + CRC 校验: 错帧按 RTU 规范静默丢弃(不回任何东西,不计数)
    if (frame.size() < 5) return;
    if (!crcOk(frame.data(), frame.size())) return;

    // 2) 结构解析(输入已按 3.5T 切帧且 CRC 通过,decodeFrame 必能定长)
    RtuFrame req;
    size_t frameLen = 0;
    std::string derr;
    if (decodeFrame(frame.data(), frame.size(), &frameLen, &req, &derr) != DecodeResult::Ok) return;

    // 3) 地址过滤: 只响应自己的从站地址(他人请求/广播静默丢弃)
    if (req.slaveId != m_slaveId_) return;
    m_requestCount_.fetch_add(1, std::memory_order_relaxed);

    const FaultType fault = m_fault_.load(std::memory_order_relaxed);

    // 4) 故障注入(在应答路径上做手脚,协议处理流程保持真实)
    if (fault == FaultType::NoResponse) return;  // 不应答 → 主站侧表现为超时

    std::vector<uint8_t> respData;
    uint8_t respFunc = req.func;
    bool ok = true;

    // 5) 功能码分发
    switch (req.func) {
        case static_cast<uint8_t>(FuncCode::ReadHoldingRegisters):
        case static_cast<uint8_t>(FuncCode::ReadInputRegisters): {
            // 请求数据段: 起始地址(2B) + 数量(2B)
            const uint16_t addr = static_cast<uint16_t>((req.data[0] << 8) | req.data[1]);
            const uint16_t cnt = static_cast<uint16_t>((req.data[2] << 8) | req.data[3]);
            if (cnt == 0 || cnt > 125) {
                // 数量非法 → 异常码 0x03(IllegalDataValue)
                ok = false;
                respFunc = static_cast<uint8_t>(req.func | 0x80);
                respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataValue)};
                break;
            }
            {
                std::lock_guard<std::mutex> lk(m_regsMutex_);
                if (static_cast<uint32_t>(addr) + cnt > m_regs_.size()) {
                    // 地址越界 → 异常码 0x02(IllegalDataAddress)
                    ok = false;
                    respFunc = static_cast<uint8_t>(req.func | 0x80);
                    respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataAddress)};
                    break;
                }
                // 正常应答: [字节数=2N][寄存器值,每寄存器高字节在前]
                respData.reserve(1 + 2u * cnt);
                respData.push_back(static_cast<uint8_t>(2 * cnt));
                for (uint16_t i = 0; i < cnt; ++i) {
                    respData.push_back(static_cast<uint8_t>(m_regs_[addr + i] >> 8));
                    respData.push_back(static_cast<uint8_t>(m_regs_[addr + i] & 0xFF));
                }
            }
            break;
        }
        case static_cast<uint8_t>(FuncCode::WriteSingleRegister): {
            // 请求数据段: 地址(2B) + 数值(2B);应答 = 请求回显
            const uint16_t addr = static_cast<uint16_t>((req.data[0] << 8) | req.data[1]);
            const uint16_t value = static_cast<uint16_t>((req.data[2] << 8) | req.data[3]);
            {
                std::lock_guard<std::mutex> lk(m_regsMutex_);
                if (addr >= m_regs_.size()) {
                    ok = false;
                    respFunc = static_cast<uint8_t>(req.func | 0x80);
                    respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataAddress)};
                    break;
                }
                m_regs_[addr] = value;
            }
            respData = req.data;  // 回显 [地址][数值]
            break;
        }
        case static_cast<uint8_t>(FuncCode::WriteMultipleRegisters): {
            // 请求数据段: 地址(2B) + 数量(2B) + 字节数(1B) + 数据(2N)
            if (req.data.size() < 7) {
                ok = false;
                respFunc = static_cast<uint8_t>(req.func | 0x80);
                respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataValue)};
                break;
            }
            const uint16_t addr = static_cast<uint16_t>((req.data[0] << 8) | req.data[1]);
            const uint16_t cnt = static_cast<uint16_t>((req.data[2] << 8) | req.data[3]);
            const uint8_t byteCount = req.data[4];
            if (cnt == 0 || cnt > 123 || byteCount != 2 * cnt) {
                ok = false;
                respFunc = static_cast<uint8_t>(req.func | 0x80);
                respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataValue)};
                break;
            }
            {
                std::lock_guard<std::mutex> lk(m_regsMutex_);
                if (static_cast<uint32_t>(addr) + cnt > m_regs_.size()) {
                    ok = false;
                    respFunc = static_cast<uint8_t>(req.func | 0x80);
                    respData = {static_cast<uint8_t>(ExceptionCode::IllegalDataAddress)};
                    break;
                }
                for (uint16_t i = 0; i < cnt; ++i) {
                    m_regs_[addr + i] = static_cast<uint16_t>(
                        (req.data[5 + 2u * i] << 8) | req.data[6 + 2u * i]);
                }
            }
            // 应答: 回显 [起始地址][数量]
            respData = {req.data[0], req.data[1], req.data[2], req.data[3]};
            break;
        }
        default:
            // 未知功能码 → 异常码 0x01(IllegalFunction)
            ok = false;
            respFunc = static_cast<uint8_t>(req.func | 0x80);
            respData = {static_cast<uint8_t>(ExceptionCode::IllegalFunction)};
            break;
    }

    if (!ok) {
        sendFrame(m_slaveId_, respFunc, respData);  // 异常应答(CRC 正确)
        return;
    }
    // 6) 故障注入叠加:
    //    exception: 即使请求合法也回异常 0x02(主站 exceptions++);
    //    wrong_slave: 换一个从站地址应答(主站报"从站地址不匹配");
    //    crc: 正常组帧后破坏 CRC(主站 crcErrors++)。
    uint8_t respSlave = m_slaveId_;
    if (fault == FaultType::Exception) {
        sendFrame(m_slaveId_, static_cast<uint8_t>(req.func | 0x80),
                  {static_cast<uint8_t>(ExceptionCode::IllegalDataAddress)});
        return;
    }
    if (fault == FaultType::WrongSlave) {
        respSlave = (m_slaveId_ == 255) ? 1 : static_cast<uint8_t>(m_slaveId_ + 1);
    }
    std::vector<uint8_t> resp;
    if (!encodeFrame(respSlave, respFunc, respData, &resp)) return;
    if (fault == FaultType::CrcError && resp.size() >= 2) {
        resp[resp.size() - 1] ^= 0xFF;  // 破坏 CRC 高字节,制造校验失败
    }
    sendFrame(resp);
}

void ModbusSlave::sendFrame(uint8_t slaveId, uint8_t func, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    if (!encodeFrame(slaveId, func, data, &frame)) return;
    sendFrame(frame);
}

void ModbusSlave::sendFrame(const std::vector<uint8_t>& frame) {
    // 应答发送失败(对端已关等)不重试: 主站侧自然表现为超时
    (void)m_port_->write(frame.data(), frame.size(), std::chrono::milliseconds(kSendTimeoutMs));
}

}  // namespace es::modbus
