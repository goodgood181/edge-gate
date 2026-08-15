// src/net/mqtt_packets.h
// 职责: MQTT 3.1.1(OASIS)报文的纯编解码 —— 只操作内存,不碰 socket/线程/IO,
//       可独立单元测试;字节级对齐协议规范。上层 MqttClient 与测试用 fake_broker
//       (用 decodePacket 自举)都依赖本模块,协议正确性在此闭环。
// 设计要点:
//  - 固定头: [类型+标志 1B][剩余长度 1..4B][可变头+载荷]。类型在字节高 4 位
//    (1..14,0/15 保留),标志在低 4 位: PUBLISH 承载 dup/qos/retain,其余报文
//    多为固定值 0000,而 PUBREL/SUBSCRIBE/UNSUBSCRIBE 固定 0010 —— 解码端
//    校验标志即可识别非法报文,也能区分 PUBREL 与同构的 PUBREC/PUBCOMP;
//  - 剩余长度变长编码: 每字节低 7 位为数据,最高位为"续延"标志(置 1 表示还有
//    下一字节),每字节 128 进制小端序;最多 4 字节,上限 2^28-1 = 268435455。
//    边界: 127→0x7F, 128→0x80 0x01, 16383→0xFF 0x7F, 16384→0x80 0x80 0x01,
//    2097151→0xFF 0xFF 0x7F。编码用"除 128 取余",解码用"乘 128 累加",数学对称;
//  - 2 字节长度前缀字符串: 主题/ClientId/用户名/密码/Will 等一律
//    [长度 2B 大端][UTF-8 字节],长度字段自描述 —— 解码端按剩余长度切出报文后,
//    靠它把载荷精确拆成多个字段;
//  - QoS1 流程: 发送方 PUBLISH(qos=1,携带包 ID)→ 接收方回 PUBACK(回显同一
//    包 ID)即确认;包 ID 由发送方递增分配、必须非 0(0 保留给"无包 ID"的 QoS0);
//    超时未确认 → 以同包 ID 重发(dup=1),这是 MqttClient 层的职责;
//  - CONNECT 标志位: 见 encodeConnect 注释 —— 任务契约注释里的位序描述与
//    OASIS 规范不一致,本实现以 OASIS 为准(要与 mosquitto 等真实 broker 互通)。
// 错误处理: 全模块无异常;解码失败一律 false + err 出参;数据不足时 false 且
// err 为空,调用方(收包循环)理解为"还需更多字节",继续累积。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace es::mqtt {

// MQTT 3.1.1 控制报文类型(固定头高 4 位取值;0 与 15 为保留值,解码视为错误)
enum class PacketType : uint8_t {
    Connect = 1,       // 客户端→服务端 连接请求
    Connack = 2,       // 服务端→客户端 连接确认
    Publish = 3,       // 双向 发布消息
    Puback = 4,        // 服务端→客户端 QoS1 确认
    Pubrec = 5,        // QoS2 四段握手的第 2 步
    Pubrel = 6,        // QoS2 四段握手的第 3 步
    Pubcomp = 7,       // QoS2 四段握手的第 4 步
    Subscribe = 8,     // 客户端→服务端 订阅
    Suback = 9,        // 服务端→客户端 订阅确认
    Unsubscribe = 10,  // 客户端→服务端 取消订阅
    Unsuback = 11,     // 服务端→客户端 取消订阅确认
    Pingreq = 12,      // 客户端→服务端 keepalive 心跳
    Pingresp = 13,     // 服务端→客户端 心跳应答
    Disconnect = 14,   // 客户端→服务端 优雅断开
};

// CONNECT 报文参数(编码输入)
struct ConnectOptions {
    std::string clientId;        // 客户端标识(2 字节长度前缀字符串)
    uint16_t keepaliveSec = 30;  // keepalive 秒数;0 表示关闭心跳
    bool cleanSession = true;    // 置位: 服务端丢弃该客户端的旧会话状态
    std::string username;        // 非空 → 置 UserName 标志并在载荷追加
    std::string password;        // 非空 → 置 Password 标志;规范要求密码必须伴随用户名
    std::string willTopic;       // 非空 → 置 Will 标志(broker 代发遗嘱)
    std::string willPayload;     // Will 消息内容
    uint8_t willQos = 0;         // Will QoS(0/1/2)
    bool willRetain = false;     // Will Retain
};

// 解码结果统一载体 —— 一个结构复用于全部报文,字段按类型复用(映射约定,
// 由本模块唯一解释;不改契约字段,故 CONNACK 的 sessionPresent 等借用未用字段):
//   PUBLISH     : topic/payload/dup/qos/retain;qos>0 时 packetId 为包 ID
//   CONNACK     : returnCode=返回码(0x00 接受,0x01..0x05 拒绝原因);
//                 packetId 复用为 sessionPresent(0/1)
//   SUBACK      : packetId;topics[i]=第 i 个主题授权 QoS 的十进制串("0"/"1"/"2"/"128")
//   UNSUBACK/PUBACK/PUBREC/PUBREL/PUBCOMP : packetId
//   CONNECT     : topics[0]=clientId;topic=willTopic;payload=willPayload
//   SUBSCRIBE   : packetId;topics[i]=主题;payload 第 i 字节=第 i 个主题的请求 QoS
//   UNSUBSCRIBE : packetId;topics[i]=主题
//   PINGREQ/PINGRESP/DISCONNECT : 仅 type 有效
struct Packet {
    PacketType type = PacketType::Connect;  // 报文类型(解码时覆盖;默认值防未初始化读取)
    bool dup = false;        // PUBLISH: 重发标志(dup=1 表示 QoS1 超时重传)
    uint8_t qos = 0;         // PUBLISH: QoS 级别(0/1/2)
    bool retain = false;     // PUBLISH: 保留标志(broker 留存,新订阅者立即收到)
    uint16_t packetId = 0;   // 包 ID(QoS>0 的 PUBLISH 及应答报文);CONNACK 复用为 sessionPresent
    std::string topic;       // PUBLISH 主题 / CONNECT 的 willTopic
    std::string payload;     // PUBLISH 载荷 / CONNECT 的 willPayload / SUBSCRIBE 的请求 QoS 字节串
    uint8_t returnCode = 0;  // CONNACK 返回码
    std::vector<std::string> topics;  // SUBACK 授权 QoS 十进制串 / SUBSCRIBE·UNSUBSCRIBE 主题 / CONNECT 的 clientId
};

