// 文件路径: tests/fake_broker.h
// 意图: 用 es::mqtt::decodePacket 自举的迷你 MQTT broker —— 测试 MqttClient 用,
//       零外部依赖(仅 std + POSIX socket)。功能覆盖测试所需的最小集合:
//       CONNACK/SUBACK/UNSUBACK/PUBACK/PINGRESP、记录收到的发布、主动向客户端
//       推送 PUBLISH、强制断开全部客户端(触发重连路径)。
//
// 设计要点(面试可讲):
// 1) 自举: broker 与 client 共用同一套报文编解码(es::mqtt::decodePacket),
//    双方字节级互通 —— 协议正确性在"测试即对端"中闭环,不依赖 mosquitto;
// 2) 线程模型: 单 reader 线程 poll 多路复用(listener + 控制管道 + 客户端),
//    所有 fd 的创建/关闭都收敛在 reader 线程 —— 跨线程 close fd 有"fd 号被
//    复用"竞态,因此 dropAllClients/stop 只往控制管道写命令字节,不碰 fd;
// 3) 半包处理: 每连接累积 rxBuf,decodePacket 的"false + err 空 = 还需更多字节"
//    约定天然支持跨 recv 边界的粘包/拆包;
// 4) 发送串行化: reader 线程的应答与测试线程的 pushMessage 都可能写同一连接,
//    用一把发送锁串行化(量级极小,锁开销可忽略)。
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/net/mqtt_packets.h"

namespace estest {

class FakeBroker
{
public:
    // 收到的一条客户端发布(测试侧断言用)
    struct RecvMsg
    {
        std::string topic;
        std::string payload;
        uint8_t qos = 0;
        bool retain = false;
    };

    FakeBroker() = default;
    ~FakeBroker() { stop(); }

    FakeBroker(const FakeBroker&) = delete;
    FakeBroker& operator=(const FakeBroker&) = delete;

    // 监听 127.0.0.1:0(内核分配临时端口),port() 回读实际端口
    bool start(std::string* err)
    {
        if (m_listener >= 0)
        {
            if (err) *err = "broker 已启动";
            return false;
        }
        m_listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listener < 0)
        {
            if (err) *err = "socket 失败";
            return false;
        }
        int one = 1;
        ::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // 临时端口
        if (::bind(m_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            if (err) *err = "bind 失败";
            ::close(m_listener);
            m_listener = -1;
            return false;
        }
        if (::listen(m_listener, 8) != 0)
        {
            if (err) *err = "listen 失败";
            ::close(m_listener);
            m_listener = -1;
            return false;
        }
        // 回读实际端口
        socklen_t alen = sizeof(addr);
        if (::getsockname(m_listener, reinterpret_cast<sockaddr*>(&addr), &alen) == 0)
        {
            m_port = ntohs(addr.sin_port);
        }
        // 控制管道: reader 线程唯一的外部刺激通道(drop/stop);读端非阻塞,
        // 避免"读完 1 字节后再次 read 阻塞"卡死整个线程
        if (::pipe(m_ctlPipe) != 0)
        {
            if (err) *err = "pipe 失败";
            ::close(m_listener);
            m_listener = -1;
            return false;
        }
        setNonBlock(m_ctlPipe[0]);
        setNonBlock(m_listener); // 非阻塞 accept: 无待处理连接时立即 EAGAIN,不卡线程
        m_stop = false;
        m_thread = std::thread(&FakeBroker::loop, this);
        return true;
    }

    void stop()
    {
        if (!m_thread.joinable())
        {
            return;
        }
        m_stop = true;
        ctlNotify(2); // 命令: 退出
        m_thread.join();
        if (m_listener >= 0)
        {
            ::close(m_listener);
            m_listener = -1;
        }
        if (m_ctlPipe[0] >= 0)
        {
            ::close(m_ctlPipe[0]);
            ::close(m_ctlPipe[1]);
            m_ctlPipe[0] = m_ctlPipe[1] = -1;
        }
        m_clients.clear();
    }

    uint16_t port() const { return m_port; }
    size_t clientCount() const
    {
        std::lock_guard<std::mutex> lk(m_clientsMutex);
        return m_clients.size();
    }

    // ---- 统计(测试断言) ----
    uint64_t connectCount() const { return m_connectCount.load(); }
    uint64_t pingReqCount() const { return m_pingReqCount.load(); }
    uint64_t subscribeCount() const { return m_subscribeCount.load(); }
    uint64_t unsubscribeCount() const { return m_unsubscribeCount.load(); }
    uint64_t pubackRecvCount() const { return m_pubackRecv.load(); }
    size_t receivedCount() const
    {
        std::lock_guard<std::mutex> lk(m_recvMutex);
        return m_recv.size();
    }
    bool getReceived(size_t i, RecvMsg* out) const
    {
        std::lock_guard<std::mutex> lk(m_recvMutex);
        if (i >= m_recv.size())
        {
            return false;
        }
        *out = m_recv[i];
        return true;
    }

