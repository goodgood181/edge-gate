// 文件路径: src/core/state_machine.h
// 职责: 网关主状态机(Init → Configuring → Running → Fault → Stopped),驱动采集/告警/
//       命令处理的全局行为;内置契约 §10 默认转移表,可增量覆盖。
//
// 设计要点:
// 1) 状态机 vs 散落 if-else: 网关有 5 状态 × 6 事件,若用散落 if-else,新增事件要翻遍
//    所有调用点,极易漏改(尤其 Fault 恢复路径);集中式转移表把"状态 × 事件 → 下一状态"
//    声明化,非法转移直接返回 false 且状态不变,行为可穷举、可测试、可画图。
// 2) 默认表 + 覆盖: 构造时注入契约默认表,addTransition 可替换任意 (from, event) 规则
//    (测试注入/扩展);查找用线性扫描 —— 规则 ≤ 十几条,O(n) 与哈希表差异可忽略。
// 3) 转移历史: 保留最近 64 条 (from, event, to),排障时直接回看"怎么进 Fault 的";
//    注意 history() 按契约返回引用、无锁读取,仅限单线程/测试场景(并发 dispatch 时外部同步)。
// 4) 回调时机: 先更新状态、再解锁、最后回调 onTransition —— 回调内可安全再调
//    dispatch/state(查到的已是新状态),且不会带着锁进入用户代码(防死锁)。
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace es {

enum class State { Init, Configuring, Running, Fault, Stopped };
enum class Event { ConfigLoaded, Start, Stop, SerialError, SerialRecovered, Fatal };

// 转移回调: 参数顺序 (from, to, ev);注意与 history 元组 (from, ev, to) 顺序不同
using TransitionAction = std::function<void(State from, State to, Event ev)>;

class StateMachine {
public:
    StateMachine(); // 内置契约默认转移表

    void addTransition(State from, Event ev, State to);                    // 覆盖或新增规则
    bool dispatch(Event ev);                                               // 触发事件;发生转移返回 true
    [[nodiscard]] State state() const;
    void onTransition(TransitionAction action);                            // 注册全局转移回调(后注册覆盖)
    const std::vector<std::tuple<State, Event, State>>& history() const;   // 最近 64 条 (from, ev, to)

    static std::string stateName(State s);
    static std::string eventName(Event e);

private:
    struct Rule {
        State from;
        Event ev;
        State to;
    };

    static constexpr size_t kMaxHistory = 64;

    mutable std::mutex m_mutex;             // 保护状态/规则/历史
    std::vector<Rule> m_rules;              // 转移表
    State m_state = State::Init;
    std::vector<std::tuple<State, Event, State>> m_history;
    TransitionAction m_action;
};

} // namespace es
