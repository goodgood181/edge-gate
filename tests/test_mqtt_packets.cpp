// 文件路径: tests/test_mqtt_packets.cpp
// 意图: MQTT 3.1.1 纯编解码(契约 §9)单测 —— 字节级对齐 OASIS 规范。
// 覆盖点:
//  - 剩余长度变长编码 7 个边界值(127/128/16383/16384/2097151/268435455/超限)
//  - CONNECT 字节级比对(固定头/协议名/级别/标志/keepalive/载荷)
//  - CONNECT 全字段(Will/用户名/密码)标志位与解码映射
//  - PUBLISH QoS0/1、PUBACK、SUBSCRIBE/SUBACK、UNSUBSCRIBE、PINGREQ/DISCONNECT 回环
//  - 半包: 数据不足 → false 且 err 为空(调用方继续累积)
//  - 非法报文: 类型 0/15、QoS=3、标志位错、包 ID 为 0、CONNACK 返回码非法等
#include "framework.h"

#include "../src/net/mqtt_packets.h"

#include <cstdint>
#include <string>
#include <vector>

using es::mqtt::ConnectOptions;
using es::mqtt::Packet;
using es::mqtt::PacketType;
using es::mqtt::decodePacket;
using es::mqtt::decodeRemainingLength;
using es::mqtt::encodeConnect;
using es::mqtt::encodeDisconnect;
using es::mqtt::encodePingreq;
using es::mqtt::encodePuback;
using es::mqtt::encodePublish;
using es::mqtt::encodeRemainingLength;
using es::mqtt::encodeSubscribe;
using es::mqtt::encodeUnsubscribe;

// 辅助: 断言编码输出与期望字节完全一致
static void expectBytes(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want)
{
    CHECK(got == want);
    if (got != want)
    {
        // 失败时打印两边长度,便于定位
        CHECK_EQ(got.size(), want.size());
    }
}

ES_TEST(mqtt_remaining_length_boundaries)
{
    // 契约 §14 指定边界: 127 / 128 / 16383 / 16384 / 2097151
    std::vector<uint8_t> out;
    CHECK(encodeRemainingLength(0, &out));
    expectBytes(out, {0x00});
    CHECK(encodeRemainingLength(127, &out));
    expectBytes(out, {0x7F});
    CHECK(encodeRemainingLength(128, &out));
    expectBytes(out, {0x80, 0x01});
    CHECK(encodeRemainingLength(16383, &out));
    expectBytes(out, {0xFF, 0x7F});
    CHECK(encodeRemainingLength(16384, &out));
    expectBytes(out, {0x80, 0x80, 0x01});
    CHECK(encodeRemainingLength(2097151, &out));
    expectBytes(out, {0xFF, 0xFF, 0x7F});
    CHECK(encodeRemainingLength(268435455, &out)); // 4 字节上限 2^28-1
    expectBytes(out, {0xFF, 0xFF, 0xFF, 0x7F});
    // 超限: 编码失败
    CHECK(!encodeRemainingLength(268435456, &out));
    CHECK(!encodeRemainingLength(0xFFFFFFFFu, &out));

    // 解码回环
    const uint32_t vals[] = {0, 1, 127, 128, 16383, 16384, 2097151, 268435455};
    for (uint32_t v : vals)
    {
        CHECK(encodeRemainingLength(v, &out));
        size_t used = 0;
        uint32_t got = 0;
        CHECK(decodeRemainingLength(out.data(), out.size(), &used, &got));
        CHECK_EQ(used, out.size());
        CHECK_EQ(got, v);
    }
    // 半字节: 只有续延位、无后续字节 → 数据不足(used < 4 且 false,err 为空语义在 decodePacket)
    {
        const uint8_t partial[] = {0x80};
        size_t used = 99;
        uint32_t got = 0;
        CHECK(!decodeRemainingLength(partial, 1, &used, &got));
        CHECK_EQ(used, static_cast<size_t>(1));
    }
    // 4 字节仍带续延位 → 协议错误(used == 4)
    {
        const uint8_t bad[] = {0x80, 0x80, 0x80, 0x80};
        size_t used = 0;
        uint32_t got = 0;
        CHECK(!decodeRemainingLength(bad, 4, &used, &got));
        CHECK_EQ(used, static_cast<size_t>(4));
    }
}

ES_TEST(mqtt_connect_byte_exact)
{
    // 最小 CONNECT: clientId="edge", keepalive=30, cleanSession
    // 期望字节: 10 | 剩余长度 16(0x10)| 00 04 'M' 'Q' 'T' 'T' | 04 | 02 | 00 1E | 00 04 e d g e
    ConnectOptions opt;
    opt.clientId = "edge";
    opt.keepaliveSec = 30;
    opt.cleanSession = true;
    const std::vector<uint8_t> bytes = encodeConnect(opt);
    const std::vector<uint8_t> want = {
        0x10, 0x10,
        0x00, 0x04, 'M', 'Q', 'T', 'T', // 协议名 "MQTT"
        0x04,                            // 协议级别 3.1.1
        0x02,                            // 标志: bit1 CleanSession
        0x00, 0x1E,                      // keepalive 30s(大端)
        0x00, 0x04, 'e', 'd', 'g', 'e'   // clientId
    };
    expectBytes(bytes, want);
}

