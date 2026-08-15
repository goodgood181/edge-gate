// src/edge/signal_filter.h
// 职责: 滑动平均滤波器 —— 采集原始值去抖,输出平滑后的物理量。
// 设计要点(与另两种常用滤波器对比):
//  - 滑动平均(本实现): 固定窗口内算术平均,O(1) 更新(环形缓冲 + 运行和);
//    对白噪声抑制好,但对脉冲尖峰不敏感(尖峰被摊薄但仍在);
//    延迟与窗口成正比(≈(window-1)/2 个采样周期),窗口越大越平滑、越迟钝;
//  - 一阶低通(EMA): y = y_prev + α*(x - y_prev),只需 2 个变量、无窗口内存,
//    适合 MCU 内存紧张场景;但 α 与采样周期耦合,周期性噪声抑制不如滑动平均;
//  - 中值滤波: 对脉冲/野值(传感器毛刺)鲁棒性最好,但需排序(O(w log w))
//    且对高斯噪声平滑能力弱;
//  - 取舍结论: 工业采集先用滑动平均压随机噪声,配合 RuleEngine 的迟滞
//    抗抖动 —— 滤波与迟滞各管一段,职责清晰。
#pragma once

#include <cstddef>
#include <vector>

namespace es::edge {

class SignalFilter {
public:
    // window: 滑动窗口大小(>= 1;传 0 时按 1 处理)
    explicit SignalFilter(size_t window);

    // 推入新样本并返回当前均值(窗口未满时返回已收集样本的均值)
    double push(double v);

    // 样本数是否已达到窗口大小(达到后输出才是"满窗均值")
    [[nodiscard]] bool ready() const;

    void reset();  // 清空历史,重新累积

private:
    size_t m_window_;
    std::vector<double> m_buf_;  // 环形缓冲
    size_t m_head_ = 0;          // 下一个写入位置
    size_t m_count_ = 0;         // 已收集样本数(<= window)
    double m_sum_ = 0.0;         // 窗口内样本和(O(1) 增量维护)
};

}  // namespace es::edge
