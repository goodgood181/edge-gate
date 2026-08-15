// 文件路径: src/net/mqtt_client.cpp
// 职责: MQTT 3.1.1 客户端实现(会话状态机 + 网络线程),字节级编解码委托 es::mqtt。
//      覆盖: CONNECT/CONNACK 校验、读循环拆包、keepalive PINGREQ、
//      QoS0 回调、QoS1 PUBACK 确认与 10s 未确认重发一次、指数退避重连(1s→30s+抖动)、
//      stop() 优雅退出。禁异常: 错误一律 bool + std::string* err。
//
// 关键设计:
// 1) keepalive 语义: MQTT 3.1.1 §3.1.2.10 要求客户端在 keepalive 间隔内至少发送一个
//    控制报文,否则 broker 可在 1.5×keepalive 无任何报文时断开连接。若客户端卡在
//    "恰好 keepalive" 时刻发包,一次调度抖动就可能越过 broker 的 1.5× 窗口被踢,
//    因此本实现按 0.7×keepalive 的空闲阈值提前发 PINGREQ,给调度与网络留出余量;
//    且任意出站报文(不限于 PINGREQ)都会刷新空闲计时。keepaliveSec=0 表示禁用。
// 2) clean session 语义: true → broker 在会话结束即丢弃订阅与在途 QoS1/2 状态,
//    因此每次(重)连接成功后必须重新 SUBSCRIBE(见 resubscribeAll);
//    false → 会话延续,订阅由 broker 保存,重连后不重订阅(代价: broker 若重启丢失
//    会话,客户端无从察觉,只能由业务层兜底;本实现如实注释保留该权衡)。
// 3) QoS1 确认流程: PUBLISH(qos=1, packetId∈[1,65535]) → broker 回 PUBACK(同 packetId)。
//    客户端登记在途表,10s 未确认则置 DUP=1 重发一次(至少一次投递语义,接收方需幂等),
//    再等 10s 仍未确认则丢弃并告警——避免在途表无限膨胀。
// 4) 收 PUBLISH: QoS0 直接回调;QoS1 先回调后回 PUBACK(先处理再确认,配合重发保证
//    "至少一次"不丢消息);QoS2 收到则告警忽略(本项目客户端不实现 QoS2 收发)。
// 5) 回调在网络线程内执行: 好处是消息顺序天然保序、无需派发队列与锁;代价是回调
//    若阻塞会连带拖垮 keepalive/重发/收包,因此回调必须快速返回,需要慢操作时
//    业务方应自行拷贝数据到自己的队列(头文件注释同样强调)。
// 6) 重连退避: 失败后等待"上一延迟×2"(1s→2s→4s→…→30s 封顶)再叠加 0~30% 随机抖动,
//    抖动用于破坏多设备重连的同步性,避免"重连风暴"打爆 broker;CONNACK 成功即清零。
// 7) stop() 退出路径: 置标志 → 写唤醒管道 → join。所有阻塞点(poll、connect 等待、
//    退避睡眠)都同时监听唤醒管道,join 必然有界;另配 SO_RCVTIMEO/SO_SNDTIMEO 兜底,
//    保证即便 poll 逻辑有疏漏,recv/send 也绝不无限阻塞。刻意不在 stop() 里跨线程
//    close/shutdown socket: 若网络线程正 poll 该 fd 时被别的线程 close,fd 号可能被
//    复用,网络线程会 poll 到无关对象——关闭一律收敛到拥有 socket 的网络线程自己。
//    退出前若在线,由网络线程尽力发送 DISCONNECT(此时独占 socket,无并发写)。
// 8) 唤醒管道写端设为非阻塞且与"关管道"共用一把锁: 杜绝用户线程在管道读端已关闭
//    时写入导致 SIGPIPE 杀进程;send 一律 MSG_NOSIGNAL,全程不碰进程级信号掩码。

#include "mqtt_client.h"

