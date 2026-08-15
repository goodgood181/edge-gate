// 文件路径: src/core/thread_utils.cpp
// 职责: thread_utils.h 的实现,设计要点见头文件注释。
#include "thread_utils.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace es {

namespace {

void setErr(std::string* err, const std::string& msg) {
    if (err) {
        *err = msg;
    }
}

} // namespace

// ---------- Thread ----------

Thread::~Thread() {
    if (m_thread.joinable()) {
        m_thread.join(); // RAII: 保证线程资源回收;协作退出见头文件注释
    }
}

Thread::Thread(Thread&& o) noexcept
    : m_thread(std::move(o.m_thread)) {}

Thread& Thread::operator=(Thread&& o) noexcept {
    if (this != &o) {
        if (m_thread.joinable()) {
            m_thread.join(); // 先回收当前线程,再接管(与析构语义一致)
        }
        m_thread = std::move(o.m_thread);
    }
    return *this;
}

void Thread::join() {
    if (m_thread.joinable()) {
        m_thread.join(); // 非 joinable 时空操作: 避免 std::thread 抛 std::system_error(禁异常)
    }
}

bool Thread::joinable() const {
    return m_thread.joinable();
}

void Thread::detach() {
    if (m_thread.joinable()) {
        m_thread.detach(); // 同上: 吞掉非法状态异常
    }
}

// ---------- PeriodicTask ----------

PeriodicTask::PeriodicTask(std::chrono::milliseconds period, std::function<void()> task)
    : m_period(period),
      m_task(std::move(task)) {}

PeriodicTask::~PeriodicTask() {
    stop(); // 防悬垂: 销毁即停止后台线程
}

bool PeriodicTask::start(std::string* err) {
    if (m_period.count() <= 0) {
        setErr(err, "PeriodicTask: 周期必须大于 0");
        return false;
    }
    if (m_running.load()) {
        setErr(err, "PeriodicTask: 已在运行");
        return false;
    }
    // 回收上一个线程时先置停止标志并通知,再 join ——
    // 否则若旧线程仍存活(如重复 start 的竞态窗口),join 会等一个永远不会退出
    // 的线程(它只在收到停止信号后退出),造成死锁。
    m_stop.store(true);
    if (m_thread.joinable()) {
        m_cv.notify_all();
        m_thread.join();
    }
    m_stop.store(false);
    m_thread = std::thread([this] { runLoop(); });
    return true;
}

void PeriodicTask::stop() {
    m_stop.store(true);
    m_cv.notify_all(); // 无锁通知即可: predicate 形式不会丢失唤醒
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // 注意: 禁止在任务回调内调用 stop()/start()(会 join 自己 → 死锁),见头文件注释
}

bool PeriodicTask::running() const {
    return m_running.load();
}

void PeriodicTask::runLoop() {
    m_running.store(true);
    while (true) {
        std::unique_lock<std::mutex> lk(m_mutex);
        // predicate 形式: 自动防虚假唤醒;stop 标志置位时立即退出
        const bool stopRequested = m_cv.wait_for(lk, m_period, [this] { return m_stop.load(); });
        if (stopRequested) {
            break;
        }
        lk.unlock();
        m_task(); // 任务在锁外执行(任务内部可自由调用本对象只读接口)
        // 超周期不补偿: 任务耗时超过周期时,下一次调度从任务返回后重新计时,
        // 而不是追拍补发多次 —— 避免积压与无意义的忙循环
    }
    m_running.store(false);
}

} // namespace es
