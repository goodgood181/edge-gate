// 文件路径: src/net/cmd_server.cpp
// 职责: TCP 命令服务器实现——单线程 poll() 事件循环,同时服务监听套接字与 ≤8 个
//      客户端: 按行切 JSON 命令(≤4096B)→ 回调处理 → 回一行 JSON;120s 空闲断开;
//      stop() 置标志 + 唤醒管道 + join,线程必然退出。
//
// 关键设计:
// 1) poll 事件循环的结构: 每个迭代重建 pollfd 集合(监听 fd + 唤醒管道 + 各客户端),
//    客户端 fd 全部非阻塞,事件驱动收发。任何一次 close/accept 导致集合变化后,
//    立即跳出本轮派发、重建集合,避免"fd 快照与客户端集合错位"的经典 bug。
// 2) 行协议防粘包: 每连接一个累积缓冲,收到数据先追加,再循环 find('\n') 切帧——
//    一条 recv 里多条命令(粘包)与跨 recv 的半条命令(拆包)都能正确处理。
//    单条命令上限 4096B: 超限回错误应答并清缓冲,防恶意客户端无限占内存。
//    与长度前缀帧的取舍见头文件注释。
// 3) 出站缓冲: send 非阻塞,写不完留在 txBuf、注册 POLLOUT 续写;若对端长期不读
//    导致出站缓冲 >64KB,直接断开,防内存膨胀(慢客户端防护)。
// 4) 空闲断开: 每次收到命令刷新 lastActivity,超过 120s 断开(释放 fd 与内存)。
// 5) stop(): 置标志 → 写唤醒管道(读端在 poll 集合里,立即返回)→ join。
//    poll 超时上限 1s,因此 join 必然有界;fd 全部由服务器线程自己关闭,
//    无跨线程 close 的 fd 复用竞态。管道关闭与写入共用一把锁,防 SIGPIPE。
// 6) 禁异常: 回调契约"返回单行 JSON",工程禁异常故不捕获;回调必须快速返回。

#include "cmd_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace es {
namespace {

uint64_t nowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool setFdNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool CmdServer::start(uint16_t port, CmdHandler handler, std::string* err)
{
    if (m_thread.joinable()) {
        return setErr(err, "already running");
    }
    if (!handler) {
        return setErr(err, "handler is null");
    }

    // ---- 监听套接字 ----
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return setErr(err, std::string("socket: ") + std::strerror(errno));
    }
    int on = 1;
    // SO_REUSEADDR: 快速重启时不被 TIME_WAIT 卡住端口
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // 默认仅绑定回环地址: 命令通道无鉴权,绑 0.0.0.0 会让局域网任意主机可写寄存器/注入故障(M3)。
    // 需要远程运维时改为 INADDR_ANY 并自行加鉴权(见 docs/architecture.md 安全说明)。
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        std::string e = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        return setErr(err, e);
    }
    if (::listen(fd, 16) != 0) {
        std::string e = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        return setErr(err, e);
    }
    setFdNonBlocking(fd);          // accept 由 poll 驱动,非阻塞防僵住
    m_listener = fd;
    // 回读实际端口(port=0 时由内核分配)
    struct sockaddr_in got;
    socklen_t glen = sizeof(got);
    if (::getsockname(fd, (struct sockaddr*)&got, &glen) == 0) {
        m_port = ntohs(got.sin_port);
    } else {
        m_port = port;
    }

    // ---- 唤醒管道 ----
    if (::pipe(m_wakePipe) != 0) {
        std::string e = std::string("pipe: ") + std::strerror(errno);
        ::close(fd);
        m_listener = -1;
        return setErr(err, e);
    }
    setFdNonBlocking(m_wakePipe[0]);
    setFdNonBlocking(m_wakePipe[1]);

    // ---- 启动服务器线程 ----
    m_stop = false;
    m_handler = handler;           // start 返回前无并发,线程只读该成员
    m_running = true;              // 先置位: start 返回后 running() 立即可见
    // 注: std::thread 构造在资源耗尽时抛 std::system_error;工程禁异常,概率极低,
    // 属进程级资源耗尽,按终止处理(与 mqtt_client 相同的已知边界)。
    m_thread = std::thread(&CmdServer::serverLoop, this);
    return true;
}

