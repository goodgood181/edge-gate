// 文件路径: tests/test_rule_engine.cpp
// 意图: 阈值 + 迟滞告警状态机(契约 §8)单测 —— 进入/恢复边界精确验证。
// 迟滞语义(契约): 高限进入需 value >= highAlarm,恢复需 value < highAlarm - hysteresis;
//                 低限进入需 value <= lowAlarm,恢复需 value > lowAlarm + hysteresis。
// 覆盖点:
//  - 高限: 进入边界(恰好等于阈值)、恢复边界(恰好等于 high-hyst 不恢复,小于才恢复)
//  - 低限: 进入边界、恢复边界(恰好等于 low+hyst 不恢复,大于才恢复)
//  - 双限独立: 一次 evaluate 可同时输出"高限恢复 + 低限进入"两条事件
//  - 状态查询 stateOf / reset 后重新进入 / 未配置点恒 Normal
//  - 状态机语义: 只在翻转时发事件(同方向重复值不重复发)
#include "framework.h"

#include "../src/edge/rule_engine.h"
#include "../src/modbus/modbus_master.h"

#include <cstdint>
#include <string>
#include <vector>

using es::edge::AlarmEvent;
using es::edge::AlarmState;
using es::edge::RuleEngine;
using es::modbus::ModbusPoint;

namespace {

// 构造一个带高限/低限配置的点
ModbusPoint alarmPoint(const char* id, double high, bool highEn, double low, bool lowEn,
                       double hyst)
{
    ModbusPoint p;
    p.id = id;
    p.name = id;
    p.highAlarm = high;
    p.highAlarmEnabled = highEn;
    p.lowAlarm = low;
    p.lowAlarmEnabled = lowEn;
    p.hysteresis = hyst;
    return p;
}

} // namespace

ES_TEST(re_high_alarm_enter_recover_boundaries)
{
    RuleEngine re;
    std::vector<ModbusPoint> points;
    points.push_back(alarmPoint("t1", 80.0, true, 0.0, false, 2.0));
    re.configure(points);

    // 阈值下: 无事件
    CHECK(re.evaluate("t1", 79.999, 1).empty());
    CHECK_EQ(static_cast<int>(re.stateOf("t1")), static_cast<int>(AlarmState::Normal));

    // 进入边界: value >= 80 触发
    std::vector<AlarmEvent> ev = re.evaluate("t1", 80.0, 2);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(ev[0].active);
    CHECK_EQ(ev[0].level, std::string("high"));
    CHECK_EQ(ev[0].threshold, 80.0);
    CHECK_EQ(ev[0].pointId, std::string("t1"));
    CHECK_EQ(static_cast<int>(re.stateOf("t1")), static_cast<int>(AlarmState::Active));

    // 进入后阈值下但仍在迟滞带内: 不恢复、不发事件
    CHECK(re.evaluate("t1", 79.5, 3).empty());
    CHECK(re.evaluate("t1", 78.0, 4).empty()); // 恰好等于 high-hyst = 78: 不恢复(需 < 78)
    CHECK_EQ(static_cast<int>(re.stateOf("t1")), static_cast<int>(AlarmState::Active));

    // 恢复边界: value < 78 才恢复
    ev = re.evaluate("t1", 77.999, 5);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(!ev[0].active);
    CHECK_EQ(ev[0].level, std::string("high"));
    CHECK_EQ(static_cast<int>(re.stateOf("t1")), static_cast<int>(AlarmState::Normal));

    // 恢复后仍在阈值下: 不再发事件(状态机防抖)
    CHECK(re.evaluate("t1", 50.0, 6).empty());
}

ES_TEST(re_low_alarm_enter_recover_boundaries)
{
    RuleEngine re;
    std::vector<ModbusPoint> points;
    points.push_back(alarmPoint("p1", 0.0, false, 5.0, true, 1.0));
    re.configure(points);

    // 进入边界: value <= 5 触发
    CHECK(re.evaluate("p1", 5.1, 1).empty());
    std::vector<AlarmEvent> ev = re.evaluate("p1", 5.0, 2);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(ev[0].active);
    CHECK_EQ(ev[0].level, std::string("low"));

    // 迟滞带内(<= 6): 不恢复
    CHECK(re.evaluate("p1", 5.5, 3).empty());
    CHECK(re.evaluate("p1", 6.0, 4).empty()); // 恰好等于 low+hyst = 6: 不恢复(需 > 6)

    // 恢复边界: value > 6
    ev = re.evaluate("p1", 6.001, 5);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(!ev[0].active);
    CHECK_EQ(ev[0].level, std::string("low"));
}

ES_TEST(re_dual_limit_independent)
{
    // 同一路信号同时配高限与低限: 各方向独立翻转,一次 evaluate 可发 2 条事件
    RuleEngine re;
    std::vector<ModbusPoint> points;
    points.push_back(alarmPoint("d1", 90.0, true, 10.0, true, 5.0));
    re.configure(points);

    // 进入高限
    std::vector<AlarmEvent> ev = re.evaluate("d1", 95.0, 1);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(ev[0].active && ev[0].level == "high");

    // 猛跌到低限: 高限恢复 + 低限进入 → 2 条事件(0..2 契约允许)
    ev = re.evaluate("d1", 5.0, 2);
    CHECK_EQ(ev.size(), static_cast<size_t>(2));
    CHECK(!ev[0].active && ev[0].level == "high"); // 高限先恢复
    CHECK(ev[1].active && ev[1].level == "low");   // 低限后进入
    CHECK_EQ(static_cast<int>(re.stateOf("d1")), static_cast<int>(AlarmState::Active));

    // 回到中区: 低限恢复
    ev = re.evaluate("d1", 50.0, 3);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(!ev[0].active && ev[0].level == "low");
    CHECK_EQ(static_cast<int>(re.stateOf("d1")), static_cast<int>(AlarmState::Normal));
}

ES_TEST(re_reset_and_unknown_point)
{
    RuleEngine re;
    std::vector<ModbusPoint> points;
    points.push_back(alarmPoint("a1", 80.0, true, 0.0, false, 2.0));
    re.configure(points);

    // 未配置的点恒 Normal
    CHECK_EQ(static_cast<int>(re.stateOf("ghost")), static_cast<int>(AlarmState::Normal));
    CHECK(re.evaluate("ghost", 999.0, 1).empty());

    // 进入告警后 reset: 状态清空,再喂超阈值值 → 重新触发(复位语义)
    CHECK(!re.evaluate("a1", 85.0, 2).empty());
    CHECK_EQ(static_cast<int>(re.stateOf("a1")), static_cast<int>(AlarmState::Active));
    re.reset();
    CHECK_EQ(static_cast<int>(re.stateOf("a1")), static_cast<int>(AlarmState::Normal));
    std::vector<AlarmEvent> ev = re.evaluate("a1", 85.0, 3);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK(ev[0].active);

    // 重复 configure 覆盖旧配置
    std::vector<ModbusPoint> points2;
    points2.push_back(alarmPoint("a1", 100.0, true, 0.0, false, 1.0));
    re.configure(points2);
    CHECK(re.evaluate("a1", 99.0, 4).empty()); // 新阈值 100,99 不触发
    ev = re.evaluate("a1", 100.0, 5);
    CHECK_EQ(ev.size(), static_cast<size_t>(1));
    CHECK_EQ(ev[0].threshold, 100.0);
}
