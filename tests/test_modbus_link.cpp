// 文件路径: tests/test_modbus_link.cpp
// 意图: PTY 虚拟串口 + 主站 + 软件从站全链路集成测试(契约 §14 核心亮点)。
//       真实走 termios 时序: 主站连 PTY master 端、从站连 slave 端,
//       协议栈的组帧/拆帧/CRC/3.5T 静默/超时逻辑与真实串口完全同路径。
// 覆盖点:
//  - 03 读 4 寄存器 / 04 读输入寄存器 / 06 写单寄存器(回显核对)/ 16 写多寄存器
//  - convertRaw 标度变换: u16 标度、i16 符号、f32 IEEE754、u32 大小端
//  - 故障注入: crc → crcErrors++;no_response → timeouts++;exception → exceptions++;
//    wrong_slave → 从站地址不匹配;恢复 none 后链路恢复正常
//  - 从站 requestCount 随合法请求增长
#include "framework.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "../src/hal/posix_serial.h"
#include "../src/hal/pty_pair.h"
#include "../src/modbus/modbus_crc.h"
#include "../src/modbus/modbus_master.h"
#include "../src/modbus/modbus_rtu.h"
#include "../src/modbus/modbus_slave.h"

using es::ISerialDevice;
using es::PosixSerial;
using es::PtyPair;
using es::SerialConfig;
using es::createPtyPair;
using es::modbus::FuncCode;
using es::modbus::ModbusMaster;
using es::modbus::ModbusPoint;
using es::modbus::ModbusSlave;

namespace {

// 全链路夹具: 建 PTY 对 → 主站包装 master fd、从站打开 slave 路径 → 启动从站
struct LinkFixture
{
    PtyPair pair;
    std::shared_ptr<PosixSerial> masterPort;
    std::shared_ptr<PosixSerial> slavePort;
    std::shared_ptr<ModbusSlave> slave;
    std::shared_ptr<ModbusMaster> master;
    std::string err;

    explicit LinkFixture(uint8_t slaveId = 1)
    {
        CHECK(createPtyPair(&pair, &err));
        // 主站: 包装已打开的 master fd(PTY 主端无设备路径)
        masterPort = std::make_shared<PosixSerial>(SerialConfig{"pty-sim-master"}, pair.master);
        // 从站: 按设备路径打开 slave 端(/dev/pts/N)
        slavePort = std::make_shared<PosixSerial>(SerialConfig{pair.slaveName});
        CHECK(masterPort->open(&err));
        CHECK(slavePort->open(&err));
        // 原始 slave fd 已由 slavePort 独立打开,关闭避免泄漏
        ::close(pair.slave);
        pair.slave = -1;

        slave = std::make_shared<ModbusSlave>(slavePort, slaveId);
        CHECK(slave->start(&err));
        master = std::make_shared<ModbusMaster>(masterPort);
    }

    ~LinkFixture()
    {
        if (slave)
        {
            slave->stop();
        }
        if (masterPort)
        {
            masterPort->close();
        }
        if (slavePort)
        {
            slavePort->close();
        }
    }

    // 等待从站线程就绪(首个请求前留出轮询启动时间)
    void settle()
    {
        estest::sleepMs(20);
    }
};

// 构造一个读点(默认 03)
ModbusPoint readPoint(const char* id, uint8_t func, uint16_t addr, uint16_t count,
                      const char* dataType = "u16", double scale = 1.0)
{
    ModbusPoint p;
    p.id = id;
    p.name = id;
    p.slaveId = 1;
    p.func = func;
    p.startAddr = addr;
    p.count = count;
    p.dataType = dataType;
    p.scale = scale;
    return p;
}

} // namespace