#include "mqtt_packets.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace es {
namespace {

// 收包缓冲上限: 超过视为协议异常,防恶意/损坏数据撑爆内存
constexpr size_t kRxBufMax = 1024 * 1024;
// TCP 连接超时(非阻塞 connect + poll 等待)
constexpr uint64_t kConnectTimeoutMs = 10000;
// 等待 CONNACK 超时
constexpr uint64_t kConnackTimeoutMs = 10000;
// QoS1 发布等待 PUBACK 超时(10s)
constexpr uint64_t kPubackTimeoutMs = 10000;
// SO_RCVTIMEO/SO_SNDTIMEO 兜底(主驱动仍是 poll;这是第二道保险)
constexpr uint64_t kSockTimeoutMs = 2000;
// poll 单次最长阻塞: 所有截止点(keepalive/重发/停止)的响应误差 ≤1s
constexpr int kPollCapMs = 1000;

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

MqttClient::MqttClient(const MqttConfig& cfg)
    : m_cfg(cfg)
{
    if (m_cfg.clientId.empty() && !m_cfg.cleanSession) {
        // MQTT 3.1.1 §3.1.3.1: cleanSession=0 时 broker 必须以 clientId 标识会话,
        // 空 clientId 会被 CONNACK returnCode=2 拒绝,这里提前告警
        std::fprintf(stderr, "[mqtt] warning: empty clientId with cleanSession=false, broker will reject\n");
    }
}

MqttClient::~MqttClient()
{
    stop();
}

bool MqttClient::start(std::string* err)
{
    if (m_thread.joinable()) {
        return setErr(err, "already started");
    }
    if (pipe(m_wakePipe) != 0) {
        return setErr(err, std::string("pipe: ") + std::strerror(errno));
    }
    setFdNonBlocking(m_wakePipe[0]);   // 读端非阻塞: 排空唤醒字节时不阻塞
    setFdNonBlocking(m_wakePipe[1]);   // 写端非阻塞: 管道满时入队不阻塞用户线程

    m_stop = false;
    m_isConnected = false;
    {
        std::lock_guard<std::mutex> lk(m_txMutex);
        m_txQueue.clear();             // 丢弃上个生命周期遗留的入队报文
        m_nextPacketId = 1;
    }

    // 注: std::thread 构造在资源耗尽时抛 std::system_error;本工程禁异常,该失败概率
    // 极低且属进程级资源耗尽,按终止处理(这是"禁异常"工程的已知边界,如实注释)。
    m_thread = std::thread(&MqttClient::networkThread, this);
    return true;
}

void MqttClient::stop()
{
    if (!m_thread.joinable()) {
        // 从未 start 或已停止: 清理可能残留的管道(无网络线程,无竞态)
        std::lock_guard<std::mutex> lk(m_wakeMutex);
        if (m_wakePipe[0] >= 0) {
            ::close(m_wakePipe[0]);
            m_wakePipe[0] = -1;
        }
        if (m_wakePipe[1] >= 0) {
            ::close(m_wakePipe[1]);
            m_wakePipe[1] = -1;
        }
        m_isConnected = false;
        return;
    }
    // 1) 置标志 → 2) 唤醒所有阻塞点 → 3) join。
    // 不在本线程 close/shutdown socket: 理由见文件头注释 7(fd 复用竞态)。
    // join 有界性: 所有等待都监听唤醒管道;send/recv 由 SO_SNDTIMEO/SO_RCVTIMEO 兜底。
    m_stop = true;
    wakePipe();
    m_thread.join();
    m_isConnected = false;
}

// ---------------------------------------------------------------------------
// 对外 API(用户线程调用)
// ---------------------------------------------------------------------------

bool MqttClient::publish(const std::string& topic, const std::string& payload, uint8_t qos,
                         bool retain, std::string* err)
{
    if (topic.empty()) {
        return setErr(err, "empty topic");
    }
    if (qos > 1) {
        return setErr(err, "qos must be 0 or 1 (QoS2 not supported)");
    }
    uint16_t packetId = 0;
    if (qos > 0) {
        packetId = allocPacketId();   // QoS1 必须携带非 0 包 ID(MQTT 3.1.1 §2.3.1)
    }
    TxItem item;
    item.type = (uint8_t)mqtt::PacketType::Publish;
    item.qos = qos;
    item.packetId = packetId;
    item.bytes = mqtt::encodePublish(topic, payload, qos, retain, packetId);
    enqueuePacket(std::move(item));
    return true;
}

bool MqttClient::subscribe(const std::string& topic, uint8_t qos, std::string* err)
{
    if (topic.empty()) {
        return setErr(err, "empty topic");
    }
    if (qos > 1) {
        return setErr(err, "qos must be 0 or 1");
    }
    {
        std::lock_guard<std::mutex> lk(m_subMutex);
        bool found = false;
        for (auto& s : m_subscriptions) {
            if (s.first == topic) {
                s.second = qos;       // 已订阅同 topic: 仅更新 QoS
                found = true;
                break;
            }
        }
        if (!found) {
            m_subscriptions.emplace_back(topic, qos);
        }
    }
    uint16_t id = allocPacketId();
    TxItem item;
    item.type = (uint8_t)mqtt::PacketType::Subscribe;
    item.packetId = id;
    item.bytes = mqtt::encodeSubscribe(id, {{topic, qos}});
    enqueuePacket(std::move(item));
    return true;
}

bool MqttClient::unsubscribe(const std::string& topic, std::string* err)
{
    if (topic.empty()) {
        return setErr(err, "empty topic");
    }
    {
        std::lock_guard<std::mutex> lk(m_subMutex);
        for (auto it = m_subscriptions.begin(); it != m_subscriptions.end(); ++it) {
            if (it->first == topic) {
                m_subscriptions.erase(it);
                break;
            }
        }
    }
    uint16_t id = allocPacketId();
    TxItem item;
    item.type = (uint8_t)mqtt::PacketType::Unsubscribe;
    item.packetId = id;
    item.bytes = mqtt::encodeUnsubscribe(id, {topic});
    enqueuePacket(std::move(item));
    return true;
}

void MqttClient::setMessageHandler(MqttMessageHandler h)
{
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_msgHandler = std::move(h);
}

void MqttClient::setStateHandler(MqttStateHandler h)
{
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_stateHandler = std::move(h);
}

bool MqttClient::isConnected() const
{
    return m_isConnected.load();
}

MqttStats MqttClient::stats() const
{
    MqttStats s;
    s.sentPackets = m_sentPackets.load();
    s.recvPackets = m_recvPackets.load();
    s.publishesSent = m_publishesSent.load();
    s.publishesAcked = m_publishesAcked.load();
    s.reconnectCount = m_reconnectCount.load();
    s.pingSent = m_pingSent.load();
    return s;
}

const MqttConfig& MqttClient::config() const
{
    return m_cfg;   // 构造后只读,线程安全
}

// ---------------------------------------------------------------------------
// 网络线程主循环
// ---------------------------------------------------------------------------

void MqttClient::networkThread()
{
    // 未连接 → 退避等待 + 建连 + 握手;已连接 → 在线收发。
    // 所有"等待"都监听唤醒管道,stop() 随时可打断,线程必然退出。
    while (!m_stop.load()) {
        if (m_sock < 0) {
            // 首次连接不退避(启动延迟无必要);重连才走指数退避(L3)
            uint32_t delay = 0;
            if (m_reconnectCount.load() > 0 || m_backoffMs != 0) {
                delay = nextBackoffDelay();
                delay = std::max(delay, 50u);   // base=0 时给最小下限,防忙循环
                if (waitWake(delay)) {
                    break;                      // 停止请求
                }
            }
            ++m_reconnectCount;
            if (!connectSocket()) {
                onDisconnect("connect failed");
                continue;
            }
            if (!doHandshake()) {
                onDisconnect("handshake failed");  // CONNACK returnCode≠0 已内部记日志
                continue;
            }
            m_backoffMs = 0;                       // 握手成功 → 退避清零
            m_idleSinceMs = nowMs();
            m_isConnected = true;
            notifyState(true, "");
            if (m_cfg.cleanSession) {
                resubscribeAll();                  // clean session: 每次连接后重建订阅
            }
            continue;
        }
        if (!onlineLoop()) {
            onDisconnect(m_disconnectReason);      // 内部已 closeSocket
            continue;
        }
    }

    // ---- 线程退出清理 ----
    if (m_sock >= 0) {
        if (m_isConnected.load()) {
            // 尽力 DISCONNECT: 此时网络线程独占 socket,无并发写
            (void)sendRaw(mqtt::encodeDisconnect());
        }
        closeSocket();
    }
    bool wasConnected = m_isConnected.exchange(false);
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
    if (wasConnected) {
        notifyState(false, "client stopped");
    }
}

// ---------------------------------------------------------------------------
// 在线一轮: 冲刷发送队列 → keepalive → QoS1 重发 → poll 收发
// ---------------------------------------------------------------------------

bool MqttClient::onlineLoop()
{
    // 1) 冲刷用户线程入队的报文(发布/订阅/退订)
    if (!drainTxQueue()) {
        m_disconnectReason = "send failed";
        return false;
    }

    uint64_t now = nowMs();

    // 2) keepalive: 空闲超过 0.7×keepalive 即发 PINGREQ(语义见文件头注释 1)
    if (m_cfg.keepaliveSec > 0) {
        uint64_t thresholdMs = (uint64_t)m_cfg.keepaliveSec * 1000u * 7u / 10u;
        if (now - m_idleSinceMs >= thresholdMs) {
            if (!sendRaw(mqtt::encodePingreq())) {
                m_disconnectReason = "pingreq send failed";
                return false;
            }
            ++m_pingSent;
            m_idleSinceMs = nowMs();   // 发包即刷新空闲计时
            m_pingSentAtMs = m_idleSinceMs;  // 开启 PINGRESP 看门狗计时(H1)
        }
    }

    // 3) QoS1 在途发布: 10s 未确认 → 置 DUP 重发一次;再 10s 仍未确认 → 丢弃(告警)
    now = nowMs();
    for (auto it = m_pendingPubs.begin(); it != m_pendingPubs.end();) {
        if (now - it->sentMs >= kPubackTimeoutMs) {
            if (it->resendCount == 0) {
                it->packet[0] |= 0x08;               // 固定头 DUP 位(bit3)=1
                if (!sendRaw(it->packet)) {
                    m_disconnectReason = "resend failed";
                    return false;
                }
                it->sentMs = nowMs();
                it->resendCount = 1;
                std::fprintf(stderr, "[mqtt] QoS1 packetId=%u unacked, resend with DUP\n",
                             (unsigned)it->packetId);
                ++it;
            } else {
                std::fprintf(stderr, "[mqtt] QoS1 packetId=%u still unacked, dropped\n",
                             (unsigned)it->packetId);
                it = m_pendingPubs.erase(it);
            }
        } else {
            ++it;
        }
    }

    // 4) poll: 同时等 socket 可读/出错 与唤醒管道;超时取"到下一个截止点"与 1s 的较小值,
    //    保证 keepalive/重发/停止的响应误差 ≤1s
    uint64_t nextDeadline = now + (uint64_t)kPollCapMs;
    if (m_cfg.keepaliveSec > 0) {
        nextDeadline = std::min(nextDeadline,
                                m_idleSinceMs + (uint64_t)m_cfg.keepaliveSec * 1000u * 7u / 10u);
        if (m_pingSentAtMs != 0) {
            // PINGRESP 看门狗截止点: 1.5×keepalive(与 broker 的 1.5× 断连窗口对应)
            nextDeadline = std::min(nextDeadline,
                                    m_pingSentAtMs + (uint64_t)m_cfg.keepaliveSec * 1500u);
        }
    }
    for (const auto& p : m_pendingPubs) {
        nextDeadline = std::min(nextDeadline, p.sentMs + kPubackTimeoutMs);
    }
    int64_t pollTimeout = (int64_t)(nextDeadline - nowMs());
    if (pollTimeout < 0) {
        pollTimeout = 0;
    }

    struct pollfd pfd[2];
    pfd[0].fd = m_sock;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = m_wakePipe[0];
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;
    int rc = ::poll(pfd, 2, (int)std::min<int64_t>(pollTimeout, kPollCapMs));
    if (rc < 0) {
        if (errno == EINTR) {
            return true;
        }
        m_disconnectReason = std::string("poll: ") + std::strerror(errno);
        return false;
    }
    if (pfd[1].revents & POLLIN) {
        drainWakePipe();   // 唤醒即消费;下一轮循环顶部自然冲刷队列
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        m_disconnectReason = "socket error or hangup";
        return false;
    }
    if (pfd[0].revents & POLLIN) {
        if (!processIncoming()) {
            return false;   // 内部已设置 m_disconnectReason
        }
    }
    // 5) PINGRESP 看门狗: 发出 PINGREQ 后 1.5×keepalive 内没有任何入站报文
    //    → broker 假死/网络黑洞(连接不 FIN 不 RST),主动判死走重连,避免"假在线"
    if (m_cfg.keepaliveSec > 0 && m_pingSentAtMs != 0 &&
        nowMs() - m_pingSentAtMs > (uint64_t)m_cfg.keepaliveSec * 1500u) {
        m_disconnectReason = "pingresp timeout (broker unresponsive)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 发送队列与 QoS1 在途登记
// ---------------------------------------------------------------------------

bool MqttClient::drainTxQueue()
{
    std::deque<TxItem> batch;
    {
        std::lock_guard<std::mutex> lk(m_txMutex);
        if (m_txQueue.empty()) {
            return true;
        }
        batch.swap(m_txQueue);
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        const TxItem& item = batch[i];
        if (m_cfg.cleanSession && !item.queuedWhileConnected &&
            (item.type == (uint8_t)mqtt::PacketType::Subscribe ||
             item.type == (uint8_t)mqtt::PacketType::Unsubscribe)) {
            // 断线期间入队的订阅/退订: cleanSession=true 时重连后由 resubscribeAll
            // 按 m_subscriptions 统一重建,队列里的旧副本跳过,避免重复 SUBSCRIBE(M7)。
            // 在线入队的订阅必须走队列发送(resubscribeAll 只在连接建立时跑一次)。
            continue;
        }
        if (!sendRaw(item.bytes)) {
            // 发送失败: 剩余报文退回队首,待重连后继续发送(QoS1 至少一次语义)
            std::lock_guard<std::mutex> lk(m_txMutex);
            m_txQueue.insert(m_txQueue.begin(), batch.begin() + (ptrdiff_t)i, batch.end());
            std::fprintf(stderr, "[mqtt] tx send failed, %zu packet(s) requeued\n",
                         batch.size() - i);
            return false;
        }
        if (item.type == (uint8_t)mqtt::PacketType::Publish) {
            ++m_publishesSent;
            if (item.qos == 1 && item.packetId != 0) {
                PendingPub pp;
                pp.packetId = item.packetId;
                pp.packet = item.bytes;
                pp.sentMs = nowMs();
                m_pendingPubs.push_back(std::move(pp));   // 开始 10s 确认计时
            }
        }
    }
    return true;
}

bool MqttClient::sendRaw(const std::vector<uint8_t>& data)
{
    if (m_sock < 0) {
        return false;
    }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(m_sock, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN/EWOULDBLOCK 在阻塞 socket + SO_SNDTIMEO 下意味着"发送超时"——
        // 对端已不消费,直接判失败走重连,避免无限重试;任何成功发送都会刷新空闲计时
        return false;
    }
    ++m_sentPackets;
    m_idleSinceMs = nowMs();
    return true;
}

// ---------------------------------------------------------------------------
// 收包与报文处理
// ---------------------------------------------------------------------------

bool MqttClient::processIncoming()
{
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(m_sock, buf, sizeof(buf), 0);
        if (n > 0) {
            m_rxBuf.append(buf, (size_t)n);
            if (m_rxBuf.size() > kRxBufMax) {
                m_disconnectReason = "rx buffer overflow";
                return false;
            }
            break;   // 先拆包处理,下一轮循环再收剩余,避免一次读爆缓冲
        }
        if (n == 0) {
            m_disconnectReason = "broker closed connection";
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;   // 本批已读完
        }
        if (errno == EINTR) {
            continue;
        }
        m_disconnectReason = std::string("recv: ") + std::strerror(errno);
        return false;
    }
    // 拆包循环: decodePacket 返回 false 且 err 为空 = 数据不足(半包,等下一批);
    // err 非空 = 协议错误(断开重连)
    for (;;) {
        if (m_rxBuf.empty()) {
            return true;
        }
        size_t consumed = 0;
        std::string err;
        mqtt::Packet pkt;
        if (!mqtt::decodePacket(reinterpret_cast<const uint8_t*>(m_rxBuf.data()),
                                m_rxBuf.size(), &consumed, &pkt, &err)) {
            if (!err.empty()) {
                m_disconnectReason = "protocol error: " + err;
                return false;
            }
            return true;
        }
        m_rxBuf.erase(0, consumed);
        ++m_recvPackets;
        m_pingSentAtMs = 0;   // 任意入站报文都证明 broker 活着: 清除 PINGRESP 看门狗(H1)
        if (!handlePacket(pkt)) {
            return false;
        }
    }
}

bool MqttClient::handlePacket(const mqtt::Packet& pkt)
{
    switch (pkt.type) {
    case mqtt::PacketType::Publish: {
        if (pkt.qos == 1 && pkt.packetId == 0) {
            m_disconnectReason = "PUBLISH qos1 with packetId=0 (protocol violation)";
            return false;
        }
        MqttMessage msg;
        msg.topic = pkt.topic;
        msg.payload = pkt.payload;
        msg.qos = pkt.qos;
        msg.retain = pkt.retain;
        if (pkt.qos == 0) {
            notifyMessage(msg);                  // QoS0: 直接回调
        } else if (pkt.qos == 1) {
            notifyMessage(msg);                  // 先处理、后 PUBACK: "至少一次"语义
            if (!sendRaw(mqtt::encodePuback(pkt.packetId))) {
                m_disconnectReason = "puback send failed";
                return false;
            }
        } else {
            // QoS2 接收需要 PUBREC/PUBREL/PUBCOMP 状态机,本项目不实现;不回应即触发
            // broker 重发,这里告警后忽略,避免"假确认"造成丢消息(偏差记录见 subagent 报告)
            std::fprintf(stderr, "[mqtt] received QoS2 publish, unsupported, ignored\n");
        }
        return true;
    }
    case mqtt::PacketType::Puback: {
        // 匹配在途表: PUBACK 的包 ID 必须对应一条未确认的 QoS1 发布
        for (auto it = m_pendingPubs.begin(); it != m_pendingPubs.end(); ++it) {
            if (it->packetId == pkt.packetId) {
                m_pendingPubs.erase(it);
                ++m_publishesAcked;
                return true;
            }
        }
        // 重复/迟到的确认: 协议允许,仅告警
        std::fprintf(stderr, "[mqtt] unexpected PUBACK packetId=%u\n", (unsigned)pkt.packetId);
        return true;
    }
    case mqtt::PacketType::Pingresp:
        return true;                             // broker 活着,keepalive 闭环
    case mqtt::PacketType::Suback:
        // 订阅确认;子返回码 0x80 表示该主题被拒绝,这里仅记录(业务可自行关注)
        std::fprintf(stderr, "[mqtt] SUBACK packetId=%u\n", (unsigned)pkt.packetId);
        return true;
    case mqtt::PacketType::Unsuback:
        return true;
    case mqtt::PacketType::Disconnect:
        // MQTT 3.1.1 §3.14: 服务端不得发送 DISCONNECT,收到视为异常
        m_disconnectReason = "broker sent DISCONNECT (protocol violation)";
        return false;
    case mqtt::PacketType::Connack:
        m_disconnectReason = "duplicate CONNACK";
        return false;
    default:
        std::fprintf(stderr, "[mqtt] unhandled packet type %d, ignored\n", (int)pkt.type);
        return true;
    }
}

// 握手期专用: 等待并解码"下一个完整报文";数据不足继续收,超时/断线/协议错误返回 false。
// 注意同时监听唤醒管道,保证 stop() 可随时打断 10s 的 CONNACK 等待。
bool MqttClient::recvPacket(mqtt::Packet* out, uint64_t timeoutMs)
{
    uint64_t deadline = nowMs() + timeoutMs;
    while (!m_stop.load()) {
        int64_t remain = (int64_t)(deadline - nowMs());
        if (remain <= 0) {
            return false;
        }
        struct pollfd pfd[2];
        pfd[0].fd = m_sock;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = m_wakePipe[0];
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        int rc = ::poll(pfd, 2, (int)std::min<int64_t>(remain, kPollCapMs));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (pfd[1].revents & POLLIN) {
            drainWakePipe();
            if (m_stop.load()) {
                return false;
            }
            continue;
        }
        if (rc == 0) {
            continue;   // 未到超时时刻
        }
        if (!(pfd[0].revents & POLLIN)) {
            if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return false;
            }
            continue;
        }
        char buf[4096];
        ssize_t n = ::recv(m_sock, buf, sizeof(buf), 0);
        if (n > 0) {
            m_rxBuf.append(buf, (size_t)n);
            if (m_rxBuf.size() > kRxBufMax) {
                return false;
            }
            size_t consumed = 0;
            std::string err;
            if (mqtt::decodePacket(reinterpret_cast<const uint8_t*>(m_rxBuf.data()),
                                   m_rxBuf.size(), &consumed, out, &err)) {
                m_rxBuf.erase(0, consumed);
                ++m_recvPackets;
                return true;
            }
            if (!err.empty()) {
                std::fprintf(stderr, "[mqtt] decode error: %s\n", err.c_str());
                return false;
            }
            continue;   // 半包,继续收
        }
        if (n == 0) {
            return false;   // 对端关闭
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 建连与握手
// ---------------------------------------------------------------------------

bool MqttClient::connectSocket()
{
    std::string portStr = std::to_string(m_cfg.port);
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;             // 目标场景 IPv4(嵌入式/局域网);IPv6 可扩展
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(m_cfg.host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "[mqtt] getaddrinfo(%s): %s\n", m_cfg.host.c_str(),
                     gai_strerror(rc));
        return false;
    }
    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        setFdNonBlocking(fd);              // 非阻塞 connect,用 poll 限时,避免内核 2 分钟级超时
        int ret = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (ret == 0) {
            break;                         // 立即成功(本机回环常见)
        }
        if (errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;                      // 立即失败(拒绝等),试下一个地址
        }
        // 连接进行中 → poll 等可写;同时监听唤醒管道,stop() 可打断
        struct pollfd pfd[2];
        pfd[0].fd = fd;
        pfd[0].events = POLLOUT;
        pfd[0].revents = 0;
        pfd[1].fd = m_wakePipe[0];
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        int prc = ::poll(pfd, 2, (int)kConnectTimeoutMs);
        if (prc <= 0) {
            ::close(fd);
            fd = -1;
            continue;                      // 超时或 poll 错误
        }
        if (pfd[1].revents & POLLIN) {
            drainWakePipe();
            ::close(fd);
            fd = -1;
            break;                         // stop 请求: 放弃本轮连接
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ::close(fd);
            fd = -1;
            continue;                      // 连接被拒等
        }
        // 非阻塞 connect 的结果由 SO_ERROR 表达
        int soErr = 0;
        socklen_t slen = sizeof(soErr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &slen) != 0 || soErr != 0) {
            ::close(fd);
            fd = -1;
            continue;
        }
        break;                             // 连接成功
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        return false;
    }
    // 恢复阻塞模式,并设收发超时兜底: 主驱动仍是 poll;SO_RCVTIMEO 保证 recv 绝不死等,
    // SO_SNDTIMEO 保证 send 有界(stop() 的 join 不被拖死)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    struct timeval tv;
    tv.tv_sec = (time_t)(kSockTimeoutMs / 1000);
    tv.tv_usec = (suseconds_t)((kSockTimeoutMs % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    m_sock = fd;
    return true;
}

bool MqttClient::doHandshake()
{
    mqtt::ConnectOptions opt;
    opt.clientId = m_cfg.clientId;
    opt.keepaliveSec = m_cfg.keepaliveSec;
    opt.cleanSession = m_cfg.cleanSession;
    opt.username = m_cfg.username;
    opt.password = m_cfg.password;

    if (!sendRaw(mqtt::encodeConnect(opt))) {
        std::fprintf(stderr, "[mqtt] CONNECT send failed\n");
        return false;
    }
    mqtt::Packet pkt;
    if (!recvPacket(&pkt, kConnackTimeoutMs)) {
        std::fprintf(stderr, "[mqtt] CONNACK timeout or connection lost\n");
        return false;
    }
    if (pkt.type != mqtt::PacketType::Connack) {
        std::fprintf(stderr, "[mqtt] expected CONNACK, got type %d\n", (int)pkt.type);
        return false;
    }
    if (pkt.returnCode != 0) {
        // returnCode 含义(MQTT 3.1.1 §3.2.2.3):
        // 1=不接受的协议版本 2=标识符被拒绝 3=服务器不可用 4=用户名/密码错误 5=未授权
        std::fprintf(stderr,
                     "[mqtt] CONNACK rejected, returnCode=%u (1=bad proto 2=id rejected "
                     "3=server unavailable 4=bad auth 5=not authorized), will reconnect\n",
                     (unsigned)pkt.returnCode);
        return false;
    }
    std::fprintf(stderr, "[mqtt] connected to %s:%u (clientId=%s, cleanSession=%d, keepalive=%us)\n",
                 m_cfg.host.c_str(), (unsigned)m_cfg.port, m_cfg.clientId.c_str(),
                 m_cfg.cleanSession ? 1 : 0, (unsigned)m_cfg.keepaliveSec);
    return true;
}

// ---------------------------------------------------------------------------
// 断线处理 / 退避 / 唤醒管道 / 回调
// ---------------------------------------------------------------------------

void MqttClient::onDisconnect(const std::string& reason)
{
    closeSocket();
    bool wasConnected = m_isConnected.exchange(false);
    if (wasConnected) {
        std::fprintf(stderr, "[mqtt] disconnected: %s\n", reason.c_str());
        notifyState(false, reason);
    } else {
        std::fprintf(stderr, "[mqtt] connect attempt failed: %s\n", reason.c_str());
    }
}

void MqttClient::closeSocket()
{
    if (m_sock >= 0) {
        ::close(m_sock);
        m_sock = -1;
    }
    m_rxBuf.clear();
    m_pingSentAtMs = 0;   // 断开即复位看门狗,新连接从头计时(H1)
    // 断线即清空在途 QoS1。cleanSession=true 时 broker 也会丢弃这些消息,语义一致;
    // cleanSession=false 场景的"跨断线续传"(需持久化在途表并在重连后按 DUP 重发)
    // 超出本项目范围,如实注释为已知限制(偏差记录见 subagent 报告)
    m_pendingPubs.clear();
}

uint32_t MqttClient::nextBackoffDelay()
{
    if (m_backoffMs == 0) {
        m_backoffMs = m_cfg.reconnectBaseMs;
    }
    uint32_t delay = m_backoffMs;
    // xorshift32(轻量自实现伪随机,避免依赖 rand 的全局状态): 0~30% 抖动
    uint32_t x = m_rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_rngState = x;
    uint32_t jitterPercent = x % 31;       // 0..30
    uint64_t d = (uint64_t)delay + (uint64_t)delay * jitterPercent / 100u;
    m_backoffMs = std::min(m_backoffMs * 2u, m_cfg.reconnectMaxMs);
    return (uint32_t)d;
}

// 可中断睡眠: 退避等待期间 stop() 写唤醒管道立即返回;返回 true = 收到停止请求
bool MqttClient::waitWake(uint32_t delayMs)
{
    if (delayMs == 0) {
        return m_stop.load();
    }
    struct pollfd pfd;
    pfd.fd = m_wakePipe[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    uint32_t remain = delayMs;
    while (remain > 0 && !m_stop.load()) {
        int chunk = (int)std::min<uint32_t>(remain, 1000u);
        int rc = ::poll(&pfd, 1, chunk);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            drainWakePipe();
            break;
        }
        if (rc < 0 && errno != EINTR) {
            break;
        }
        remain -= (uint32_t)chunk;
    }
    return m_stop.load();
}

void MqttClient::resubscribeAll()
{
    std::vector<std::pair<std::string, uint8_t>> subs;
    {
        std::lock_guard<std::mutex> lk(m_subMutex);
        subs = m_subscriptions;
    }
    if (subs.empty()) {
        return;
    }
    uint16_t id = allocPacketId();
    std::vector<uint8_t> pkt = mqtt::encodeSubscribe(id, subs);
    if (!sendRaw(pkt)) {
        std::fprintf(stderr, "[mqtt] resubscribe send failed\n");
    }
}

uint16_t MqttClient::allocPacketId()
{
    std::lock_guard<std::mutex> lk(m_txMutex);
    uint16_t id = m_nextPacketId;
    // 包 ID 空间 1..65535(MQTT 3.1.1 §2.3.1),0 非法;循环复用
    m_nextPacketId = (uint16_t)((m_nextPacketId % 65535) + 1);
    return id;
}

void MqttClient::enqueuePacket(TxItem item)
{
    {
        std::lock_guard<std::mutex> lk(m_txMutex);
        if (m_txQueue.size() >= kMaxTxQueue) {
            // 队列满(典型场景: broker 长期断线): 丢弃新报文保最新状态(H4),
            // 遥测是状态型数据,丢旧保新优于无界积压后恢复期发送风暴
            ++m_droppedTx;
            return;
        }
        item.queuedWhileConnected = m_isConnected.load();  // M7: 记录入队时连接状态
        m_txQueue.push_back(std::move(item));
    }
    wakePipe();
}

void MqttClient::wakePipe()
{
    // 写端非阻塞;与"关管道"共用一把锁: 保证不会写到读端已关闭的管道(SIGPIPE 杀进程)
    std::lock_guard<std::mutex> lk(m_wakeMutex);
    if (m_wakePipe[1] >= 0) {
        char c = 'w';
        (void)::write(m_wakePipe[1], &c, 1);   // EAGAIN=管道满: 无妨,网络线程本就要轮询
    }
}

void MqttClient::drainWakePipe()
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

void MqttClient::notifyMessage(const MqttMessage& msg)
{
    MqttMessageHandler h;
    {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        h = m_msgHandler;
    }
    if (h) {
        h(msg);   // 网络线程内执行;禁止阻塞/抛异常(权衡见文件头注释)
    }
}

void MqttClient::notifyState(bool connected, const std::string& reason)
{
    MqttStateHandler h;
    {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        h = m_stateHandler;
    }
    if (h) {
        h(connected, reason);
    }
}

bool MqttClient::setErr(std::string* err, const std::string& msg)
{
    if (err) {
        *err = msg;
    }
    return false;
}

} // namespace es
