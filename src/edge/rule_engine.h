// src/edge/rule_engine.h
// 职责: 阈值 + 迟滞告警状态机 —— 对滤波后的物理量做高/低限告警,
//       仅当状态翻转时输出 AlarmEvent。
// 设计要点:
//  - 为什么迟滞(滞回): 阈值附近噪声会让裸比较"进入/恢复"来回抖,
//    继电器/阀门/上云消息被反复触发。迟滞带把进入阈值与恢复阈值分开:
//    高限进入需 value >= highAlarm,恢复需 value < highAlarm - hysteresis;
//    低限进入需 value <= lowAlarm,恢复需 value > lowAlarm + hysteresis。
//    只要噪声幅度小于迟滞带,状态就稳定 —— 这是工业现场最经典的做法;
//  - 为什么状态机而非每次全量比较: 网关每次采样都做比较,但只有
//    Normal↔Active 翻转才有业务价值(报警/恢复消息)。状态机保证每个
//    翻转恰好输出一个事件,上云流量 = 事件数,而不是采样数 ——
//    遥测周期 1s、告警事件一天几个,流量差 4~5 个数量级;
//  - 与 RuleEngine 上游配合: SignalFilter 压随机噪声,迟滞压阈值抖动,
//    两层防线职责分离;质量位(quality != Good)时网关不应喂给本引擎。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../modbus/modbus_master.h"

namespace es::edge {

enum class AlarmState { Normal, Active };

// 一次状态翻转事件(进入告警 active=true / 恢复 active=false)
struct AlarmEvent {
    std::string pointId;
    std::string name;
    double value;        // 触发时刻的采样值
    double threshold;    // 触发阈值(高限 highAlarm / 低限 lowAlarm)
    std::string level;   // "high" | "low"
    bool active;         // true = 进入告警,false = 恢复
    uint64_t ts;         // 毫秒时间戳
};

class RuleEngine {
public:
    // 从点表提取阈值配置(高/低限使能位、阈值、迟滞带、点名)
    void configure(const std::vector<modbus::ModbusPoint>& points);

    // 评估一个点的当前值;仅在状态翻转时输出事件(0..2 条: 高/低限可各自翻转)
    std::vector<AlarmEvent> evaluate(const std::string& pointId, double value, uint64_t ts);

    // 当前告警状态(任一方向 active 即 Active;未配置的点恒为 Normal)
    [[nodiscard]] AlarmState stateOf(const std::string& pointId) const;

    void reset();  // 清空各点 active 标志(保留配置;网关启动/重连后调用)

private:
    struct PointState {
        std::string name;
        bool highEnabled = false;
        bool lowEnabled = false;
        double highAlarm = 0;
        double lowAlarm = 0;
        double hysteresis = 0;
        bool highActive = false;  // 内部状态: 高限是否处于告警
        bool lowActive = false;   // 内部状态: 低限是否处于告警
    };
    std::map<std::string, PointState> m_points_;
};

}  // namespace es::edge