ES_TEST(link_read_holding_and_input)
{
    LinkFixture fx;
    // 从站寄存器预置: r0=0x0123 r1=0x4567 r2=0x89AB r3=0xCDEF
    CHECK(fx.slave->setRegister(0, 0x0123));
    CHECK(fx.slave->setRegister(1, 0x4567));
    CHECK(fx.slave->setRegister(2, 0x89AB));
    CHECK(fx.slave->setRegister(3, 0xCDEF));
    fx.slave->setRegisterCount(64);
    fx.settle();

    std::vector<ModbusPoint> points;
    points.push_back(readPoint("hold1", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                               0, 4));
    points.push_back(readPoint("input1", static_cast<uint8_t>(FuncCode::ReadInputRegisters),
                               0, 1));
    CHECK(fx.master->configure(points, &fx.err));

    // 03 读 4 个保持寄存器
    std::vector<uint16_t> regs;
    CHECK(fx.master->readPoint(points[0], &regs, &fx.err));
    CHECK_EQ(regs.size(), static_cast<size_t>(4));
    CHECK_EQ(regs[0], static_cast<uint16_t>(0x0123));
    CHECK_EQ(regs[1], static_cast<uint16_t>(0x4567));
    CHECK_EQ(regs[2], static_cast<uint16_t>(0x89AB));
    CHECK_EQ(regs[3], static_cast<uint16_t>(0xCDEF));

    // 04 读输入寄存器(从站同一寄存器堆应答)
    regs.clear();
    CHECK(fx.master->readPoint(points[1], &regs, &fx.err));
    CHECK_EQ(regs.size(), static_cast<size_t>(1));
    CHECK_EQ(regs[0], static_cast<uint16_t>(0x0123));

    // 统计: 2 次成功事务
    const ModbusMaster::Stats st = fx.master->stats();
    CHECK_EQ(st.txFrames, static_cast<uint64_t>(2));
    CHECK_EQ(st.rxFrames, static_cast<uint64_t>(2));
    CHECK_EQ(st.timeouts, static_cast<uint64_t>(0));
    CHECK_EQ(st.crcErrors, static_cast<uint64_t>(0));
    CHECK_EQ(st.exceptions, static_cast<uint64_t>(0));
    CHECK(fx.slave->requestCount() >= 2);
}

ES_TEST(link_write_single_register)
{
    LinkFixture fx;
    fx.slave->setRegisterCount(64);
    fx.settle();

    ModbusPoint pw = readPoint("w1", static_cast<uint8_t>(FuncCode::WriteSingleRegister),
                               20, 1);
    std::vector<ModbusPoint> points;
    points.push_back(pw);
    CHECK(fx.master->configure(points, &fx.err));

    // 06 写单寄存器 1200(0x04B0): 应答为请求回显,主站逐字节核对
    CHECK(fx.master->writeRegister(pw, 1200, &fx.err));
    uint16_t v = 0;
    CHECK(fx.slave->getRegister(20, &v));
    CHECK_EQ(v, static_cast<uint16_t>(1200));

    // 读回校验(同一链路)
    ModbusPoint pr = readPoint("r20", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                               20, 1);
    std::vector<ModbusPoint> points2;
    points2.push_back(pr);
    CHECK(fx.master->configure(points2, &fx.err));
    std::vector<uint16_t> regs;
    CHECK(fx.master->readPoint(pr, &regs, &fx.err));
    CHECK_EQ(regs[0], static_cast<uint16_t>(1200));
}

ES_TEST(link_write_multiple_registers)
{
    // 主站无 16 写多 API(契约只要求 06 写单),这里走原始帧路径:
    // 主站侧串口直接发出标准 0x10 请求 → 从站应答回显 → 读回校验寄存器内容。
    LinkFixture fx;
    fx.slave->setRegisterCount(64);
    fx.settle();

    // 组 0x10 请求: 起始地址 0x10、2 个寄存器、数据 0x1234 0x5678
    std::vector<uint8_t> req;
    CHECK(es::modbus::encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
                                  {0x00, 0x10, 0x00, 0x02, 0x04, 0x12, 0x34, 0x56, 0x78},
                                  &req));
    (void)fx.masterPort->flush();
    CHECK_EQ(fx.masterPort->write(req.data(), req.size(), std::chrono::milliseconds(500)),
             static_cast<ssize_t>(req.size()));

    // 收应答(回显: [1][0x10][00 10 00 02][CRC],共 8 字节)
    std::vector<uint8_t> resp;
    uint8_t tmp[64];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (resp.size() < 8 && std::chrono::steady_clock::now() < deadline)
    {
        const ssize_t n = fx.masterPort->read(tmp, sizeof(tmp), std::chrono::milliseconds(200));
        if (n > 0)
        {
            resp.insert(resp.end(), tmp, tmp + n);
        }
        else if (n < 0)
        {
            break;
        }
    }
    CHECK_EQ(resp.size(), static_cast<size_t>(8));
    std::vector<uint8_t> expect;
    CHECK(es::modbus::encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
                                  {0x00, 0x10, 0x00, 0x02}, &expect));
    CHECK(resp == expect); // 应答 = 起始地址 + 数量回显

    // 读回: 从站寄存器 0x10/0x11 已被写入
    uint16_t v = 0;
    CHECK(fx.slave->getRegister(0x10, &v));
    CHECK_EQ(v, static_cast<uint16_t>(0x1234));
    CHECK(fx.slave->getRegister(0x11, &v));
    CHECK_EQ(v, static_cast<uint16_t>(0x5678));

    // 经主站协议栈读回(03 读 2 寄存器)
    ModbusPoint pr = readPoint("r16", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                               0x10, 2);
    std::vector<ModbusPoint> points;
    points.push_back(pr);
    CHECK(fx.master->configure(points, &fx.err));
    std::vector<uint16_t> regs;
    CHECK(fx.master->readPoint(pr, &regs, &fx.err));
    CHECK_EQ(regs[0], static_cast<uint16_t>(0x1234));
    CHECK_EQ(regs[1], static_cast<uint16_t>(0x5678));
}

