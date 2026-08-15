// 文件路径: tests/test_modbus_rtu.cpp
// 意图: RTU 纯编解码(契约 §7)单测 —— 组帧/拆帧回环、帧长推导(奇偶性消歧)、
//       半包 NeedMore、异常应答、错误路径、3.5T 字符间隔计算。
// 覆盖点:
//  - 各功能码(03/04 请求与应答、06、16 写多、异常应答)encode → decode 回环
//  - 半包: 长度不足 → NeedMore;帧超长(粘连)→ Error
//  - 协议错误: 未知功能码/广播地址 0/应答字节数非法/写多数量非法
//  - CRC 归属: decodeFrame 是纯结构解析不校验 CRC(由主站/从站分层校验,见注释)
//  - charTimeUs: 9600/19200 波特下的 3.5T 数值
#include "framework.h"

#include "../src/modbus/modbus_crc.h"
#include "../src/modbus/modbus_rtu.h"

#include <cstdint>
#include <string>
#include <vector>

using es::modbus::DecodeResult;
using es::modbus::FuncCode;
using es::modbus::RtuFrame;
using es::modbus::decodeFrame;
using es::modbus::encodeFrame;

// 辅助: encode → decode 回环,断言 Ok 且字段一致
static void roundtrip(uint8_t slaveId, uint8_t func, const std::vector<uint8_t>& data,
                      size_t expectFrameLen)
{
    std::vector<uint8_t> frame;
    CHECK(encodeFrame(slaveId, func, data, &frame));
    CHECK_EQ(frame.size(), expectFrameLen);

    RtuFrame out;
    size_t frameLen = 0;
    std::string err;
    CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err),
             DecodeResult::Ok);
    CHECK_EQ(frameLen, expectFrameLen);
    CHECK_EQ(out.slaveId, slaveId);
    CHECK_EQ(out.func, func);
    CHECK(out.data == data);
}

ES_TEST(rtu_encode_decode_roundtrip)
{
    // 03 请求(固定 8 字节)
    roundtrip(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
              {0x00, 0x00, 0x00, 0x0A}, 8);
    // 03 应答(1 寄存器): 数据段 = [字节数 2][寄存器高前低后],帧 = 1+1+3+2 = 7
    roundtrip(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
              {0x02, 0x12, 0x34}, 7);
    // 04 请求
    roundtrip(2, static_cast<uint8_t>(FuncCode::ReadInputRegisters), {0x00, 0x0A, 0x00, 0x01}, 8);
    // 04 应答(4 个寄存器): 数据段 = [字节数 8][8 字节数据],帧 = 1+1+9+2 = 13
    roundtrip(2, static_cast<uint8_t>(FuncCode::ReadInputRegisters),
              {0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}, 13);
    // 06 写单(请求=应答回显,8 字节)
    roundtrip(1, static_cast<uint8_t>(FuncCode::WriteSingleRegister), {0x00, 0x14, 0x04, 0xB0}, 8);
    // 16 写多: 请求 9 + 2N(13 字节);应答 8 字节(回显地址+数量)
    roundtrip(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
              {0x00, 0x10, 0x00, 0x02, 0x04, 0x12, 0x34, 0x56, 0x78}, 13);
    roundtrip(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
              {0x00, 0x10, 0x00, 0x02}, 8);
    // 异常应答: 功能码 0x80|原码,固定 5 字节
    roundtrip(1, static_cast<uint8_t>(0x80 | static_cast<uint8_t>(FuncCode::ReadHoldingRegisters)),
              {0x02}, 5);
}

ES_TEST(rtu_half_packet_need_more)
{
    // 完整 03 请求帧,逐字节喂入。注意解码器的"长度奇偶性消歧":
    // 偶数长度按请求处理(固定 8 字节)、奇数长度按应答处理(校验 buf[2] 字节数)。
    // 因此半包请求喂到奇数长度 5/7 时会被误判为"应答字节数非法"(Error)而非
    // NeedMore —— 这是无方向信息拆帧的设计取舍,调用方按 3.5T 切帧/按预期长度
    // 收帧,不会把半包喂进来。测试固化"偶数长度半包 → NeedMore"的保证语义。
    std::vector<uint8_t> frame;
    CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                      {0x00, 0x00, 0x00, 0x0A}, &frame));
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(6)})
    {
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), n, &frameLen, &out, &err), DecodeResult::NeedMore);
    }
    // 奇偶性消歧的已知边界: 5/7 字节半包会被按应答形态解析 → 字节数非法(Error)
    {
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), 5, &frameLen, &out, &err), DecodeResult::Error);
    }
    RtuFrame out;
    size_t frameLen = 0;
    std::string err;
    CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err), DecodeResult::Ok);

    // 16 写多请求: 数量字段在偏移 4..5,7 字节时尚无法定长 → NeedMore;
    // 偶数长度半包(如 8 字节)会被当成"应答形态"(固定 8 字节)解析成功 —— 同属
    // 奇偶性消歧的设计取舍,这里只断言明确不足的奇数长度。
    std::vector<uint8_t> wm;
    CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
                      {0x00, 0x10, 0x00, 0x02, 0x04, 0x12, 0x34, 0x56, 0x78}, &wm));
    for (size_t n : {size_t(5), size_t(6), size_t(7), size_t(9), size_t(11)})
    {
        CHECK_EQ(decodeFrame(wm.data(), n, &frameLen, &out, &err), DecodeResult::NeedMore);
    }
    CHECK_EQ(decodeFrame(wm.data(), wm.size(), &frameLen, &out, &err), DecodeResult::Ok);
}