void CmdServer::stop()
{
    if (!m_thread.joinable()) {
        // 未启动或已停止: 清理残留 fd(无服务器线程,无竞态)
        std::lock_guard<std::mutex> lk(m_wakeMutex);
        if (m_wakePipe[0] >= 0) {
            ::close(m_wakePipe[0]);
            m_wakePipe[0] = -1;
        }
        if (m_wakePipe[1] >= 0) {
            ::close(m_wakePipe[1]);
            m_wakePipe[1] = -1;
        }
        if (m_listener >= 0) {
            ::close(m_listener);
            m_listener = -1;
        }
        m_running = false;
        return;
    }
    m_stop = true;
    wakePipe();                    // 唤醒 poll(超时上限 1s,通常立即返回)
    m_thread.join();               // 线程退出时自行关闭全部 fd
    m_running = false;
}

uint16_t CmdServer::port() const
{
    return m_port.load();
}

bool CmdServer::running() const
{
    return m_running.load();
}

size_t CmdServer::activeClients() const
{
    return m_activeClients.load();
}

// ---------------------------------------------------------------------------
// 服务器事件循环(单线程)
// ---------------------------------------------------------------------------

void CmdServer::serverLoop()
{
    while (!m_stop.load()) {
        // ---- 组装 pollfd 集合: [0]=listener [1]=wake [2..]=clients ----
        std::vector<struct pollfd> fds;
        fds.reserve(2 + m_clients.size());
        fds.push_back({m_listener, POLLIN, 0});
        fds.push_back({m_wakePipe[0], POLLIN, 0});
        for (const auto& c : m_clients) {
            short ev = POLLIN;
            if (!c.txBuf.empty()) {
                ev |= POLLOUT;     // 有积压应答才关心可写
            }
            fds.push_back({c.fd, ev, 0});
        }

        // ---- 超时: 上限 1s(保证 stop 及时);有客户端时提前到最近的空闲截止点 ----
        int64_t timeout = kPollCapMs;
        uint64_t now = nowMs();
        uint64_t idleDeadline = idleDeadlineMs();
        if (idleDeadline != 0 && idleDeadline > now) {
            timeout = std::min<int64_t>(timeout, (int64_t)(idleDeadline - now));
        }

        int rc = ::poll(fds.data(), fds.size(), (int)timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;                 // poll 致命错误(罕见)→ 退出
        }
        if (m_stop.load()) {
            break;
        }

        // ---- 唤醒管道(stop) ----
        if (fds[1].revents & POLLIN) {
            drainWakePipe();
            if (m_stop.load()) {
                break;
            }
        }

        // ---- 监听套接字: 新连接 ----
        if (fds[0].revents & POLLIN) {
            if (acceptClients() > 0) {
                continue;          // 集合变化 → 重建 fds 重新派发
            }
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;                 // 监听套接字异常 → 退出
        }

        // ---- 客户端事件(集合变化 → 跳出本轮,重建 fds) ----
        bool mutated = false;
        for (size_t i = 0; i < m_clients.size() && !mutated; ++i) {
            const struct pollfd& p = fds[i + 2];
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                closeClient(i);
                mutated = true;
                break;
            }
            if (p.revents & POLLIN) {
                if (!processClientData(i)) {
                    mutated = true;
                    break;
                }
            }
            if (i < m_clients.size() && (p.revents & POLLOUT)) {
                if (!flushClient(i)) {
                    mutated = true;
                    break;
                }
            }
            if (i < m_clients.size() && m_clients[i].closedByServer) {
                // enqueueReply 标记的超限关闭(先清缓冲再关,避免残留应答继续发送)
                closeClient(i);
                mutated = true;
                break;
            }
            if (i < m_clients.size() && m_clients[i].txBuf.size() > kMaxOutBytes) {
                std::fprintf(stderr, "[cmd] client tx buffer overflow, closing\n");
                closeClient(i);
                mutated = true;
                break;
            }
        }
        if (mutated) {
            continue;
        }

        // ---- 空闲超时检查(120s 无命令 → 断开) ----
        now = nowMs();
        for (size_t i = 0; i < m_clients.size();) {
            if (now - m_clients[i].lastActivityMs >= kIdleTimeoutMs) {
                std::fprintf(stderr, "[cmd] client idle timeout (120s), closing\n");
                closeClient(i);
                continue;          // 索引已移位,不递增
            }
            ++i;
        }
    }

    // ---- 退出清理: 全部 fd 由本线程关闭 ----
    for (auto& c : m_clients) {
        ::close(c.fd);
        --m_activeClients;
    }
    m_clients.clear();
    if (m_listener >= 0) {
        ::close(m_listener);
        m_listener = -1;
    }
    {
        std::lock_guard<std::mutex> lk(m_wakeMutex);
        if (m_wakePipe[0] >= 0) {
            ::close(m_wakePipe[0]);
            m_wakePipe[0] = -1;
        }
        if (m_wakePipe[1] >= 0) {
            ::close(m_wakePipe[1]);
            m_wakePipe[1] = -1;
        }
    }
    m_running = false;
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