ES_TEST(link_convert_raw_scaling)
{
    // convertRaw 是纯函数,但这里结合真实链路读到的寄存器做端到端标度验证
    LinkFixture fx;
    fx.slave->setRegisterCount(64);
    CHECK(fx.slave->setRegister(0, 0x0123));  // 291
    CHECK(fx.slave->setRegister(10, 0x40A0)); // f32 5.0 高字
    CHECK(fx.slave->setRegister(11, 0x0000)); // f32 5.0 低字
    CHECK(fx.slave->setRegister(12, 0xFFFF)); // i16 = -1
    CHECK(fx.slave->setRegister(14, 0x0001)); // u32 BE = 0x00010002
    CHECK(fx.slave->setRegister(15, 0x0002));
    fx.settle();

    // u16 + 标度 0.1: 291 * 0.1 = 29.1
    ModbusPoint pScale = readPoint("t1", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                   0, 1, "u16", 0.1);
    // f32 IEEE754: 5.0
    ModbusPoint pF32 = readPoint("t2", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                 10, 2, "f32");
    pF32.is32Bit = true;
    // i16 符号扩展: 0xFFFF → -1
    ModbusPoint pI16 = readPoint("t3", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                 12, 1, "i16");
    // u32 大端: 0x00010002 = 65538
    ModbusPoint pU32BE = readPoint("t4", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                   14, 2, "u32");
    pU32BE.is32Bit = true;
    pU32BE.bigEndian = true;
    // u32 小端(低字在前): 0x00020001 = 131073
    ModbusPoint pU32LE = readPoint("t5", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                   14, 2, "u32");
    pU32LE.is32Bit = true;
    pU32LE.bigEndian = false;

    std::vector<ModbusPoint> points = {pScale, pF32, pI16, pU32BE, pU32LE};
    CHECK(fx.master->configure(points, &fx.err));

    // 纯函数校验(不经链路): 与真实链路读值交叉验证
    bool ok = false;
    CHECK_NEAR(fx.master->convertRaw(pScale, {0x0123}, &ok), 29.1, 1e-9);
    CHECK(ok);
    CHECK_NEAR(fx.master->convertRaw(pF32, {0x40A0, 0x0000}, &ok), 5.0, 1e-6);
    CHECK(ok);
    CHECK_NEAR(fx.master->convertRaw(pI16, {0xFFFF}, &ok), -1.0, 1e-9);
    CHECK(ok);
    CHECK_NEAR(fx.master->convertRaw(pU32BE, {0x0001, 0x0002}, &ok), 65538.0, 1e-9);
    CHECK(ok);
    CHECK_NEAR(fx.master->convertRaw(pU32LE, {0x0001, 0x0002}, &ok), 131073.0, 1e-9);
    CHECK(ok);

    // 端到端: 真实链路读 + 标度
    std::vector<uint16_t> regs;
    CHECK(fx.master->readPoint(pF32, &regs, &fx.err));
    CHECK_NEAR(fx.master->convertRaw(pF32, regs, &ok), 5.0, 1e-6);
    CHECK(ok);
}