    // 主动向全部在线客户端推送 PUBLISH(测试消息回调);QoS1 会带包 ID
    bool pushMessage(const std::string& topic, const std::string& payload, uint8_t qos)
    {
        const uint16_t pid = ++m_pushPacketId;
        const std::vector<uint8_t> bytes =
            es::mqtt::encodePublish(topic, payload, qos, false, qos > 0 ? pid : 0);
        std::lock_guard<std::mutex> lk(m_sendMutex);
        std::lock_guard<std::mutex> ck(m_clientsMutex);
        bool any = false;
        for (const Client& c : m_clients)
        {
            if (c.fd >= 0 && sendAll(c.fd, bytes))
            {
                any = true;
            }
        }
        return any;
    }

    // 强制断开全部客户端(触发客户端的断线重连路径)
    void dropAllClients() { ctlNotify(1); }

private:
    struct Client
    {
        int fd = -1;
        std::string rxBuf; // 累积缓冲(跨 recv 粘包拆包)
    };

    static void setNonBlock(int fd)
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // 全量发送(非阻塞 + 有限重试;串行化由调用方持 m_sendMutex 保证)
    bool sendAll(int fd, const std::vector<uint8_t>& data)
    {
        size_t sent = 0;
        for (int attempt = 0; attempt < 50 && sent < data.size(); ++attempt)
        {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n > 0)
            {
                sent += static_cast<size_t>(n);
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                struct pollfd pfd{fd, POLLOUT, 0};
                ::poll(&pfd, 1, 5);
            }
            else
            {
                return false; // 连接已断开
            }
        }
        return sent == data.size();
    }

    void ctlNotify(uint8_t cmd)
    {
        std::lock_guard<std::mutex> lk(m_ctlMutex); // 与 reader 关管道互斥
        if (m_ctlPipe[1] >= 0)
        {
            (void)::write(m_ctlPipe[1], &cmd, 1);
        }
    }

