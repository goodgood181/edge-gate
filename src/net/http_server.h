// 文件路径: src/net/http_server.h
// 职责: 极简 HTTP/1.1 服务器 —— 为网关提供内置 Web 监控页。
//       浏览器(Windows 侧)直接访问 http://localhost:18080 查看实时数据,
//       绕开 WSLg 图形链路,任何环境都能打开。
// 设计要点:
// 1) 为什么自写 HTTP 服务器而不是引入库: 仅需要 GET 两个路由
//    (/ 与 /api/snapshot),手写 ~30 行解析,零依赖、可裁剪;
// 2) 单线程 poll 多路复用(与 cmd_server 同思路): 低频短报文场景,
//    每连接一线程是浪费;≤4 并发客户端,非阻塞 fd + poll;
// 3) 每请求关闭连接(Connection: close): 省略 keep-alive 状态机,
//    监控页 1s 轮询一次,连接开销可忽略;实现最简单、最不易错;
// 4) 安全: 默认只绑定 127.0.0.1(同 cmd_server 的考虑),页面只读
//    (无写接口),不暴露控制面。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace es {

// 快照提供者: 返回点表 JSON(网关侧 snapshotJson(),每 1s 轮询调用)
using SnapshotProvider = std::function<std::string()>;

class HttpServer {
public:
    // 启动监听;provider 为空返回 false。port=0 由内核分配,可用 port() 回读。
    bool start(uint16_t port, SnapshotProvider provider, std::string* err);
    // 线程安全、幂等;stop() 后不可复用实例。
    void stop();

    [[nodiscard]] uint16_t port() const;
    [[nodiscard]] bool running() const;

private:
    static constexpr int kMaxClients = 4;            // 并发客户端上限
    static constexpr size_t kMaxRequest = 8192;      // 请求头上限
    static constexpr uint64_t kIdleTimeoutMs = 30000; // 30s 空闲断开
    static constexpr int kPollCapMs = 1000;          // poll 最长阻塞(退出有界)

    struct Client {
        int fd = -1;
        std::string rxBuf;
        uint64_t lastActivityMs = 0;
    };

    void serverLoop();
    void handleClient(size_t idx);        // 解析请求行 → 路由 → 写响应
    void closeClient(size_t idx);
    static bool sendAll(int fd, const std::string& data);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    SnapshotProvider m_provider;
    int m_listener = -1;
    int m_wakePipe[2] = {-1, -1};
    uint16_t m_port = 0;
    std::vector<Client> m_clients;
    std::atomic<int> m_activeClients{0};
};

} // namespace es
