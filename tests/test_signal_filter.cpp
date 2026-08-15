// 文件路径: tests/test_signal_filter.cpp
// 意图: 滑动平均滤波器(契约 §8)单测 —— 数值正确性与窗口语义。
// 覆盖点:
//  - 窗口未满: 输出已收集样本的均值(递进均值);满窗后输出满窗均值
//  - ready() 语义: 样本数 >= window 才 true
//  - 窗口滑动: 新样本进入、旧样本退出(运行和 O(1) 维护)
//  - 边界: window=0 按 1 处理;window=1 直通;reset 后重新累积
#include "framework.h"

#include "../src/edge/signal_filter.h"

using es::edge::SignalFilter;

ES_TEST(sf_moving_average_values)
{
    // 窗口 3: 1,2,3 → 递进均值 1 / 1.5 / 2;满窗后滑动
    SignalFilter f(3);
    CHECK_NEAR(f.push(1.0), 1.0, 1e-12);
    CHECK(!f.ready()); // 1/3 样本
    CHECK_NEAR(f.push(2.0), 1.5, 1e-12);
    CHECK(!f.ready());
    CHECK_NEAR(f.push(3.0), 2.0, 1e-12);
    CHECK(f.ready()); // 3/3 样本
    // 满窗滑动: 窗口内变为 2,3,4 → 均值 3
    CHECK_NEAR(f.push(4.0), 3.0, 1e-12);
    CHECK_NEAR(f.push(6.0), (3.0 + 4.0 + 6.0) / 3.0, 1e-12);
}

ES_TEST(sf_window_boundaries)
{
    // window=1: 直通,第一次 push 即 ready
    SignalFilter f1(1);
    CHECK_NEAR(f1.push(42.0), 42.0, 1e-12);
    CHECK(f1.ready());
    CHECK_NEAR(f1.push(7.0), 7.0, 1e-12);

    // window=0 按 1 处理
    SignalFilter f0(0);
    CHECK_NEAR(f0.push(3.5), 3.5, 1e-12);
    CHECK(f0.ready());
}

ES_TEST(sf_reset)
{
    SignalFilter f(2);
    f.push(10.0);
    f.push(20.0);
    CHECK(f.ready());
    f.reset();
    CHECK(!f.ready()); // 历史清空
    CHECK_NEAR(f.push(100.0), 100.0, 1e-12); // 重新累积
    CHECK(!f.ready());
    CHECK_NEAR(f.push(0.0), 50.0, 1e-12);
    CHECK(f.ready());
}
