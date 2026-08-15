// 文件路径: src/core/event_bus.cpp
// 职责: event_bus.h 的实现,设计要点见头文件注释。
#include "event_bus.h"

#include <utility>

namespace es {

bool EventBus::matches(const std::string& sub, const std::string& pub) {
    if (sub == "#") {
        return true; // 单 "#" 匹配一切主题
    }
    if (sub.size() >= 2 && sub.compare(sub.size() - 2, 2, "/#") == 0) {
        // "a/#" 匹配 "a" 以及 "a/..." 任意前缀(与 MQTT 语义一致)
        const std::string prefix = sub.substr(0, sub.size() - 2);
        if (pub == prefix) {
            return true;
        }
        return pub.size() > prefix.size() &&
               pub.compare(0, prefix.size(), prefix) == 0 &&
               pub[prefix.size()] == '/';
    }
    return sub == pub; // 精确匹配
}

size_t EventBus::subscribe(const std::string& topic, EventHandler h) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const size_t id = m_nextId++;
    m_subs.push_back(Subscription{id, topic, std::move(h)});
    return id;
}

bool EventBus::unsubscribe(size_t id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto it = m_subs.begin(); it != m_subs.end(); ++it) {
        if (it->id == id) {
            m_subs.erase(it);
            return true;
        }
    }
    return false;
}

void EventBus::publish(const std::string& topic, const std::string& payload) {
    // 快照: 锁内只拷贝匹配的 handler(函数对象拷贝,开销小),锁外回调(防死锁,见头文件)
    std::vector<EventHandler> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        snapshot.reserve(m_subs.size());
        for (const auto& s : m_subs) {
            if (s.handler && matches(s.topic, topic)) {
                snapshot.push_back(s.handler);
            }
        }
    }
    for (const auto& h : snapshot) {
        h(topic, payload);
    }
}

size_t EventBus::listenerCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_subs.size();
}

} // namespace es
