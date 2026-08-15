// 文件路径: tests/test_state_machine.cpp
// 意图: 网关主状态机(契约 §9/§10)单测 —— 默认转移表全量、非法转移拒绝、
//       转移历史(含 64 条环形上限)、转移回调、状态/事件名。
// 覆盖点:
//  - 默认表: Init+ConfigLoaded→Configuring、Configuring+Start→Running、
//    Running+Stop→Stopped、Running+SerialError→Fault、Fault+SerialRecovered→Running、
//    Fault+Stop→Stopped、任意态+Fatal→Stopped
//  - 非法转移: 状态不变、返回 false
//  - addTransition 覆盖默认规则
//  - 历史: 最近 64 条,最旧被淘汰;回调参数 (from, to, ev) 正确
#include "framework.h"

#include "../src/core/state_machine.h"

#include <string>
#include <tuple>

using es::Event;
using es::State;
using es::StateMachine;

ES_TEST(sm_default_transition_table)
{
    StateMachine sm;
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Init));

    // Init → Configuring → Running(主流程)
    CHECK(sm.dispatch(Event::ConfigLoaded));
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Configuring));
    CHECK(sm.dispatch(Event::Start));
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Running));

    // Running + Stop → Stopped
    CHECK(sm.dispatch(Event::Stop));
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Stopped));

    // 重新走一遍到 Running,测 Fault 两条路径
    CHECK(sm.dispatch(Event::Fatal));
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Stopped));
    // Stopped 态没有到 Configuring 的规则,需用 addTransition 覆盖才能回 Init ——
    // 直接验证默认规则集合的另一半: 从 Init 起 SerialError 非法
    StateMachine sm2;
    CHECK(!sm2.dispatch(Event::SerialError));
    CHECK_EQ(static_cast<int>(sm2.state()), static_cast<int>(State::Init));

    // 用独立实例验证 Fault 路径
    StateMachine sm3;
    sm3.dispatch(Event::ConfigLoaded);
    sm3.dispatch(Event::Start);
    CHECK(sm3.dispatch(Event::SerialError));
    CHECK_EQ(static_cast<int>(sm3.state()), static_cast<int>(State::Fault));
    CHECK(sm3.dispatch(Event::SerialRecovered));
    CHECK_EQ(static_cast<int>(sm3.state()), static_cast<int>(State::Running));
    // Fault + Stop → Stopped
    sm3.dispatch(Event::SerialError);
    CHECK(sm3.dispatch(Event::Stop));
    CHECK_EQ(static_cast<int>(sm3.state()), static_cast<int>(State::Stopped));
}

ES_TEST(sm_invalid_transition_rejected)
{
    StateMachine sm;
    sm.dispatch(Event::ConfigLoaded);
    sm.dispatch(Event::Start);
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Running));

    // Running 下这些事件无规则 → 拒绝且状态不变
    CHECK(!sm.dispatch(Event::ConfigLoaded));
    CHECK(!sm.dispatch(Event::Start));
    CHECK(!sm.dispatch(Event::SerialRecovered));
    CHECK_EQ(static_cast<int>(sm.state()), static_cast<int>(State::Running));

    // 自定义规则覆盖: Fault + Start → Running(测试注入)
    StateMachine sm2;
    sm2.addTransition(State::Fault, Event::Start, State::Running);
    sm2.dispatch(Event::ConfigLoaded);
    sm2.dispatch(Event::Start);
    sm2.dispatch(Event::SerialError);
    CHECK_EQ(static_cast<int>(sm2.state()), static_cast<int>(State::Fault));
    CHECK(sm2.dispatch(Event::Start)); // 覆盖后的新规则生效
    CHECK_EQ(static_cast<int>(sm2.state()), static_cast<int>(State::Running));
}

ES_TEST(sm_history_and_callback)
{
    StateMachine sm;
    // 回调: 记录最后一次 (from, to, ev)
    State lastFrom = State::Init;
    State lastTo = State::Init;
    Event lastEv = Event::ConfigLoaded;
    sm.onTransition([&](State from, State to, Event ev) {
        lastFrom = from;
        lastTo = to;
        lastEv = ev;
    });

    sm.dispatch(Event::ConfigLoaded);
    sm.dispatch(Event::Start);
    sm.dispatch(Event::SerialError);
    sm.dispatch(Event::SerialRecovered);

    // 回调参数顺序 (from, to, ev)
    CHECK_EQ(static_cast<int>(lastFrom), static_cast<int>(State::Fault));
    CHECK_EQ(static_cast<int>(lastTo), static_cast<int>(State::Running));
    CHECK_EQ(static_cast<int>(lastEv), static_cast<int>(Event::SerialRecovered));

    // 历史: 4 条转移,顺序正确
    const auto& hist = sm.history();
    CHECK_EQ(hist.size(), static_cast<size_t>(4));
    CHECK(std::get<0>(hist[0]) == State::Init && std::get<1>(hist[0]) == Event::ConfigLoaded &&
          std::get<2>(hist[0]) == State::Configuring);
    CHECK(std::get<1>(hist[3]) == Event::SerialRecovered && std::get<2>(hist[3]) == State::Running);
}

ES_TEST(sm_history_capped_at_64)
{
    StateMachine sm;
    // 默认表里 Stopped 态只认 Fatal,补一条规则让每轮循环产生 4 条转移:
    // ConfigLoaded → Start → Stop → Fatal(20 轮 = 80 条,超 64 上限)
    sm.addTransition(State::Stopped, Event::ConfigLoaded, State::Configuring);
    for (int i = 0; i < 20; ++i)
    {
        CHECK(sm.dispatch(Event::ConfigLoaded));
        CHECK(sm.dispatch(Event::Start));
        CHECK(sm.dispatch(Event::Stop));
        CHECK(sm.dispatch(Event::Fatal));
    }
    const auto& hist = sm.history();
    CHECK_EQ(hist.size(), static_cast<size_t>(64)); // 环形: 只留最近 64 条
    // 最旧一条应是第 17 轮(第 65 条)开始的 ConfigLoaded → Configuring
    CHECK(std::get<1>(hist[0]) == Event::ConfigLoaded && std::get<2>(hist[0]) == State::Configuring);
    // 最新一条是 Fatal → Stopped
    CHECK(std::get<1>(hist[63]) == Event::Fatal && std::get<2>(hist[63]) == State::Stopped);
}

ES_TEST(sm_names)
{
    CHECK_EQ(StateMachine::stateName(State::Running), std::string("Running"));
    CHECK_EQ(StateMachine::stateName(State::Fault), std::string("Fault"));
    CHECK_EQ(StateMachine::eventName(Event::SerialRecovered), std::string("SerialRecovered"));
    CHECK_EQ(StateMachine::eventName(Event::Stop), std::string("Stop"));
    // 非法枚举值应返回可读的占位(不崩溃)
    CHECK(!StateMachine::stateName(static_cast<State>(99)).empty());
    CHECK(!StateMachine::eventName(static_cast<Event>(99)).empty());
}
