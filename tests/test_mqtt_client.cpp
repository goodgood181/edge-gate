// 文件路径: tests/test_mqtt_client.cpp
// 意图: MqttClient 与 fake_broker(用 es::mqtt 编解码自举)的端到端会话测试。
// 覆盖点(契约 §14):
//  - 握手: CONNECT → CONNACK → isConnected / 状态回调 connected=true
//  - 订阅: SUBSCRIBE → SUBACK;broker 侧 subscribeCount 增长
//  - 发布 QoS0/QoS1: broker 收到消息内容;QoS1 收到 PUBACK(publishesAcked)
//  - 接收: broker 主动推送 QoS0/QoS1 → 消息回调触发;QoS1 客户端回 PUBACK
//  - keepalive: keepaliveSec=1 → 空闲 0.7s 后自动 PINGREQ(broker 计数)
//  - 断线重连: broker 强制断开 → 客户端感知 → 指数退避重连 → 恢复发布
//  - 优雅退出: stop() 后 broker 侧连接关闭
#include "framework.h"
#include "fake_broker.h"

#include "../src/net/mqtt_client.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using es::MqttClient;
using es::MqttConfig;
using es::MqttMessage;
using estest::FakeBroker;

namespace {

// 记录回调(消息与连接状态)的收集器,线程安全
struct Sink
{
    mutable std::mutex mtx;
    std::vector<MqttMessage> messages;
    std::vector<std::pair<bool, std::string>> states;

    void onMessage(const MqttMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx);
        messages.push_back(m);
    }
    void onState(bool connected, const std::string& reason)
    {
        std::lock_guard<std::mutex> lk(mtx);
        states.push_back({connected, reason});
    }
    size_t msgCount() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return messages.size();
    }
    size_t stateCount() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return states.size();
    }
    bool everConnected() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& s : states)
        {
            if (s.first)
            {
                return true;
            }
        }
        return false;
    }
    bool everDisconnected() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& s : states)
        {
            if (!s.first)
            {
                return true;
            }
        }
        return false;
    }
};

MqttConfig makeCfg(const FakeBroker& b, const char* clientId, uint16_t keepalive = 30)
{
    MqttConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = b.port();
    cfg.clientId = clientId;
    cfg.keepaliveSec = keepalive;
    cfg.reconnectBaseMs = 200; // 测试用短退避,加速重连断言
    cfg.reconnectMaxMs = 1000;
    return cfg;
}

} // namespace

ES_TEST(mqtt_client_handshake_subscribe)
{
    FakeBroker broker;
    std::string err;
    CHECK(broker.start(&err));
    CHECK(broker.port() > 0);

    Sink sink;
    MqttClient client(makeCfg(broker, "t-handshake"));
    client.setStateHandler([&](bool c, const std::string& r) { sink.onState(c, r); });
    CHECK(client.start(&err));
    CHECK(estest::waitUntil(3000, [&]() { return client.isConnected(); }));
    CHECK_EQ(broker.connectCount(), static_cast<uint64_t>(1));
    CHECK(sink.everConnected());

    // 订阅: SUBACK 后订阅成功(broker 计数)
    CHECK(client.subscribe("edge/test", 1, &err));
    CHECK(estest::waitUntil(3000, [&]() { return broker.subscribeCount() >= 1; }));

    client.stop();
    CHECK(estest::waitUntil(2000, [&]() { return broker.clientCount() == 0; }));
}

