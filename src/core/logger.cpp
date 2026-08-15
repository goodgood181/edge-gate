// 文件路径: src/core/logger.cpp
// 职责: logger.h 的实现 —— 详见头文件设计要点。
#include "logger.h"

#include "time_utils.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace es {

namespace {

// 级别名,定宽 5 字符保证日志列对齐: "[INFO ] [tag] msg"
const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

} // namespace

Logger& Logger::instance() {
    static Logger s_instance; // C++11 起函数局部 static 初始化线程安全(单例双检锁由编译器保证)
    return s_instance;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void Logger::init(const std::string& filePath, LogLevel level, bool console) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_filePath = filePath;
    m_console = console;
    m_fileBytes = 0;
    m_level.store(static_cast<int>(level));
    if (!filePath.empty()) {
        m_file = std::fopen(filePath.c_str(), "a"); // 追加模式
        if (!m_file) {
            // 打开失败 → 降级为仅控制台(日志是旁路,不阻塞主流程)
            m_filePath.clear();
        }
    }
}

void Logger::setLevel(LogLevel level) {
    m_level.store(static_cast<int>(level));
}

LogLevel Logger::level() const {
    return static_cast<LogLevel>(m_level.load());
}

void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
    // 快速路径: 级别不过滤直接丢弃,不碰锁(原子读,免争用)
    if (static_cast<int>(level) < m_level.load()) {
        return;
    }
    if (!fmt) {
        fmt = "";
    }
    if (!tag) {
        tag = "";
    }

    // 1) 格式化消息体: 栈缓冲优先,超长自动扩容(两遍法,第二遍需重新取 va_list)
    char stackBuf[1024];
    std::vector<char> heapBuf;
    char* buf = stackBuf;
    size_t cap = sizeof(stackBuf);
    int n = 0;
    {
        va_list args;
        va_start(args, fmt);
        n = std::vsnprintf(buf, cap, fmt, args);
        va_end(args);
        if (n < 0) {
            n = 0; // vsnprintf 返回负值(编码错误等),按空消息处理
        }
        if (static_cast<size_t>(n) >= cap) {
            heapBuf.resize(static_cast<size_t>(n) + 1);
            buf = heapBuf.data();
            va_list args2;
            va_start(args2, fmt);
            std::vsnprintf(buf, heapBuf.size(), fmt, args2);
            va_end(args2);
        }
    }

    // 2) 锁内组装整行并落盘: 保证多线程下日志行不交错、时间戳有序
    std::lock_guard<std::mutex> lk(m_mutex);

    // 时间戳复用 time_utils 的 ISO8601 生成,再转为日志格式:
    // "2026-08-14T12:00:00.123+08:00" → "2026-08-14 12:00:00.123"(T→空格,截掉时区偏移)
    std::string ts = nowIso8601();
    if (ts.size() >= 23) {
        ts[10] = ' ';
        ts.resize(23);
    }

    char head[160];
    std::snprintf(head, sizeof(head), "[%s] [%s] [%s] ", ts.c_str(), levelName(level), tag);
    const size_t headLen = std::strlen(head);

    if (m_console) {
        std::fputs(head, stdout);
        std::fwrite(buf, 1, static_cast<size_t>(n), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout); // 控制台实时可见(管道/重定向下尤其重要)
    }
    if (m_file) {
        std::fwrite(head, 1, headLen, m_file);
        std::fwrite(buf, 1, static_cast<size_t>(n), m_file);
        std::fwrite("\n", 1, 1, m_file);
        std::fflush(m_file); // 逐行 flush: 进程崩溃/断电不丢日志,代价是少量系统调用(日志量小,可接受)
        m_fileBytes += headLen + static_cast<size_t>(n) + 1;
        if (m_fileBytes >= kMaxFileBytes) {
            rotateLocked();
        }
    }
}

void Logger::rotateLocked() {
    if (!m_file) {
        return;
    }
    std::fclose(m_file);
    m_file = nullptr;
    const std::string backup = m_filePath + ".1";
    std::rename(m_filePath.c_str(), backup.c_str()); // POSIX 下覆盖旧 *.1;失败仅丢一次轮转,不致命
    m_file = std::fopen(m_filePath.c_str(), "a");
    m_fileBytes = 0;
}

} // namespace es
