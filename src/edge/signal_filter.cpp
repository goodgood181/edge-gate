// src/edge/signal_filter.cpp
// 职责: 滑动平均实现(环形缓冲 + 运行和,O(1) 每样本)。
#include "signal_filter.h"

#include <algorithm>

namespace es::edge {

SignalFilter::SignalFilter(size_t window)
    : m_window_(std::max<size_t>(1, window)), m_buf_(m_window_, 0.0) {}

double SignalFilter::push(double v) {
    // 窗口已满: 先减掉即将被覆盖的旧值(保持运行和正确)
    if (m_count_ == m_window_) {
        m_sum_ -= m_buf_[m_head_];
    } else {
        ++m_count_;
    }
    m_buf_[m_head_] = v;
    m_sum_ += v;
    m_head_ = (m_head_ + 1) % m_window_;  // 环形回绕
    return m_sum_ / static_cast<double>(m_count_);
}

bool SignalFilter::ready() const { return m_count_ >= m_window_; }

void SignalFilter::reset() {
    m_head_ = 0;
    m_count_ = 0;
    m_sum_ = 0.0;
    std::fill(m_buf_.begin(), m_buf_.end(), 0.0);
}

}  // namespace es::edge