ES_TEST(link_fault_injection_and_recovery)
{
    LinkFixture fx;
    CHECK(fx.slave->setRegister(0, 0xABCD));
    fx.slave->setRegisterCount(64);
    fx.settle();

    ModbusPoint p = readPoint("f1", static_cast<uint8_t>(FuncCode::ReadHoldingRegisters), 0, 1);
    std::vector<ModbusPoint> points;
    points.push_back(p);
    CHECK(fx.master->configure(points, &fx.err));

    // 基线: 一次成功读
    std::vector<uint16_t> regs;
    CHECK(fx.master->readPoint(p, &regs, &fx.err));
    CHECK_EQ(regs[0], static_cast<uint16_t>(0xABCD));
    fx.master->resetStats();

    // 1) crc 注入: 应答 CRC 被破坏 → crcErrors
    fx.slave->injectFault("crc");
    std::string derr;
    CHECK(!fx.master->readPoint(p, &regs, &derr));
    CHECK(!derr.empty());
    CHECK(fx.master->stats().crcErrors >= 1);

    // 2) no_response: 从站不应答 → 超时(首字节等待 ≈ 3.5T + 1000ms 兜底)
    fx.slave->injectFault("no_response");
    derr.clear();
    CHECK(!fx.master->readPoint(p, &regs, &derr));
    CHECK(!derr.empty());
    CHECK(fx.master->stats().timeouts >= 1);

    // 3) exception: 从站回异常应答(功能码 0x80|03,异常码 0x02)→ exceptions
    fx.slave->injectFault("exception");
    derr.clear();
    CHECK(!fx.master->readPoint(p, &regs, &derr));
    CHECK(!derr.empty());
    CHECK(fx.master->stats().exceptions >= 1);

    // 4) wrong_slave: 从站用错误从站地址应答 → 主站报"从站地址不匹配"
    fx.slave->injectFault("wrong_slave");
    derr.clear();
    CHECK(!fx.master->readPoint(p, &regs, &derr));
    CHECK(!derr.empty());
    CHECK(derr.find("从站地址不匹配") != std::string::npos);

    // 5) 恢复: none 后链路恢复正常,数值与基线一致
    fx.slave->injectFault("none");
    derr.clear();
    CHECK(fx.master->readPoint(p, &regs, &derr));
    CHECK(derr.empty());
    CHECK_EQ(regs[0], static_cast<uint16_t>(0xABCD));

    // 从站确实处理了全部合法请求(含故障注入期间的请求)
    CHECK(fx.slave->requestCount() >= 6);
    CHECK_EQ(fx.slave->fault(), std::string("none"));
}

ES_TEST(link_slave_streaming_frame_f1_regression)
{
    // F1 回归: 模拟 9600 波特逐字节到达(字符时间 ≈1.15ms,8 字节请求历时 ≈9.2ms,
    // 大于 3.5T≈4ms)。修复前从站按"帧首字节"锚定静默计时,会在第 4~5 字节处误判
    // 帧结束 → 半帧 CRC 失败 → 整帧丢弃 → 主站表现为超时;PTY 整包投递掩盖此缺陷。
    // 修复后锚点随最后字节移动,字节间隔 <3.5T 时整帧完整 → 正常应答。
    LinkFixture fx;
    fx.settle();

    std::vector<uint8_t> req;
    CHECK(es::modbus::encodeFrame(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                                  {0x00, 0x00, 0x00, 0x02}, &req));
    CHECK_EQ(req.size(), size_t(8));

    // 逐字节写入,字节间隔 ≈1.2ms(1ms sleep + 调度开销,< 3.5T=4ms)
    for (size_t i = 0; i < req.size(); ++i) {
        const ssize_t w = fx.masterPort->write(&req[i], 1, std::chrono::milliseconds(500));
        CHECK_EQ(w, ssize_t(1));
        if (i + 1 < req.size()) {
            estest::sleepMs(1);
        }
    }

    // 从站应完整应答(03 读 2 寄存器 = 9 字节),而非超时
    uint8_t resp[32] = {0};
    const ssize_t n = fx.masterPort->read(resp, sizeof(resp), std::chrono::milliseconds(800));
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(resp[0], uint8_t(1));     // 从站地址
        CHECK_EQ(resp[1], uint8_t(0x03));  // 功能码
        CHECK_EQ(resp[2], uint8_t(4));     // 字节数 = 2*count
        // 应答 CRC 合法(低字节在前)
        const uint16_t crc = es::modbus::crc16(resp, static_cast<size_t>(n) - 2);
        const uint16_t line = static_cast<uint16_t>(resp[n - 2]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(resp[n - 1]) << 8);
        CHECK_EQ(crc, line);
    }
}
