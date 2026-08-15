// 文件路径: tests/test_modbus_crc.cpp
// 意图: CRC16-Modbus(契约 §7)标准已知向量测试 —— 用协议文档/主流工具可复算的
//       固定帧比对,证明位运算实现与查表法结果一致。
// 已知向量(低字节在前,与线路序一致):
//   "01 03 00 00 00 0A"(读 10 个保持寄存器请求)→ CRC 0xCDC5 → 帧尾 C5 CD
//   "01 03 00 00 00 64"(读 100 个)→ CRC 0x2144 → 帧尾 44 21
//   "01 04 00 00 00 01"(读 1 个输入寄存器)→ CRC 0xCA31 → 帧尾 31 CA
//   "01 06 00 14 04 B0"(写单寄存器 0x14 = 0x04B0)→ CRC 0xBACA → 帧尾 CA BA
//   空数据 → 0xFFFF(初值即输出,无最终异或)
//   单字节 0x00 → 0x40BF;单字节 0xFF → 0x00FF
#include "framework.h"

#include "../src/modbus/modbus_crc.h"
#include "../src/modbus/modbus_rtu.h"

#include <cstdint>
#include <vector>

using es::modbus::crc16;

ES_TEST(crc_standard_known_vectors)
{
    // 经典教程帧: 01 03 00 00 00 0A → C5 CD(低字节在前)
    {
        const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
        CHECK_EQ(crc16(frame, sizeof(frame)), static_cast<uint16_t>(0xCDC5));
    }
    // 读 100 寄存器: 01 03 00 00 00 64 → 44 21
    {
        const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x64};
        CHECK_EQ(crc16(frame, sizeof(frame)), static_cast<uint16_t>(0x2144));
    }
    // 读输入寄存器: 01 04 00 00 00 01 → 31 CA
    {
        const uint8_t frame[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x01};
        CHECK_EQ(crc16(frame, sizeof(frame)), static_cast<uint16_t>(0xCA31));
    }
    // 写单寄存器: 01 06 00 14 04 B0 → CA BA
    {
        const uint8_t frame[] = {0x01, 0x06, 0x00, 0x14, 0x04, 0xB0};
        CHECK_EQ(crc16(frame, sizeof(frame)), static_cast<uint16_t>(0xBACA));
    }
    // 边界: 空数据 = 初值 0xFFFF(无最终异或);单字节 0x00/0xFF
    CHECK_EQ(crc16(nullptr, 0), static_cast<uint16_t>(0xFFFF));
    {
        const uint8_t one = 0x00;
        CHECK_EQ(crc16(&one, 1), static_cast<uint16_t>(0x40BF));
    }
    {
        const uint8_t one = 0xFF;
        CHECK_EQ(crc16(&one, 1), static_cast<uint16_t>(0x00FF));
    }
}

ES_TEST(crc_frame_tail_low_byte_first)
{
    // encodeFrame 把 CRC 低字节放在帧尾倒数第 2 字节、高字节在最后一字节(线路序)
    std::vector<uint8_t> out;
    const std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x0A}; // 03 请求数据段
    CHECK(es::modbus::encodeFrame(1, static_cast<uint8_t>(es::modbus::FuncCode::ReadHoldingRegisters),
                                  data, &out));
    const std::vector<uint8_t> expect = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
    CHECK(out == expect);
    CHECK_EQ(out.size(), static_cast<size_t>(8));
}
