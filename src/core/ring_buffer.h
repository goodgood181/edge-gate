// 文件路径: src/core/ring_buffer.h
// 职责: 固定容量 MPMC(多生产者/多消费者)环形缓冲(纯模板,头文件实现),
//       生产/消费线程间解耦。典型用途: 采集线程推入原始帧,协议解析线程取出,
//       生产速率波动不阻塞采集线程。
//
// 设计要点:
// 1) 为什么双条件变量 notEmpty/notFull 而不是一个:
//    a) 单条件变量时,生产者入队后只能 notify_all —— 否则可能唤醒的仍是生产者,
//       消费者继续睡,即"丢失唤醒";notify_all 会把所有等待者全部叫醒,其中大部分
//       条件仍不满足、重新睡下,即"惊群",浪费调度且放大锁竞争;
//    b) 双条件变量 + notify_one: 入队只唤醒一个消费者、出队只唤醒一个生产者,
//       精确点对点通知,无惊群;两个条件变量天然避免"生产者等空位、消费者等数据"
//       相互等待的死锁闭环 —— 任一时刻至少一边有可用资源。
// 2) 防虚假唤醒: 所有等待都用 wait_until(predicate) 形式,谓词在锁内复查,
//    spurious wakeup 会被谓词挡回继续等;谓词形式同时消除丢失唤醒。
// 3) push/pop 超时语义: 以 steady_clock 计算绝对截止时刻 wait_until;
//    截止时刻到达时若恰好出现空位/数据,谓词已满足则"顺便"成功 ——
//    避免"明明有资源却因超时失败"的边界抖动;timeout=0 等价 tryXxx。
// 4) clear() 只通知 notFull(生产者): 清空后有空位;消费者仍无数据可读,
//    唤醒它们只会徒增竞争 —— 通知要"按需",不要盲目 notify_all。
// 5) 单互斥锁 + head/tail/count: push/pop 本就互斥(同一条生产-消费流水线),
//    双锁/读写锁只会更复杂;push/pop 在解锁后再 notify,减少唤醒后立刻抢锁的乒乓。
// 6) 用 count 而非 head==tail 判空/满: 空与满时 head==tail 都成立,count 消除歧义。
// 7) vector<T> + 取模下标: 容量固定、零运行时分配;T 需可默认构造(可用 unique_ptr<T[]> 替代,注释说明)。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace es {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_capacity(capacity == 0 ? 1 : capacity), // 容量 0 无意义,按 1 处理,避免取模除零
          m_buf(m_capacity) {}

    // 入队;满则等待至 timeout,超时返回 false(截止时刻恰好有空位则成功,见文件头注释)
    bool push(T item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!m_notFull.wait_until(lk, deadline, [this] { return m_count < m_capacity; })) {
            return false; // 超时且仍满
        }
        m_buf[m_tail] = std::move(item);
        m_tail = (m_tail + 1) % m_capacity;
        ++m_count;
        lk.unlock(); // 先解锁再通知: 减少消费者唤醒后的锁乒乓
        m_notEmpty.notify_one();
        return true;
    }

    // 出队;空则等待至 timeout,超时返回 false
    bool pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!m_notEmpty.wait_until(lk, deadline, [this] { return m_count > 0; })) {
            return false; // 超时且仍空
        }
        out = std::move(m_buf[m_head]);
        m_head = (m_head + 1) % m_capacity;
        --m_count;
        lk.unlock();
        m_notFull.notify_one();
        return true;
    }

    bool tryPush(T item) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_count == m_capacity) {
            return false;
        }
        m_buf[m_tail] = std::move(item);
        m_tail = (m_tail + 1) % m_capacity;
        ++m_count;
        m_notEmpty.notify_one();
        return true;
    }

    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_count == 0) {
            return false;
        }
        out = std::move(m_buf[m_head]);
        m_head = (m_head + 1) % m_capacity;
        --m_count;
        m_notFull.notify_one();
        return true;
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_count;
    }

    [[nodiscard]] size_t capacity() const {
        return m_capacity; // 固定值,无需加锁
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_count == 0;
    }

    // 清空(线程安全);只唤醒等待的生产者,见文件头注释
    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_head = m_tail = m_count = 0;
        m_notFull.notify_all();
    }

private:
    const size_t m_capacity;  // 固定容量(构造后不可变)
    std::vector<T> m_buf;     // 存储
    size_t m_head = 0;        // 队首: 下一个被 pop 的位置
    size_t m_tail = 0;        // 队尾: 下一个被 push 的位置
    size_t m_count = 0;       // 当前元素数
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty; // 消费者等待: 有数据
    std::condition_variable m_notFull;  // 生产者等待: 有空位
};

} // namespace es