ES_TEST(mqtt_connect_full_fields)
{
    // 全字段: Will(QoS1+retain)+ 用户名 + 密码
    // 标志位(OASIS 位序): bit1 CleanSession | bit2 Will | bit3 WillQoS1 | bit5 WillRetain
    //                     | bit6 Password | bit7 UserName = 0x02|0x04|0x08|0x20|0x40|0x80 = 0xEE
    ConnectOptions opt;
    opt.clientId = "gw-01";
    opt.keepaliveSec = 60;
    opt.cleanSession = true;
    opt.willTopic = "edge/status";
    opt.willPayload = "offline";
    opt.willQos = 1;
    opt.willRetain = true;
    opt.username = "user1";
    opt.password = "pass1";
    const std::vector<uint8_t> bytes = encodeConnect(opt);
    CHECK_EQ(bytes[0], static_cast<uint8_t>(0x10));
    CHECK_EQ(bytes[9], static_cast<uint8_t>(0xEE)); // 标志字节

    // 解码回读: 字段映射(topic=willTopic,payload=willPayload,topics[0]=clientId)
    std::string err;
    Packet p;
    size_t consumed = 0;
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(consumed, bytes.size());
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Connect));
    CHECK_EQ(p.topics.size(), static_cast<size_t>(1));
    CHECK_EQ(p.topics[0], std::string("gw-01"));
    CHECK_EQ(p.topic, std::string("edge/status"));
    CHECK_EQ(p.payload, std::string("offline"));
}

ES_TEST(mqtt_publish_roundtrip)
{
    std::string err;
    // QoS0: 固定头 0x30;无包 ID
    std::vector<uint8_t> bytes = encodePublish("t/a", "hello", 0, false, 0);
    Packet p;
    size_t consumed = 0;
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Publish));
    CHECK_EQ(p.qos, static_cast<uint8_t>(0));
    CHECK(!p.retain);
    CHECK(!p.dup);
    CHECK_EQ(p.packetId, static_cast<uint16_t>(0));
    CHECK_EQ(p.topic, std::string("t/a"));
    CHECK_EQ(p.payload, std::string("hello"));
    CHECK_EQ(bytes[0], static_cast<uint8_t>(0x30));

    // QoS1 + retain: 固定头 0x32|0x01 = 0x33;包 ID 7
    bytes = encodePublish("t/b", "world", 1, true, 7);
    CHECK_EQ(bytes[0], static_cast<uint8_t>(0x33));
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(p.qos, static_cast<uint8_t>(1));
    CHECK(p.retain);
    CHECK_EQ(p.packetId, static_cast<uint16_t>(7));
    CHECK_EQ(p.topic, std::string("t/b"));
    CHECK_EQ(p.payload, std::string("world"));

    // 大载荷: payload 200 字节 → 剩余长度跨字节编码,再解码仍一致
    const std::string big(200, 'x');
    bytes = encodePublish("t", big, 0, false, 0);
    CHECK_EQ(bytes[1], static_cast<uint8_t>(0xCB)); // 剩余长度 203 = 0xCB
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(p.payload.size(), static_cast<size_t>(200));
}

