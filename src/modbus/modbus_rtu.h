// src/modbus/modbus_rtu.h
// 职责: Modbus RTU 纯编解码(组帧/拆帧/帧长推导/3.5T 时序工具)。
//       本模块不碰 IO,可独立单元测试 —— 协议正确性在此闭环。
// 设计要点:
//  - RTU 帧结构: [从站地址 1B][功能码 1B][数据 N B][CRC16 2B,低字节在前],
//    无帧头/帧尾标记 —— 帧边界完全靠"字符间隔"确定(见 charTimeUs);
//  - 功能码语义: 03 读保持寄存器(可读写区,PLC 的保持区)、04 读输入寄存器
//    (只读区,传感器/采集通道)、06 写单个寄存器(写保持区)、0x10 写多个寄存器
//    (Modbus 标准功能码);应答读类 = 5 + N*2(1 地址 + 1 功能码 + 1 字节数 + 2N 数据 + 2 CRC);
//  - 异常应答: 功能码最高位置 1(0x80 | 原功能码) + 1 字节异常码,总长固定 5 字节;
//  - decodeFrame 三态: NeedMore(半包,继续收)/ Ok(完整帧)/ Error(协议错误);
//  - 帧长推导技巧: 03/04 的请求固定 8 字节(偶数),应答 5+字节数(恒为奇数),
//    16 的应答 8 字节(偶数)、请求 9+2N(奇数) —— 借"奇偶性"即可在不知道
//    收发方向时无歧义地区分请求与应答(见 .cpp 逐 case 注释)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace es::modbus {

enum class FuncCode : uint8_t {
    ReadHoldingRegisters = 0x03,   // 读保持寄存器(可读写)
    ReadInputRegisters = 0x04,     // 读输入寄存器(只读)
    WriteSingleRegister = 0x06,    // 写单个寄存器
    WriteMultipleRegisters = 0x10, // 写多个寄存器(Modbus 标准功能码 0x10;曾有中间版本误写 0x16,已修正)
};

enum class ExceptionCode : uint8_t {
    IllegalFunction = 0x01,      // 功能码不支持
    IllegalDataAddress = 0x02,   // 数据地址越界
    IllegalDataValue = 0x03,     // 数据值非法(如寄存器数量为 0)
    SlaveDeviceFailure = 0x04,   // 从站内部故障
};

// 已解码帧: data 不含地址/功能码/CRC,仅含协议数据段(请求参数或应答载荷)
struct RtuFrame {
    uint8_t slaveId = 0;
    uint8_t func = 0;
    std::vector<uint8_t> data;
};

// 完整 ADU 长度推导:
//   请求 03/04        = 8 字节固定   (1+1+2 起始地址+2 数量+2 CRC)
//   请求 06           = 8 字节固定   (1+1+2 地址+2 数值+2 CRC)
//   请求 16           = 9 + N*2      (1+1+2 地址+2 数量+1 字节数+2N 数据+2 CRC)
//   应答 03/04        = 5 + N*2      (1+1+1 字节数+2N 数据+2 CRC)
//   应答 06 / 应答 16 = 8 字节       (请求回显)
//   异常应答          = 5 字节       (1+1+1 异常码+2 CRC)

// 组帧: out = [slaveId][func][data...][CRC_Lo][CRC_Hi];失败仅当 out 为空指针
bool encodeFrame(uint8_t slaveId, uint8_t func, const std::vector<uint8_t>& data,
                 std::vector<uint8_t>* out);

// 拆帧: 输入为已按 3.5T 静默切分(或按预期长度收齐)的字节流。
//   NeedMore — 数据不足,继续累积;Ok — 得到完整帧,frameLen 为帧长,out 已填充;
//   Error    — 协议错误(未知功能码/非法字节数字段/数据粘连),err 描述原因。
//   注意: CRC 校验不在本函数内(纯结构解析),由调用方按自身分类需求校验。
enum class DecodeResult { NeedMore, Ok, Error };
DecodeResult decodeFrame(const uint8_t* buf, size_t len, size_t* frameLen, RtuFrame* out,
                         std::string* err);

// 3.5T 静默间隔工具。
// RTU 无帧头帧尾,靠字符间隔定帧界: 两帧之间静默 >= 3.5 个字符时间才认为
// 上一帧结束(小于 3.5T 的间隙视为同一帧内的字符间隔,防止把慢速到达的
// 字节拆成两帧)。1 字符 = 1 起始位 + 8 数据位 + 1 校验位 + 1 停止位 = 11 bit,
// 故单字符时间(µs) = 11 * 1e6 / baud,3.5T = 3.5 * charTimeUs。
// (注: 8N1 无校验时理论为 10 bit;Modbus 规范与主流实现按 11 bit 计,差异 <10%,
//  对帧分隔判定无实质影响。)
double charTimeUs(int baud);

}  // namespace es::modbus