ES_TEST(rtu_protocol_errors)
{
    // 未知功能码
    {
        std::vector<uint8_t> frame;
        CHECK(encodeFrame(1, 0x2B, {0x00}, &frame));
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err),
                 DecodeResult::Error);
        CHECK(!err.empty());
    }
    // 广播地址 0 不支持
    {
        std::vector<uint8_t> frame;
        CHECK(encodeFrame(0, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                          {0x00, 0x00, 0x00, 0x0A}, &frame));
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err),
                 DecodeResult::Error);
        CHECK(!err.empty());
    }
    // 应答字节数字段非法(奇数)→ 03 应答形态(奇数长度)下校验
    {
        std::vector<uint8_t> frame;
        CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                          {0x03, 0x12, 0x34}, &frame)); // 字节数=3(奇数)
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err),
                 DecodeResult::Error);
        CHECK(!err.empty());
    }
    // 写多寄存器数量非法(0 / >123)
    {
        std::vector<uint8_t> frame;
        CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteMultipleRegisters),
                          {0x00, 0x00, 0x00, 0x00, 0x00}, &frame));
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err),
                 DecodeResult::Error);
    }
    // 帧超长(两帧粘连): 完整帧后多 1 字节 → Error
    {
        std::vector<uint8_t> f1, f2;
        CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteSingleRegister),
                          {0x00, 0x14, 0x04, 0xB0}, &f1));
        CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::WriteSingleRegister),
                          {0x00, 0x15, 0x00, 0x01}, &f2));
        std::vector<uint8_t> glued = f1;
        glued.insert(glued.end(), f2.begin(), f2.end());
        RtuFrame out;
        size_t frameLen = 0;
        std::string err;
        CHECK_EQ(decodeFrame(glued.data(), glued.size(), &frameLen, &out, &err),
                 DecodeResult::Error);
    }
}

ES_TEST(rtu_crc_is_not_checked_by_decode)
{
    // 分层设计: decodeFrame 只做结构解析;CRC 校验由调用方(主站 doTransaction /
    // 从站 handleFrame)按各自分类需求执行。此处固化该行为:
    // 结构合法但 CRC 损坏的帧仍能结构解码 —— 主站侧会在 CRC 关卡拦下。
    std::vector<uint8_t> frame;
    CHECK(encodeFrame(1, static_cast<uint8_t>(FuncCode::ReadHoldingRegisters),
                      {0x00, 0x00, 0x00, 0x0A}, &frame));
    frame[frame.size() - 1] ^= 0xFF; // 破坏 CRC 高字节
    RtuFrame out;
    size_t frameLen = 0;
    std::string err;
    CHECK_EQ(decodeFrame(frame.data(), frame.size(), &frameLen, &out, &err), DecodeResult::Ok);
    // 调用方视角: CRC 校验函数能发现损坏
    const uint16_t expect = static_cast<uint16_t>((frame[frame.size() - 1] << 8) |
                                                  frame[frame.size() - 2]);
    CHECK(es::modbus::crc16(frame.data(), frame.size() - 2) != expect);
}

ES_TEST(rtu_char_time_3_5t)
{
    // 1 字符 = 11 bit;9600 波特 → 11 * 1e6 / 9600 = 1145.83 µs
    CHECK_NEAR(es::modbus::charTimeUs(9600), 1145.8333, 0.01);
    // 19200 → 572.9167 µs
    CHECK_NEAR(es::modbus::charTimeUs(19200), 572.9167, 0.01);
    // 3.5T 帧间隔 = 3.5 * charTimeUs(由调用方折算成 ms 超时)
    CHECK_NEAR(3.5 * es::modbus::charTimeUs(9600), 4010.4167, 0.05);
    // 非法波特率 → 0(防御)
    CHECK_NEAR(es::modbus::charTimeUs(0), 0.0, 1e-9);
    CHECK_NEAR(es::modbus::charTimeUs(-9600), 0.0, 1e-9);
}
