// 文件路径: src/core/state_machine.cpp
// 职责: state_machine.h 的实现;默认转移表来自契约 §10。
#include "state_machine.h"

#include <mutex>
#include <utility>

namespace es {

StateMachine::StateMachine() {
    // 契约 §10 默认转移表
    addTransition(State::Init, Event::ConfigLoaded, State::Configuring);
    addTransition(State::Configuring, Event::Start, State::Running);
    addTransition(State::Running, Event::Stop, State::Stopped);
    addTransition(State::Running, Event::SerialError, State::Fault);
    addTransition(State::Fault, Event::SerialRecovered, State::Running);
    addTransition(State::Fault, Event::Stop, State::Stopped);
    // 任意态 + Fatal → Stopped(5 条)
    addTransition(State::Init, Event::Fatal, State::Stopped);
    addTransition(State::Configuring, Event::Fatal, State::Stopped);
    addTransition(State::Running, Event::Fatal, State::Stopped);
    addTransition(State::Fault, Event::Fatal, State::Stopped);
    addTransition(State::Stopped, Event::Fatal, State::Stopped);
}

void StateMachine::addTransition(State from, Event ev, State to) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& r : m_rules) {
        if (r.from == from && r.ev == ev) {
            r.to = to; // 覆盖已有规则(默认表可被调用方定制)
            return;
        }
    }
    m_rules.push_back(Rule{from, ev, to});
}

bool StateMachine::dispatch(Event ev) {
    std::unique_lock<std::mutex> lk(m_mutex);
    for (const auto& r : m_rules) {
        if (r.from == m_state && r.ev == ev) {
            const State from = m_state;
            m_state = r.to;
            m_history.emplace_back(from, ev, r.to);
            if (m_history.size() > kMaxHistory) {
                m_history.erase(m_history.begin()); // 只保留最近 64 条
            }
            const State to = r.to;
            TransitionAction act = m_action;  // 锁内拷贝: 避免与 onTransition 的写构成数据竞争(UB)
            lk.unlock(); // 先解锁再回调: 回调内可安全再调 dispatch/state,不持锁进用户代码
            if (act) {
                act(from, to, ev);
            }
            return true;
        }
    }
    return false; // 未定义转移: 状态保持不变(调用方决定是否告警)
}

State StateMachine::state() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_state;
}

void StateMachine::onTransition(TransitionAction action) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_action = std::move(action);
}

const std::vector<std::tuple<State, Event, State>>& StateMachine::history() const {
    // 契约要求返回引用,无法在返回引用时持锁;仅供单线程/测试场景(见头文件注释)
    return m_history;
}

std::string StateMachine::stateName(State s) {
    switch (s) {
        case State::Init:        return "Init";
        case State::Configuring: return "Configuring";
        case State::Running:     return "Running";
        case State::Fault:       return "Fault";
        case State::Stopped:     return "Stopped";
    }
    return "Unknown";
}

std::string StateMachine::eventName(Event e) {
    switch (e) {
        case Event::ConfigLoaded:    return "ConfigLoaded";
        case Event::Start:           return "Start";
        case Event::Stop:            return "Stop";
        case Event::SerialError:     return "SerialError";
        case Event::SerialRecovered: return "SerialRecovered";
        case Event::Fatal:           return "Fatal";
    }
    return "Unknown";
}

} // namespace es
