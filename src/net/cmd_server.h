// 文件路径: src/net/cmd_server.h
// 职责: 网关 TCP 命令服务器(JSON over newline),用于运维/调试: 收命令、回 JSON 应答。
//      单线程 poll() 多路复用,支持 ≤8 个并发客户端。
//
// 设计要点:
// 1) 为什么 poll() 多路复用而不是"每连接一线程": 命令通道是低频短报文,每连接一线程
//    会引入线程创建/上下文切换开销与共享状态的锁;单线程 + 非阻塞 fd + poll 在连接数
//    小(≤8)时更简单、确定、省资源,且天然串行化所有客户端,无需锁。
//    代价是单个慢客户端会拖慢其他客户端(本项目命令处理均为 O(1) 快操作,可接受)。
// 2) 为什么"新行分隔"而不是"长度前缀帧": 命令 ≤4096B 且约定为单行 JSON,
//    行协议实现简单、可用 nc/telnet 直接调试;长度前缀帧(4 字节大端长度+载荷)解析更
//    严谨、无"命令内换行"歧义,但需额外编帧/解帧代码。本项目 JSON 命令不含裸换行
//    (文档约定),行协议 + 4096B 上限足够。防粘包: 接收侧维护每连接累积缓冲,
//    按 '\n' 切帧,跨 recv 边界自动重组(一条 recv 多条命令 / 半条命令都能正确处理)。
// 3) 线程安全退出: stop() 置标志 + 写唤醒管道 + join;poll 超时上限 1s,退出有界。
// 4) 禁异常: 所有错误以 bool + std::string* err 表达。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace es {

// 命令处理回调: 入参为一行 JSON 命令(不含换行),返回值必须是单行 JSON 应答(不含换行)。
// 在服务器线程内执行: 禁止阻塞、禁止抛异常(工程禁异常);应答超长(>64KB 出站缓冲)
// 会被断开。
using CmdHandler = std::function<std::string(const std::string& jsonCmd)>;

class CmdServer {
public:
    // 启动监听;handler 为空返回 false。port=0 时由内核分配,可用 port() 回读。
    bool start(uint16_t port, CmdHandler handler, std::string* err);
    // 线程安全、幂等;stop() 后不可复用同一实例(如需重启请新建)。
    void stop();

    [[nodiscard]] uint16_t port() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] size_t activeClients() const;

private:
    static constexpr int kMaxClients = 8;         // 并发客户端上限
    static constexpr size_t kMaxCmdBytes = 4096;  // 单条命令上限(超出 → 错误应答并清缓冲)
    static constexpr size_t kMaxOutBytes = 65536; // 单连接出站缓冲上限(对端不读 → 断开)
    static constexpr uint64_t kIdleTimeoutMs = 120000;   // 120s 无命令 → 断开
    static constexpr int kPollCapMs = 1000;              // poll 最长阻塞(退出有界)

    // 一个客户端连接的全部状态;仅服务器线程访问
    struct Client {
        int fd = -1;
        std::string rxBuf;    // 累积接收缓冲(按行切帧,防粘包)
        std::string txBuf;    // 待发送缓冲(对端读得慢时积压,POLLOUT 时冲刷)
        uint64_t lastActivityMs = 0;
        bool closedByServer = false;  // 服务器主动关闭标记(如应答超长,事件循环下一轮处理)
    };

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<uint16_t> m_port{0};
    std::atomic<size_t> m_activeClients{0};
    CmdHandler m_handler;              // start 时设置,服务器线程只读

    int m_listener = -1;               // 监听 fd(服务器线程独占)
    int m_wakePipe[2] = {-1, -1};      // 唤醒管道: stop() → 服务器线程
    std::mutex m_wakeMutex;            // 串行化"写管道 vs 关管道",防 SIGPIPE
    std::vector<Client> m_clients;     // 客户端集合(服务器线程独占)

    void serverLoop();
    int acceptClients();               // 返回本次接受的连接数(0=无)
    bool processClientData(size_t idx);    // true=连接仍存活;false=已关闭
    bool flushClient(size_t idx);          // true=连接仍存活;false=已关闭
    void closeClient(size_t idx);
    void handleLine(Client& c, const std::string& line);
    void enqueueReply(Client& c, const std::string& reply);
    uint64_t idleDeadlineMs() const;   // 全部客户端中最近的空闲截止时刻;无客户端 → 0
    void wakePipe();
    void drainWakePipe();
    static bool setErr(std::string* err, const std::string& msg);
};

} // namespace es
