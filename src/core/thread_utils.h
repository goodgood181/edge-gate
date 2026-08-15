// 文件路径: src/core/thread_utils.h
// 职责: 线程生命周期管理工具 —— Thread(RAII 包装 std::thread,析构自动 join)与
//       PeriodicTask(周期任务,支持优雅停止、超周期跳过补偿)。
//
// 设计要点:
// 1) 析构 join 而非 detach: detach 后线程仍持有 this/捕获对象,对象销毁即悬垂访问;
//    join 保证线程资源(栈/TLS)一定回收。代价是线程若卡死会阻塞析构 ——
//    因此本工程所有长任务线程都以"停止标志 + 条件变量"协作退出,绝不粗暴 terminate。
// 2) join()/detach() 对非 joinable 状态做空操作: std::thread 在这些场景会抛
//    std::system_error,工程禁异常,包装层直接吞掉(注释说明取舍)。
// 3) PeriodicTask 用 wait_for(predicate) 等待: predicate 形式自动处理条件变量
//    虚假唤醒与丢失唤醒;停止标志用 atomic(免锁读),stop() 无需持锁即可通知。
// 4) "超周期跳过补偿": 任务耗时 > 周期时,下一次从任务返回后重新计时,不追拍补发 ——
//    避免任务积压与 CPU 空转;适合遥测这类"丢一拍可接受,追成风暴不可接受"的场景。
// 5) 禁止在任务回调内调用本任务的 stop()/start()(会 join 自己 → 死锁),注释明示。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace es {

// RAII 线程: 构造即启动;析构自动 join;仅可移动,不可拷贝
class Thread {
public:
    template <typename Fn>
    explicit Thread(Fn&& fn)
        : m_thread(std::forward<Fn>(fn)) {}
    // 注意: 线程创建失败时 std::thread 会抛 std::system_error(资源耗尽),
    // 禁异常工程中视为致命错误(进程终止),属可接受取舍。

    ~Thread(); // 若 joinable 则 join
    Thread(Thread&&) noexcept;
    Thread& operator=(Thread&&) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void join();
    [[nodiscard]] bool joinable() const;
    void detach();

private:
    std::thread m_thread;
};

// 周期任务: 每 period 执行一次 task;单次超周期则跳过补偿(不追拍)
class PeriodicTask {
public:
    explicit PeriodicTask(std::chrono::milliseconds period, std::function<void()> task);
    ~PeriodicTask(); // 自动 stop()

    bool start(std::string* err); // 启动后台线程;已在运行时报错(旧线程回收前先发停止信号,防死锁)
    void stop();                  // 置停止标志 + 通知 + join
    [[nodiscard]] bool running() const;

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

private:
    void runLoop();

    const std::chrono::milliseconds m_period; // 周期(校验 > 0)
    std::function<void()> m_task;             // 任务(锁外执行)
    std::thread m_thread;
    std::mutex m_mutex;                 // 配合 m_cv 等待周期/停止
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{true};     // 停止标志(atomic,免锁读)
    std::atomic<bool> m_running{false}; // 是否在运行
};

} // namespace es