ES_TEST(mqtt_client_publish_qos0_qos1)
{
    FakeBroker broker;
    std::string err;
    CHECK(broker.start(&err));

    Sink sink;
    MqttClient client(makeCfg(broker, "t-pub"));
    client.setMessageHandler([&](const MqttMessage& m) { sink.onMessage(m); });
    CHECK(client.start(&err));
    CHECK(estest::waitUntil(3000, [&]() { return client.isConnected(); }));

    // QoS0 发布: broker 收到 topic/payload/qos
    CHECK(client.publish("edge/q0", "zero", 0, false, &err));
    CHECK(estest::waitUntil(3000, [&]() { return broker.receivedCount() >= 1; }));
    FakeBroker::RecvMsg m0;
    CHECK(broker.getReceived(0, &m0));
    CHECK_EQ(m0.topic, std::string("edge/q0"));
    CHECK_EQ(m0.payload, std::string("zero"));
    CHECK_EQ(m0.qos, static_cast<uint8_t>(0));

    // QoS1 发布: broker 回 PUBACK → publishesAcked 增长
    CHECK(client.publish("edge/q1", "one", 1, false, &err));
    CHECK(estest::waitUntil(3000, [&]() { return broker.receivedCount() >= 2; }));
    FakeBroker::RecvMsg m1;
    CHECK(broker.getReceived(1, &m1));
    CHECK_EQ(m1.topic, std::string("edge/q1"));
    CHECK_EQ(m1.qos, static_cast<uint8_t>(1));
    CHECK(estest::waitUntil(3000, [&]() { return client.stats().publishesAcked >= 1; }));

    // retain 标志透传
    CHECK(client.publish("edge/ret", "r", 0, true, &err));
    CHECK(estest::waitUntil(3000, [&]() { return broker.receivedCount() >= 3; }));
    FakeBroker::RecvMsg m2;
    CHECK(broker.getReceived(2, &m2));
    CHECK(m2.retain);

    client.stop();
}

ES_TEST(mqtt_client_receive_and_pingreq)
{
    FakeBroker broker;
    std::string err;
    CHECK(broker.start(&err));

    Sink sink;
    // keepalive=1s: 空闲 0.7s 后应自动发 PINGREQ
    MqttClient client(makeCfg(broker, "t-rx", 1));
    client.setMessageHandler([&](const MqttMessage& m) { sink.onMessage(m); });
    CHECK(client.start(&err));
    CHECK(estest::waitUntil(3000, [&]() { return client.isConnected(); }));

    // broker 主动推 QoS0 → 消息回调
    CHECK(broker.pushMessage("edge/in", "hello-push", 0));
    CHECK(estest::waitUntil(3000, [&]() { return sink.msgCount() >= 1; }));
    {
        std::lock_guard<std::mutex> lk(sink.mtx);
        CHECK_EQ(sink.messages[0].topic, std::string("edge/in"));
        CHECK_EQ(sink.messages[0].payload, std::string("hello-push"));
        CHECK_EQ(sink.messages[0].qos, static_cast<uint8_t>(0));
    }

    // broker 主动推 QoS1 → 回调 + 客户端自动回 PUBACK(broker 侧可观察到)
    CHECK(broker.pushMessage("edge/in1", "qos1-push", 1));
    CHECK(estest::waitUntil(3000, [&]() { return sink.msgCount() >= 2; }));
    CHECK(estest::waitUntil(3000, [&]() { return broker.pubackRecvCount() >= 1; }));

    // keepalive PINGREQ: 空闲 0.7s 内自动发出(broker 计数增长)
    CHECK(estest::waitUntil(4000, [&]() { return broker.pingReqCount() >= 1; }));

    client.stop();
}

ES_TEST(mqtt_client_disconnect_reconnect)
{
    FakeBroker broker;
    std::string err;
    CHECK(broker.start(&err));

    Sink sink;
    MqttClient client(makeCfg(broker, "t-reconnect"));
    client.setStateHandler([&](bool c, const std::string& r) { sink.onState(c, r); });
    CHECK(client.start(&err));
    CHECK(estest::waitUntil(3000, [&]() { return client.isConnected(); }));
    const uint64_t connectsBefore = broker.connectCount();

    // 强制断开: 客户端应感知(recv EOF)→ 状态回调 disconnected → 退避重连
    broker.dropAllClients();
    CHECK(estest::waitUntil(5000, [&]() { return sink.everDisconnected(); }));
    CHECK(estest::waitUntil(8000, [&]() {
        return client.isConnected() && broker.connectCount() > connectsBefore;
    }));
    CHECK(client.stats().reconnectCount >= 1);

    // 重连后会话可用: 发布再次成功(clean session 会重建订阅)
    CHECK(client.subscribe("edge/again", 0, &err));
    CHECK(estest::waitUntil(3000, [&]() { return broker.subscribeCount() >= 1; }));
    CHECK(client.publish("edge/after-reconnect", "back", 1, false, &err));
    CHECK(estest::waitUntil(3000, [&]() {
        return broker.receivedCount() >= 1 && client.stats().publishesAcked >= 1;
    }));

    client.stop();
}
