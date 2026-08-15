// 文件路径: src/core/event_bus.h
// 职责: 进程内发布/订阅事件总线(主题字符串 + payload 字符串),支持尾部 "#" 通配。
// 典型用途: 采集线程发布点数据/告警事件,遥测线程与命令服务器订阅转发。
//
// 设计要点:
// 1) publish 采用"快照后释放锁再回调": 锁内只复制匹配订阅者的 handler 并立即释放锁,
//    锁外逐个回调 —— 回调可能再次调用 publish/subscribe/unsubscribe(如告警回调里
//    触发 MQTT 发送、订阅新主题),若在锁内回调: 重入同一把非递归锁 → 死锁;
//    容器被改 → 迭代器失效/挂死。快照语义还保证回调期间的新订阅/退订不影响本次分发,
//    行为可预期。
// 2) 订阅号 id 由总线分配、单调递增(0 保留为无效);退订 O(n) 线性扫描 ——
//    订阅者数量级很小(几个到几十个),线性扫描比 map 缓存友好,注释说明取舍。
// 3) 通配: 仅支持 MQTT 风格的尾部 "#"("a/#" 匹配 "a" 与 "a/x/y",纯 "#" 匹配一切),
//    不做多级通配 —— 本工程事件主题是固定集合,过度通配反而掩盖拼写错误。
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace es {

using EventHandler = std::function<void(const std::string& topic, const std::string& payload)>;

class EventBus {
public:
    // 订阅;topic 支持尾部 "#" 通配;返回订阅号(>0,用于退订)
    size_t subscribe(const std::string& topic, EventHandler h);
    bool unsubscribe(size_t id);                 // 按订阅号退订;不存在返回 false
    void publish(const std::string& topic, const std::string& payload);
    [[nodiscard]] size_t listenerCount() const;  // 当前订阅数(含通配)

private:
    struct Subscription {
        size_t id = 0;
        std::string topic;
        EventHandler handler;
    };

    static bool matches(const std::string& subTopic, const std::string& pubTopic);

    mutable std::mutex m_mutex;
    std::vector<Subscription> m_subs;
    size_t m_nextId = 1; // 订阅号分配器(从 1 开始,0 保留为无效)
};

} // namespace es
