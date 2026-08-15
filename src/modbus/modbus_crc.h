// src/modbus/modbus_crc.h
// 职责: CRC16-Modbus 校验(Modbus RTU 帧尾 2 字节)。
// 设计要点:
//  - 标准多项式 0x8005(x^16 + x^15 + x^2 + 1)按 LSB 优先反射后为 0xA001;
//  - 初值 0xFFFF,无最终异或(Modbus 规范与 CRC-16/XMODEM 等变体的关键差异);
//  - 线路字节序: 低字节先发(先 CRC_Lo 后 CRC_Hi),与多字节寄存器"高字节在前"
//    的惯例相反,拆帧时注意;
//  - 本实现用位运算(逐 bit, 8*len 次循环,代码量小、零查表内存);
//    查表法把 256 个"字节余数"预计算进 512B ROM,每个字节一次查表 + 异或,
//    速度约快 8 倍 —— 在无刷写成本的 MCU 上常选查表,资源紧张时选位运算。
#pragma once

#include <cstddef>
#include <cstdint>

namespace es::modbus {

// 计算 data[0..len) 的 CRC16-Modbus 值(返回的 uint16 高位是线路上的低字节)
uint16_t crc16(const uint8_t* data, size_t len);

}  // namespace es::modbus