ES_TEST(mqtt_ack_and_control_roundtrip)
{
    std::string err;
    Packet p;
    size_t consumed = 0;

    // PUBACK 回环
    std::vector<uint8_t> bytes = encodePuback(0x1234);
    expectBytes(bytes, {0x40, 0x02, 0x12, 0x34});
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Puback));
    CHECK_EQ(p.packetId, static_cast<uint16_t>(0x1234));

    // SUBSCRIBE(2 主题): 0x82 + 包 ID + 主题与 QoS
    bytes = encodeSubscribe(5, {{"a/b", 0}, {"c/d", 1}});
    CHECK_EQ(bytes[0], static_cast<uint8_t>(0x82));
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(p.packetId, static_cast<uint16_t>(5));
    CHECK_EQ(p.topics.size(), static_cast<size_t>(2));
    CHECK_EQ(p.topics[0], std::string("a/b"));
    CHECK_EQ(p.topics[1], std::string("c/d"));
    CHECK_EQ(p.payload.size(), static_cast<size_t>(2));
    CHECK_EQ(static_cast<uint8_t>(p.payload[0]), static_cast<uint8_t>(0));
    CHECK_EQ(static_cast<uint8_t>(p.payload[1]), static_cast<uint8_t>(1));

    // UNSUBSCRIBE
    bytes = encodeUnsubscribe(9, {"x/y"});
    CHECK_EQ(bytes[0], static_cast<uint8_t>(0xA2));
    CHECK(decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
    CHECK_EQ(p.packetId, static_cast<uint16_t>(9));
    CHECK_EQ(p.topics.size(), static_cast<size_t>(1));
    CHECK_EQ(p.topics[0], std::string("x/y"));

    // PINGREQ / DISCONNECT: 固定 2 字节
    expectBytes(encodePingreq(), {0xC0, 0x00});
    expectBytes(encodeDisconnect(), {0xE0, 0x00});
    CHECK(decodePacket(encodePingreq().data(), 2, &consumed, &p, &err));
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Pingreq));

    // CONNACK: sessionPresent 复用 packetId(0/1),returnCode 回读
    const uint8_t connack[] = {0x20, 0x02, 0x01, 0x00};
    CHECK(decodePacket(connack, 4, &consumed, &p, &err));
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Connack));
    CHECK_EQ(p.packetId, static_cast<uint16_t>(1)); // sessionPresent=1
    CHECK_EQ(p.returnCode, static_cast<uint8_t>(0));

    // SUBACK: 授权 QoS 十进制串
    const uint8_t suback[] = {0x90, 0x03, 0x00, 0x05, 0x01};
    CHECK(decodePacket(suback, 5, &consumed, &p, &err));
    CHECK_EQ(static_cast<int>(p.type), static_cast<int>(PacketType::Suback));
    CHECK_EQ(p.packetId, static_cast<uint16_t>(5));
    CHECK_EQ(p.topics.size(), static_cast<size_t>(1));
    CHECK_EQ(p.topics[0], std::string("1"));
}

ES_TEST(mqtt_half_packet_need_more)
{
    // 完整 PUBLISH 报文少 1 字节: 解码失败且 err 为空 = "还需更多字节"
    const std::vector<uint8_t> full = encodePublish("t", "payload", 0, false, 0);
    for (size_t n = 0; n < full.size(); ++n)
    {
        std::string err = "dirty";
        Packet p;
        size_t consumed = 99;
        CHECK(!decodePacket(full.data(), n, &consumed, &p, &err));
        CHECK(err.empty());       // 数据不足的约定: err 必须为空
        CHECK_EQ(consumed, static_cast<size_t>(0));
    }
    // 恰好完整 → Ok
    std::string err;
    Packet p;
    size_t consumed = 0;
    CHECK(decodePacket(full.data(), full.size(), &consumed, &p, &err));
    CHECK_EQ(consumed, full.size());
    CHECK(err.empty());
}

ES_TEST(mqtt_invalid_packets)
{
    std::string err;
    Packet p;
    size_t consumed = 0;

    // 非法类型 0 与保留类型 15
    {
        const uint8_t bad0[] = {0x00, 0x00};
        CHECK(!decodePacket(bad0, 2, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
        const uint8_t bad15[] = {0xF0, 0x00};
        CHECK(!decodePacket(bad15, 2, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // PUBLISH QoS=3 非法
    {
        const uint8_t bad[] = {0x36, 0x04, 0x00, 0x01, 't', 0x00};
        CHECK(!decodePacket(bad, sizeof(bad), &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // SUBSCRIBE 标志必须 0010(0x80 非法)
    {
        const uint8_t bad[] = {0x80, 0x04, 0x00, 0x01, 0x00, 0x01, 't', 0x00};
        CHECK(!decodePacket(bad, sizeof(bad), &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // PUBACK 包 ID 为 0 非法
    {
        const uint8_t bad[] = {0x40, 0x02, 0x00, 0x00};
        CHECK(!decodePacket(bad, 4, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // CONNACK 返回码 > 5 非法;剩余长度 != 2 非法
    {
        const uint8_t bad[] = {0x20, 0x02, 0x00, 0x06};
        CHECK(!decodePacket(bad, 4, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
        const uint8_t badLen[] = {0x20, 0x03, 0x00, 0x00, 0x00};
        CHECK(!decodePacket(badLen, 5, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // PINGREQ 带载荷非法(剩余长度必须 0)
    {
        const uint8_t bad[] = {0xC0, 0x01, 0x00};
        CHECK(!decodePacket(bad, 3, &consumed, &p, &err));
        CHECK(!err.empty());
        err.clear();
    }
    // CONNECT 协议级别非 0x04 非法
    {
        ConnectOptions opt;
        opt.clientId = "c";
        std::vector<uint8_t> bytes = encodeConnect(opt);
        bytes[7] = 0x05; // 破坏协议级别
        CHECK(!decodePacket(bytes.data(), bytes.size(), &consumed, &p, &err));
        CHECK(!err.empty());
    }
}