    void loop()
    {
        std::vector<pollfd> pfds;
        std::vector<Client> clients;
        bool running = true;
        while (running)
        {
            // 重建 poll 集合: listener + 控制管道 + 客户端
            pfds.clear();
            pollfd p0{m_listener, POLLIN, 0};
            pollfd p1{m_ctlPipe[0], POLLIN, 0};
            pfds.push_back(p0);
            pfds.push_back(p1);
            for (const Client& c : clients)
            {
                pollfd pc{c.fd, POLLIN, 0};
                pfds.push_back(pc);
            }
            const int rc = ::poll(pfds.data(), pfds.size(), 200);
            if (rc < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            // 控制命令
            if (pfds[1].revents & POLLIN)
            {
                char cmd = 0;
                while (::read(m_ctlPipe[0], &cmd, 1) > 0)
                {
                    if (cmd == 1)
                    {
                        for (Client& c : clients)
                        {
                            if (c.fd >= 0)
                            {
                                ::close(c.fd);
                                c.fd = -1;
                            }
                        }
                    }
                    else if (cmd == 2)
                    {
                        running = false;
                    }
                }
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                                             [](const Client& c) { return c.fd < 0; }),
                              clients.end());
            }
            if (!running)
            {
                break;
            }
            // 新连接
            if (pfds[0].revents & POLLIN)
            {
                while (true)
                {
                    const int fd = ::accept(m_listener, nullptr, nullptr);
                    if (fd < 0)
                    {
                        break; // EAGAIN 或关闭
                    }
                    setNonBlock(fd);
                    {
                        std::lock_guard<std::mutex> lk(m_clientsMutex);
                        m_clients.push_back(Client{fd, {}});
                    }
                    clients.push_back(Client{fd, {}});
                }
            }
            // 客户端数据
            for (size_t i = 0; i < clients.size(); ++i)
            {
                if (clients[i].fd < 0)
                {
                    continue;
                }
                const short ev = pfds[2 + i].revents;
                if (ev & (POLLERR | POLLHUP | POLLNVAL))
                {
                    ::close(clients[i].fd);
                    clients[i].fd = -1;
                    continue;
                }
                if (ev & POLLIN)
                {
                    uint8_t tmp[512];
                    const ssize_t n = ::recv(clients[i].fd, tmp, sizeof(tmp), MSG_DONTWAIT);
                    if (n > 0)
                    {
                        clients[i].rxBuf.append(reinterpret_cast<const char*>(tmp),
                                                static_cast<size_t>(n));
                        processBuffer(clients[i]);
                    }
                    else if (n == 0)
                    {
                        ::close(clients[i].fd);
                        clients[i].fd = -1;
                    }
                }
            }
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [](const Client& c) { return c.fd < 0; }),
                          clients.end());
            // 同步到对外可见的连接表(测试线程 clientCount)
            std::lock_guard<std::mutex> lk(m_clientsMutex);
            m_clients = clients;
        }
        // 线程退出前清理全部 fd
        for (const Client& c : clients)
        {
            if (c.fd >= 0)
            {
                ::close(c.fd);
            }
        }
        std::lock_guard<std::mutex> lk(m_clientsMutex);
        m_clients.clear();
    }

    // 粘包拆包: 循环 decodePacket;数据不足(err 空)保留缓冲等下一段
    void processBuffer(Client& c)
    {
        size_t pos = 0;
        while (pos < c.rxBuf.size())
        {
            es::mqtt::Packet pkt;
            std::string err;
            size_t consumed = 0;
            if (!es::mqtt::decodePacket(
                    reinterpret_cast<const uint8_t*>(c.rxBuf.data()) + pos,
                    c.rxBuf.size() - pos, &consumed, &pkt, &err))
            {
                if (err.empty())
                {
                    break; // 半包: 等更多字节
                }
                // 协议错误: 断开该客户端
                ::close(c.fd);
                c.fd = -1;
                c.rxBuf.clear();
                return;
            }
            pos += consumed;
            handlePacket(c, pkt);
        }
        c.rxBuf.erase(0, pos);
    }

    void handlePacket(Client& c, const es::mqtt::Packet& pkt)
    {
        std::lock_guard<std::mutex> lk(m_sendMutex); // 与 pushMessage 串行化
        switch (pkt.type)
        {
            case es::mqtt::PacketType::Connect:
                m_connectCount.fetch_add(1);
                // CONNACK: 接受,无会话保留
                sendAll(c.fd, {0x20, 0x02, 0x00, 0x00});
                break;
            case es::mqtt::PacketType::Subscribe:
                m_subscribeCount.fetch_add(1);
                {
                    // SUBACK: 授权 QoS = min(请求, 1)
                    std::vector<uint8_t> suback = {0x90,
                                                   static_cast<uint8_t>(2 + pkt.payload.size()),
                                                   static_cast<uint8_t>(pkt.packetId >> 8),
                                                   static_cast<uint8_t>(pkt.packetId & 0xFF)};
                    for (char q : pkt.payload)
                    {
                        suback.push_back(static_cast<uint8_t>(q > 1 ? 1 : q));
                    }
                    sendAll(c.fd, suback);
                }
                break;
            case es::mqtt::PacketType::Unsubscribe:
                m_unsubscribeCount.fetch_add(1);
                sendAll(c.fd, {0xB0, 0x02, static_cast<uint8_t>(pkt.packetId >> 8),
                               static_cast<uint8_t>(pkt.packetId & 0xFF)});
                break;
            case es::mqtt::PacketType::Publish:
            {
                RecvMsg m{pkt.topic, pkt.payload, pkt.qos, pkt.retain};
                {
                    std::lock_guard<std::mutex> rk(m_recvMutex);
                    m_recv.push_back(std::move(m));
                }
                if (pkt.qos == 1)
                {
                    sendAll(c.fd, {0x40, 0x02, static_cast<uint8_t>(pkt.packetId >> 8),
                                   static_cast<uint8_t>(pkt.packetId & 0xFF)});
                }
                break;
            }
            case es::mqtt::PacketType::Puback:
                m_pubackRecv.fetch_add(1); // 客户端确认了我们推送的 QoS1 消息
                break;
            case es::mqtt::PacketType::Pingreq:
                m_pingReqCount.fetch_add(1);
                sendAll(c.fd, {0xD0, 0x00});
                break;
            case es::mqtt::PacketType::Disconnect:
                ::close(c.fd);
                c.fd = -1;
                break;
            default:
                break; // 其余报文忽略
        }
    }

    int m_listener = -1;
    int m_ctlPipe[2] = {-1, -1};
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::mutex m_ctlMutex;   // 保护写控制管道 vs reader 关管道
    std::mutex m_sendMutex;  // 串行化所有 send(reader 应答 + 测试线程推送)
    mutable std::mutex m_clientsMutex;
    std::vector<Client> m_clients; // 对外可见快照(reader 同步)
    mutable std::mutex m_recvMutex;
    std::vector<RecvMsg> m_recv;

    std::atomic<uint64_t> m_connectCount{0};
    std::atomic<uint64_t> m_pingReqCount{0};
    std::atomic<uint64_t> m_subscribeCount{0};
    std::atomic<uint64_t> m_unsubscribeCount{0};
    std::atomic<uint64_t> m_pubackRecv{0};
    std::atomic<uint16_t> m_pushPacketId{0};
};

} // namespace estest
