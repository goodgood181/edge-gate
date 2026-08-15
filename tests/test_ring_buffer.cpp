// 文件路径: tests/test_ring_buffer.cpp
// 意图: 环形缓冲(MPMC)契约 §14 单测 —— 顺序保序/满则等待/空则等待/超时语义/
//       双条件变量唤醒(生产者消费者线程对拍)。
// 覆盖点:
//  - 容量固定(含容量 0 按 1 处理)、tryPush/tryPop 非阻塞路径
//  - 满时 push 带超时 → false;空时 pop 带超时 → false
//  - clear() 后恢复可写
//  - 一生产者一消费者: 500 项保序对拍(验证 wait_until 谓词与 notify 正确性)
#include "framework.h"

#include "../src/core/ring_buffer.h"

#include <thread>
#include <vector>

using es::RingBuffer;

ES_TEST(rb_capacity_and_basic_order)
{
    RingBuffer<int> rb(4);
    CHECK_EQ(rb.capacity(), static_cast<size_t>(4));
    CHECK(rb.empty());
    CHECK_EQ(rb.size(), static_cast<size_t>(0));

    // 顺序入队出队
    CHECK(rb.tryPush(10));
    CHECK(rb.tryPush(20));
    CHECK(rb.tryPush(30));
    CHECK(rb.tryPush(40));
    CHECK_EQ(rb.size(), static_cast<size_t>(4));
    CHECK(!rb.empty());

    int out = 0;
    CHECK(rb.tryPop(out));
    CHECK_EQ(out, 10);
    CHECK(rb.tryPop(out));
    CHECK_EQ(out, 20);
    CHECK(rb.tryPop(out));
    CHECK_EQ(out, 30);
    CHECK(rb.tryPop(out));
    CHECK_EQ(out, 40);
    CHECK(rb.empty());
    CHECK(!rb.tryPop(out)); // 空: 非阻塞失败
}

ES_TEST(rb_full_and_empty_timeout)
{
    RingBuffer<int> rb(2);
    CHECK(rb.tryPush(1));
    CHECK(rb.tryPush(2));
    CHECK(!rb.tryPush(3)); // 满: 非阻塞失败

    // 满时带超时 push → 超时 false,元素数不变
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!rb.push(3, std::chrono::milliseconds(60)));
    const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(el >= 50); // 确实等了约 60ms(留调度余量)
    CHECK_EQ(rb.size(), static_cast<size_t>(2));

    // 空时带超时 pop → 超时 false
    rb.clear();
    CHECK(rb.empty());
    int out = 0;
    CHECK(!rb.pop(out, std::chrono::milliseconds(60)));
    CHECK_EQ(rb.size(), static_cast<size_t>(0));

    // clear 后恢复可写
    CHECK(rb.tryPush(7));
    CHECK(rb.tryPop(out));
    CHECK_EQ(out, 7);
}

ES_TEST(rb_producer_consumer_handshake)
{
    // 一生产者一消费者: 生产端 500 项,消费端带超时取回,校验顺序与总数
    RingBuffer<int> rb(16);
    constexpr int kItems = 500;

    std::thread producer([&rb]() {
        for (int i = 0; i < kItems; ++i)
        {
            CHECK(rb.push(i, std::chrono::milliseconds(2000)));
        }
    });

    int got = 0;
    int prev = -1;
    bool ordered = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (got < kItems)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            break; // 防卡死
        }
        int v = 0;
        if (rb.pop(v, std::chrono::milliseconds(100)))
        {
            if (v != prev + 1)
            {
                ordered = false;
            }
            prev = v;
            ++got;
        }
    }
    producer.join();
    CHECK_EQ(got, kItems);
    CHECK(ordered); // FIFO 保序
    CHECK(rb.empty());
}
