// src/modbus/modbus_rtu.cpp
// 职责: RTU 编解码实现。核心难点是"在不知道收发方向时,仅凭功能码与帧内容
//       推导完整帧长" —— 用长度奇偶性消解歧义(理由见各 case 注释)。
#include "modbus_rtu.h"

#include "modbus_crc.h"

namespace es::modbus {
namespace {

// 1 字节转两位十六进制(仅用于错误信息可读性)
std::string byteHex(uint8_t v) {
    const char* hex = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    return s;
}

}  // namespace

bool encodeFrame(uint8_t slaveId, uint8_t func, const std::vector<uint8_t>& data,
                 std::vector<uint8_t>* out) {
    if (out == nullptr) return false;
    out->clear();
    out->reserve(data.size() + 4);
    out->push_back(slaveId);
    out->push_back(func);
    out->insert(out->end(), data.begin(), data.end());
    // CRC 覆盖 [地址..数据尾];Modbus 线路序: 低字节在前
    const uint16_t crc = crc16(out->data(), out->size());
    out->push_back(static_cast<uint8_t>(crc & 0xFF));        // CRC_Lo
    out->push_back(static_cast<uint8_t>(crc >> 8));          // CRC_Hi
    return true;
}

DecodeResult decodeFrame(const uint8_t* buf, size_t len, size_t* frameLen, RtuFrame* out,
                         std::string* err) {
    if (frameLen != nullptr) *frameLen = 0;
    if (buf == nullptr || out == nullptr || frameLen == nullptr) {
        if (err) *err = "入参为空";
        return DecodeResult::Error;
    }
    // 最小帧 = 地址 1 + 功能码 1 + 数据 1 + CRC 2 = 5 字节(异常应答)
    if (len < 5) return DecodeResult::NeedMore;

    const uint8_t slaveId = buf[0];
    const uint8_t func = buf[1];
    if (slaveId == 0) {
        if (err) *err = "广播地址 0 不支持";
        return DecodeResult::Error;
    }

    size_t expect = 0;
    if (func & 0x80) {
        // 异常应答: 功能码最高位 = 1(0x80|原功能码),总长固定 5 字节
        expect = 5;
    } else {
        switch (func) {
            case static_cast<uint8_t>(FuncCode::ReadHoldingRegisters):
            case static_cast<uint8_t>(FuncCode::ReadInputRegisters): {
                // 请求 8 字节(偶数);应答 5+字节数(5+偶数 = 奇数)。
                // 关键观察: 两种形态长度奇偶不同 → 用奇偶性无歧义区分方向。
                // 例外: 应答字节数字段非法(0/奇数/超 250)直接报错 —— 调用方
                // 只会传入"完整且 CRC 通过"的帧(从站按 3.5T 切帧、主站按
                // 预期长度收帧),长度与字节数矛盾的帧必是损坏帧。
                if (len % 2 == 0) {
                    expect = 8;  // 请求形态: 固定 8 字节
                } else {
                    const uint8_t byteCount = buf[2];  // 应答形态: 偏移 2 是字节数
                    if (byteCount == 0 || (byteCount & 1) != 0 || byteCount > 250) {
                        if (err) *err = "应答字节数字段非法(应为 2..250 的偶数)";
                        return DecodeResult::Error;
                    }
                    expect = 5 + byteCount;  // 1 地址 + 1 功能码 + 1 字节数 + 数据 + 2 CRC
                }
                break;
            }
            case static_cast<uint8_t>(FuncCode::WriteSingleRegister): {
                // 06 请求与应答同构(回显): 固定 8 字节,无歧义
                expect = 8;
                break;
            }
            case static_cast<uint8_t>(FuncCode::WriteMultipleRegisters): {
                // 应答 8 字节(偶数,回显地址+数量);请求 9+2N(奇数)。
                // 奇偶性再次消歧;请求的寄存器数量在偏移 4..5,需先等够 6 字节
                if (len % 2 == 0) {
                    expect = 8;  // 应答形态
                } else {
                    if (len < 6) return DecodeResult::NeedMore;  // 数量字段未到齐
                    const uint16_t cnt = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
                    if (cnt == 0 || cnt > 123) {  // 单次写多上限 123 寄存器
                        if (err) *err = "写多寄存器数量非法(应为 1..123)";
                        return DecodeResult::Error;
                    }
                    expect = 9 + 2 * cnt;  // 7 头 + 2N 数据 + 2 CRC
                }
                break;
            }
            default:
                if (err) *err = "不支持的功能码 0x" + byteHex(func);
                return DecodeResult::Error;
        }
    }

    if (len < expect) return DecodeResult::NeedMore;
    if (len > expect) {
        if (err) *err = "帧超长(疑似两帧粘连)";
        return DecodeResult::Error;
    }
    *frameLen = expect;
    out->slaveId = slaveId;
    out->func = func;
    out->data.assign(buf + 2, buf + expect - 2);  // 去掉地址、功能码与 CRC
    return DecodeResult::Ok;
}

double charTimeUs(int baud) {
    if (baud <= 0) return 0.0;
    return 11.0 * 1e6 / static_cast<double>(baud);  // 11 bit/字符 * 1e6 µs/s / 波特率
}

}  // namespace es::modbus