// 接受新连接直到 EAGAIN(无待处理)或达到上限;超限连接直接关闭(礼貌拒绝)
int CmdServer::acceptClients()
{
    int accepted = 0;
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = ::accept(m_listener, (struct sockaddr*)&addr, &alen);
        if (fd < 0) {
            // EAGAIN=本轮已接受完;ECONNABORTED 等瞬时错误直接跳过
            break;
        }
        if ((int)m_clients.size() >= kMaxClients) {
            std::fprintf(stderr, "[cmd] client limit reached (%d), rejecting\n", kMaxClients);
            ::close(fd);
            continue;
        }
        setFdNonBlocking(fd);      // 客户端一律非阻塞: 事件驱动收发,不阻塞事件循环
        Client c;
        c.fd = fd;
        c.lastActivityMs = nowMs();
        m_clients.push_back(std::move(c));
        ++m_activeClients;
        ++accepted;
        std::fprintf(stderr, "[cmd] client accepted (%zu active)\n", m_clients.size());
    }
    return accepted;
}

void CmdServer::closeClient(size_t idx)
{
    ::close(m_clients[idx].fd);
    m_clients.erase(m_clients.begin() + (ptrdiff_t)idx);
    --m_activeClients;
}

// ---------------------------------------------------------------------------
// 收发与命令处理
// ---------------------------------------------------------------------------

// 接收当前连接全部可读字节,按 '\n' 切帧逐条处理;返回 false = 连接已关闭
bool CmdServer::processClientData(size_t idx)
{
    Client& c = m_clients[idx];
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.lastActivityMs = nowMs();
            c.rxBuf.append(buf, (size_t)n);
            // 单条命令上限: 超限回错误应答并清缓冲(防恶意客户端无限占内存)
            if (c.rxBuf.size() > kMaxCmdBytes) {
                enqueueReply(c, "{\"ok\":false,\"error\":\"command too long\"}");
                c.rxBuf.clear();
            }
            // 切帧: 粘包(一条 recv 多条命令)与拆包(半条命令跨 recv)都正确处理
            size_t pos;
            while ((pos = c.rxBuf.find('\n')) != std::string::npos) {
                std::string line = c.rxBuf.substr(0, pos);
                c.rxBuf.erase(0, pos + 1);
                handleLine(c, line);
            }
            continue;              // 可能还有可读数据,继续收
        }
        if (n == 0) {
            std::fprintf(stderr, "[cmd] client closed\n");
            closeClient(idx);
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;                 // 本批已读完
        }
        if (errno == EINTR) {
            continue;
        }
        std::fprintf(stderr, "[cmd] recv error: %s\n", std::strerror(errno));
        closeClient(idx);
        return false;
    }
    // 读完本批后: 若出站缓冲超限 → 断开(对端长期不消费)
    if (m_clients[idx].txBuf.size() > kMaxOutBytes) {
        std::fprintf(stderr, "[cmd] client tx buffer overflow, closing\n");
        closeClient(idx);
        return false;
    }
    return true;
}

