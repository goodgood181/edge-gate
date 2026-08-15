// 文件路径: src/core/logger.h
// 职责: 全局日志单例,支持级别过滤、时间戳、控制台 + 文件双输出、>1MB 自动轮转一次(*.1)。
// 使用方式: ES_LOGI("TAG", "value=%d", v) / ES_LOGW(...) 等宏,或 Logger::instance().log(...)。
//
// 设计要点:
// 1) 线程安全: 所有共享状态(文件句柄、字节计数、开关)由一把互斥锁保护;
//    每次 log() 在锁内完成"格式化 + 写文件 + 写控制台",保证整条日志行原子落盘、多线程不交错。
// 2) varargs 日志格式化的线程安全: vsnprintf 使用调用方栈上的 va_list(每线程独立),
//    不共享全局格式化缓冲区,格式化本身天然线程安全;用"栈缓冲优先 + 超长扩容"两遍法,
//    避免固定缓冲截断长日志(第一遍消费 va_list,第二遍需 va_copy 重取)。
// 3) 轮转: 边写边累计字节数(避免每次 stat),超过 1MB 时 rename 为 *.1 后重开文件;
//    轮转也在锁内完成,避免两个线程同时触发轮转导致文件句柄错乱。
// 4) 为什么不用 iostream: 全局同步锁开销大、难以保证"整行原子写",
//    core 模块统一 stdio(FILE*),嵌入式风格更可控。
// 5) 日志失败不致命: 文件打不开自动降级为仅控制台(接口无错误通道,日志是旁路,不阻塞主流程)。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>

namespace es {

// 日志级别: 数值越大越严重;log() 只输出 >= 当前级别 的日志
enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& instance();                                   // 进程内单例(函数局部 static,懒初始化且线程安全)

    void init(const std::string& filePath, LogLevel level, bool console); // filePath 为空 → 仅控制台
    void setLevel(LogLevel level);
    [[nodiscard]] LogLevel level() const;

    // varargs 日志入口: fmt 语法与 printf 一致;线程安全,见文件头注释
    void log(LogLevel level, const char* tag, const char* fmt, ...);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static constexpr size_t kMaxFileBytes = 1024 * 1024;         // 单文件上限 1MB,超出轮转为 *.1

    std::mutex m_mutex;                                          // 保护文件句柄/字节计数/开关
    std::string m_filePath;                                      // 当前日志文件路径(空 = 仅控制台)
    FILE* m_file = nullptr;                                      // 日志文件句柄(stdio,禁 iostream)
    bool m_console = true;                                       // 是否同时输出到 stdout
    std::atomic<int> m_level{static_cast<int>(LogLevel::Info)};  // 级别过滤(原子,快速路径免锁读)
    size_t m_fileBytes = 0;                                      // 当前文件已写字节数(用于轮转判断)

    void rotateLocked();                                         // 锁内调用: 关旧文件 → rename *.1 → 重开
};

// 使用例: ES_LOGI("cfg", "baud=%d", 9600);ES_LOGE("modbus", "crc error");
#define ES_LOG(level, tag, ...) ::es::Logger::instance().log(level, tag, __VA_ARGS__)
#define ES_LOGI(tag, ...) ES_LOG(::es::LogLevel::Info, tag, __VA_ARGS__)
#define ES_LOGD(tag, ...) ES_LOG(::es::LogLevel::Debug, tag, __VA_ARGS__)
#define ES_LOGW(tag, ...) ES_LOG(::es::LogLevel::Warn, tag, __VA_ARGS__)
#define ES_LOGE(tag, ...) ES_LOG(::es::LogLevel::Error, tag, __VA_ARGS__)

} // namespace es
