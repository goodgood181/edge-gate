// src/edge/rule_engine.cpp
// 职责: 迟滞告警状态机实现。每个点维护高限/低限两个内部状态位,
//       只在状态位翻转时产出事件;stateOf 由两个状态位合成。
#include "rule_engine.h"

namespace es::edge {

void RuleEngine::configure(const std::vector<modbus::ModbusPoint>& points) {
    std::map<std::string, PointState> next;
    for (const auto& p : points) {
        PointState st;
        st.name = p.name;
        st.highEnabled = p.highAlarmEnabled;
        st.lowEnabled = p.lowAlarmEnabled;
        st.highAlarm = p.highAlarm;
        st.lowAlarm = p.lowAlarm;
        st.hysteresis = p.hysteresis;
        next[p.id] = st;  // 重新配置即复位内部状态(新状态位默认 false)
    }
    m_points_.swap(next);
}

std::vector<AlarmEvent> RuleEngine::evaluate(const std::string& pointId, double value,
                                             uint64_t ts) {
    std::vector<AlarmEvent> events;
    auto it = m_points_.find(pointId);
    if (it == m_points_.end()) return events;  // 未配置的点: 不产生告警

    PointState& st = it->second;
    const auto emit = [&](bool active, const char* level, double threshold) {
        AlarmEvent ev;
        ev.pointId = pointId;
        ev.name = st.name;
        ev.value = value;
        ev.threshold = threshold;
        ev.level = level;
        ev.active = active;
        ev.ts = ts;
        events.push_back(ev);
    };

    // 高限: 进入 active 需 value >= highAlarm;恢复需 value < highAlarm - hysteresis
    if (st.highEnabled) {
        if (!st.highActive && value >= st.highAlarm) {
            st.highActive = true;
            emit(true, "high", st.highAlarm);
        } else if (st.highActive && value < st.highAlarm - st.hysteresis) {
            st.highActive = false;
            emit(false, "high", st.highAlarm);
        }
    }
    // 低限: 进入 active 需 value <= lowAlarm;恢复需 value > lowAlarm + hysteresis
    if (st.lowEnabled) {
        if (!st.lowActive && value <= st.lowAlarm) {
            st.lowActive = true;
            emit(true, "low", st.lowAlarm);
        } else if (st.lowActive && value > st.lowAlarm + st.hysteresis) {
            st.lowActive = false;
            emit(false, "low", st.lowAlarm);
        }
    }
    return events;
}

AlarmState RuleEngine::stateOf(const std::string& pointId) const {
    const auto it = m_points_.find(pointId);
    if (it == m_points_.end()) return AlarmState::Normal;
    const PointState& st = it->second;
    return (st.highActive || st.lowActive) ? AlarmState::Active : AlarmState::Normal;
}

void RuleEngine::reset() {
    for (auto& kv : m_points_) {
        kv.second.highActive = false;
        kv.second.lowActive = false;
    }
}

}  // namespace es::edge