// 处理一行命令: 去行尾 \r(兼容 telnet),空行忽略(可当 TCP 层 keepalive 用)
void CmdServer::handleLine(Client& c, const std::string& line)
{
    std::string cmd = line;
    if (!cmd.empty() && cmd.back() == '\r') {
        cmd.pop_back();
    }
    if (cmd.empty()) {
        return;
    }
    // 回调在服务器线程内执行,天然串行;契约要求返回单行 JSON(不含换行)——
    // 若回调返回含换行的字符串,行协议将错位,这是行协议的固有约束,注释说明
    std::string reply = m_handler ? m_handler(cmd) : "{\"ok\":false,\"error\":\"no handler\"}";
    enqueueReply(c, reply);
}

void CmdServer::enqueueReply(Client& c, const std::string& reply)
{
    // S3: 回调返回含 '\n' 会破坏行协议(下一条命令被吞),替换为空格防御
    std::string sanitized;
    if (reply.find('\n') != std::string::npos || reply.find('\r') != std::string::npos) {
        sanitized = reply;
        std::replace(sanitized.begin(), sanitized.end(), '\n', ' ');
        std::replace(sanitized.begin(), sanitized.end(), '\r', ' ');
    }
    const std::string& out = sanitized.empty() ? reply : sanitized;
    // M2: 出站缓冲同样受 kMaxOutBytes 约束——先检查再 append,防止单条超大应答撑爆内存
    if (c.txBuf.size() + out.size() + 1 > kMaxOutBytes) {
        std::fprintf(stderr, "[cmd] reply too large (%zu bytes), closing client\n", out.size());
        c.txBuf.clear();
        c.closedByServer = true;   // 事件循环下一轮关闭该连接
        return;
    }
    c.txBuf.append(out);
    if (c.txBuf.empty() || c.txBuf.back() != '\n') {
        c.txBuf.push_back('\n');   // 行协议: 每条应答以换行结尾
    }
}

// 尽力冲刷出站缓冲;对端读得慢时剩余部分留在 txBuf,等下次 POLLOUT 续写
bool CmdServer::flushClient(size_t idx)
{
    Client& c = m_clients[idx];
    while (!c.txBuf.empty()) {
        ssize_t n = ::send(c.fd, c.txBuf.data(), c.txBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.txBuf.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;                 // 对端窗口满,等 POLLOUT
        }
        std::fprintf(stderr, "[cmd] send error, closing client\n");
        closeClient(idx);
        return false;
    }
    return true;
}

uint64_t CmdServer::idleDeadlineMs() const
{
    uint64_t deadline = 0;
    for (const auto& c : m_clients) {
        uint64_t d = c.lastActivityMs + kIdleTimeoutMs;
        if (deadline == 0 || d < deadline) {
            deadline = d;
        }
    }
    return deadline;
}

void CmdServer::wakePipe()
{
    // 与"关管道"共用一把锁: 保证不会写到读端已关闭的管道(SIGPIPE 杀进程)
    std::lock_guard<std::mutex> lk(m_wakeMutex);
    if (m_wakePipe[1] >= 0) {
        char c = 's';
        (void)::write(m_wakePipe[1], &c, 1);   // EAGAIN=管道满: 无妨,事件循环本就要轮询
    }
}

void CmdServer::drainWakePipe()
{
    char buf[64];
    for (;;) {
        ssize_t n = ::read(m_wakePipe[0], buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;   // EAGAIN = 已排空
    }
}

bool CmdServer::setErr(std::string* err, const std::string& msg)
{
    if (err) {
        *err = msg;
    }
    return false;
}

} // namespace es
