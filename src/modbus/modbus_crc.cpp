// src/modbus/modbus_crc.cpp
// 职责: CRC16-Modbus 位运算实现。
// 算法 Why:
//  - 多项式 0x8005 的二进制是 1000 0000 0000 0101(MSB 优先算法从左往右除);
//    Modbus 采用"LSB 优先"反射形式: 把多项式按位反转得到 0xA001,寄存器从
//    最低位开始处理,每次右移 —— 硬件移位寄存器通常就是 LSB 优先,反射形式
//    便于逐位实现,查表表项也按反射形式生成;
//  - 初值 0xFFFF: 保证长度 < 16 的短帧(如 5 字节异常应答)也能检出全 0 数据
//    导致的伪 CRC;Modbus 规范无最终异或,直接输出;
//  - 每字节: 先与本字节异或(相当于把该字节移入寄存器),再按位右移,
//    LSB 为 1 时异或 0xA001。
//
// 自检向量(Modbus 规范附录经典例,可手算复核):
//   {01 03 00 00 00 0A} -> CRC = 0xCDC5,线路字节 C5 CD
//   {01 06 00 01 00 17} -> CRC = 0x0498,线路字节 98 04
#include "modbus_crc.h"

namespace es::modbus {

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;  // 初值: 全 1
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]);  // 本字节异或进寄存器
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);  // LSB=1: 右移并异或反射多项式
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

}  // namespace es::modbus