// —— 剩余长度变长编码(固定头第 2 字节起,1..4 字节) ——
// 每字节低 7 位 = 数据,最高位 = 续延。边界:
//   127 → 0x7F           128 → 0x80 0x01
//   16383 → 0xFF 0x7F    16384 → 0x80 0x80 0x01
//   2097151 → 0xFF 0xFF 0x7F   268435455 → 0xFF 0xFF 0xFF 0x7F
// 失败: out 为空,或 v > 268435455(超出 4 字节上限)。
[[nodiscard]] bool encodeRemainingLength(uint32_t v, std::vector<uint8_t>* out);

// 剩余长度变长解码,最多消费 4 字节。成功: *out = 数值,*used = 消费字节数。
// 失败时 *used 有定义,用于区分两种情形:
//   *used == 4 且返回 false → 协议错误(第 4 字节仍有续延位);
//   *used < 4  且返回 false → 输入耗尽,还需更多字节(非错误)。
[[nodiscard]] bool decodeRemainingLength(const uint8_t* buf, size_t len, size_t* used,
                                         uint32_t* out);

// —— 报文编码(全部返回"固定头 + 剩余长度 + 可变头/载荷"的完整字节流) ——
// 编码端是受控输入(本客户端自己构造报文),约束由调用方保证并在注释说明:
// 主题非空、qos ∈ {0,1,2}、qos>0 时 packetId 非 0 等;不满足即产生非法报文。

// CONNECT: 固定头 0x10;可变头 [协议名 "MQTT"][级别 0x04][标志][keepalive 2B];
// 载荷 [clientId][willTopic+willPayload?][username?][password?]。
// 标志位(OASIS 3.1.1 §3.1.2.5 —— 注意: 任务契约注释写 "bit0 cleanSession、
// bit1 will..." 是从 0 计位的简化描述,与规范位序不一致;本实现以规范为准,
// 否则 mosquitto 等真实 broker 无法互通):
//   bit0 保留(必须 0)  bit1 CleanSession  bit2 Will  bit3-4 WillQoS(2 bit)
//   bit5 WillRetain    bit6 Password     bit7 UserName
// 规范约束: 密码标志置位则用户名标志必须同时置位[MQTT-3.1.2-22];
// Will 未置位时 WillQoS/WillRetain 必须为 0[MQTT-3.1.2-13/15]。
std::vector<uint8_t> encodeConnect(const ConnectOptions& opt);

// PUBLISH: 固定头 0x30 | dup<<3 | qos<<1 | retain;可变头 [topic][packetId?];
// 载荷为消息内容。qos>0 必须携带非 0 包 ID;QoS0 无包 ID。
std::vector<uint8_t> encodePublish(const std::string& topic, const std::string& payload,
                                   uint8_t qos, bool retain, uint16_t packetId);

// PUBACK: 0x40 + 包 ID 回显(QoS1 确认,见文件头 QoS1 流程注释)
std::vector<uint8_t> encodePuback(uint16_t packetId);

// SUBSCRIBE: 0x82(标志固定 0010);载荷 [包 ID][(topic,qos)…];包 ID 必须非 0。
std::vector<uint8_t> encodeSubscribe(
    uint16_t packetId, const std::vector<std::pair<std::string, uint8_t>>& topics);

// UNSUBSCRIBE: 0xA2(标志固定 0010);载荷 [包 ID][topic…];包 ID 必须非 0。
std::vector<uint8_t> encodeUnsubscribe(uint16_t packetId,
                                       const std::vector<std::string>& topics);

// PINGREQ: 0xC0 00(keepalive 心跳,见 MqttClient);DISCONNECT: 0xE0 00(优雅断开)
std::vector<uint8_t> encodePingreq();
std::vector<uint8_t> encodeDisconnect();

// —— 解码(一次解析一个完整报文) ——
// 返回 true         : 得到完整报文,*consumed = 报文总字节数(固定头+剩余长度+可变头/载荷)
// 返回 false + err 空: 数据不足,还需更多字节(调用方继续累积后重试)
// 返回 false + err 非空: 协议错误(非法类型/标志、剩余长度超 4 字节、
//                       SUBSCRIBE 等包 ID 为 0、QoS=3、长度字段越界等)
// 字段映射见 Packet 注释。全模块无异常。
[[nodiscard]] bool decodePacket(const uint8_t* buf, size_t len, size_t* consumed, Packet* out,
                                std::string* err);

}  // namespace es::mqtt
