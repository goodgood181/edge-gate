// 文件路径: src/net/mqtt_client.h
// 职责: MQTT 3.1.1 客户端(不依赖 Paho/Mosquitto),内部网络线程负责
//      连接 / CONNECT 握手 / 读循环 / keepalive PINGREQ / QoS0·QoS1 收发 / 指数退避重连。
//      报文字节级编解码全部委托 es::mqtt(mqtt_packets.h,另一模块),本模块只做会话与状态管理。
//
// 设计要点:
// 1) 单网络线程 + 用户线程仅"入队": publish/subscribe/unsubscribe 只把编码好的报文放进
//    发送队列并写唤醒管道,不碰 socket。socket 的读/写/关闭全部收敛在网络线程,
//    避免多线程同时 send 的字节交错,也让 stop() 的退出时序可推理。
// 2) 唤醒管道(self-pipe): poll() 同时监听 socket 与管道读端;用户线程入队后写 1 字节
//    唤醒,网络线程立即冲刷队列,发布延迟与 poll 超时无关(毫秒级)。
// 3) stop() 三件套: 置停止标志 → 写唤醒管道 → join。所有阻塞点(poll、connect 等待、
//    退避睡眠)都同时监听唤醒管道,因此 join 一定有界;fd 只由网络线程自己关闭,
//    规避"跨线程 close 后 fd 号被复用、另一线程 poll 到错误对象"的经典竞态
//    (这也是刻意不在 stop() 里直接 close socket 的原因,详见 cpp 注释)。
// 4) 回调(消息/状态)在网络线程内执行: 好处是消息天然保序、无需额外派发队列与锁;
//    代价是回调若阻塞会连带拖垮 keepalive/重发,回调必须快速返回(权衡详见 cpp 注释)。
// 5) 禁异常: 所有错误以 bool 返回值 + std::string* err 表达。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace es {

namespace mqtt {
struct Packet;   // 前向声明: 头文件不依赖 mqtt_packets.h(字节编解码由另一模块提供)
} // namespace mqtt

// MQTT 连接配置(全部带默认值,可直接聚合初始化后按需修改)
struct MqttConfig {
    std::string host = "127.0.0.1";      // broker 地址(IP 或域名,getaddrinfo 解析)
    uint16_t port = 1883;
    std::string clientId;                // 客户端标识;空串时 broker 自动分配(此时 cleanSession 必须为 true)
    uint16_t keepaliveSec = 30;          // keepalive 秒数;0 = 禁用(协议允许)
    bool cleanSession = true;            // true=每次连接全新会话;false=会话延续(语义见 cpp 注释)
    std::string username;                // 可选认证
    std::string password;
    uint32_t reconnectBaseMs = 1000;     // 重连退避起点(1s)
    uint32_t reconnectMaxMs = 30000;     // 重连退避上限(30s)
};

// 收到的应用消息(QoS0 直接回调;QoS1 回调后回 PUBACK)
struct MqttMessage {
    std::string topic;
    std::string payload;
    uint8_t qos = 0;
    bool retain = false;
};

// 收到 PUBLISH 的回调(网络线程内执行,见头注释权衡;禁抛异常、禁阻塞)
using MqttMessageHandler = std::function<void(const MqttMessage& msg)>;
// 连接状态变化回调: connected=true 表示 CONNACK 成功;false 时 reason 说明原因(断线/退出)
using MqttStateHandler = std::function<void(bool connected, const std::string& reason)>;

// 统计(原子变量累加,任意线程可安全读取)
struct MqttStats {
    uint64_t sentPackets = 0;      // 已发送的 MQTT 报文数(CONNECT/PUBLISH/PINGREQ/...)
    uint64_t recvPackets = 0;      // 已解码的入站报文数
    uint64_t publishesSent = 0;    // 发出的 PUBLISH 数(QoS0 + QoS1)
    uint64_t publishesAcked = 0;   // 收到 PUBACK 确认的 QoS1 发布数
    uint64_t reconnectCount = 0;   // 重连尝试次数
    uint64_t pingSent = 0;         // PINGREQ 发送次数
};

class MqttClient {
public:
    explicit MqttClient(const MqttConfig& cfg);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    // 启动网络线程;stop() 后可再次 start()(重启会清空遗留发送队列)
    bool start(std::string* err);
    // 优雅退出: 置标志 → 唤醒管道 → join;退出前若在线则尽力发送 DISCONNECT。
    // 幂等。注意: 禁止在网络线程回调(消息/状态 handler)内调用本方法(会自 join 死锁),
    // 如确需停止,应只置自己的标志,由其他线程调用 stop()。
    void stop();

    [[nodiscard]] bool isConnected() const;

    // 发布: qos 仅支持 0/1(QoS2 不在本项目范围内)。QoS1 走"10s 未确认 DUP 重发一次"流程。
    bool publish(const std::string& topic, const std::string& payload, uint8_t qos, bool retain, std::string* err);
    bool subscribe(const std::string& topic, uint8_t qos, std::string* err);
    bool unsubscribe(const std::string& topic, std::string* err);

    void setMessageHandler(MqttMessageHandler h);
    void setStateHandler(MqttStateHandler h);

    [[nodiscard]] MqttStats stats() const;
    [[nodiscard]] const MqttConfig& config() const;

private:
    // ---- 配置与线程与生命周期 ----
    MqttConfig m_cfg;                        // 构造后只读(config() 返回引用)
    std::thread m_thread;                    // 网络线程
    std::atomic<bool> m_stop{false};         // 停止标志(任意线程写,网络线程读)
    std::atomic<bool> m_isConnected{false};  // 当前是否已 CONNACK 成功

    // ---- 唤醒管道(用户线程 → 网络线程) ----
    int m_wakePipe[2] = {-1, -1};
    std::mutex m_wakeMutex;                  // 串行化"写管道 vs 关管道",防 SIGPIPE 竞态

    // ---- 发送队列(用户线程入队,网络线程冲刷) ----
    // 队列项自带元数据(type/qos/packetId): 不回头解析自己刚编码的字节,
    // 避免"从字节流反推包 ID"这类脆弱逻辑(固定头+变长长度+主题长度+主题+包 ID 的偏移链)
    struct TxItem {
        uint8_t type = 0;          // es::mqtt::PacketType 值(Publish/Subscribe/Unsubscribe)
        uint8_t qos = 0;
        uint16_t packetId = 0;     // QoS1 发布/订阅/退订的包 ID;QoS0 为 0
        std::vector<uint8_t> bytes;
        bool queuedWhileConnected = false;  // 入队时是否已连接(M7: 断线期间入队的
                                            // Subscribe/Unsubscribe 由 resubscribeAll 重建,
                                            // 在线入队的必须走队列发送)
    };
    std::mutex m_txMutex;
    std::deque<TxItem> m_txQueue;
    static constexpr size_t kMaxTxQueue = 512; // 断线期间发送队列上限(防内存无界增长/恢复期风暴)
    uint16_t m_nextPacketId = 1;                      // 包 ID 分配器(1..65535 循环,0 非法)

    // ---- 会话状态(仅网络线程访问) ----
    int m_sock = -1;                          // 当前 socket
    std::string m_rxBuf;                      // 收包缓冲(粘包拆包用)
    std::string m_disconnectReason;           // 最近一次断线原因
    uint64_t m_idleSinceMs = 0;               // 最近一次成功发包的 steady 时刻(keepalive 计时)
    uint64_t m_pingSentAtMs = 0;               // 最近一次 PINGREQ 发出时刻(0=无在途 PINGREQ);
                                               // PINGRESP 看门狗: 入站报文清零,超 1.5×keepalive 判死
    uint32_t m_backoffMs = 0;                 // 当前退避值(0=未进入退避;连接成功即清零)
    uint32_t m_rngState = 0x12345678u;        // xorshift32 种子(重连抖动)

    // QoS1 在途发布(已发出、尚未收到 PUBACK)
    struct PendingPub {
        uint16_t packetId = 0;
        std::vector<uint8_t> packet;          // 已编码报文(重发时置 DUP 位后原样重发)
        uint64_t sentMs = 0;                  // 上次发送时刻(steady)
        int resendCount = 0;                  // 已重发次数(最多 1 次)
    };
    std::vector<PendingPub> m_pendingPubs;

    // ---- 订阅表(用户线程 subscribe/unsubscribe 写,网络线程连接后读) ----
    std::mutex m_subMutex;
    std::vector<std::pair<std::string, uint8_t>> m_subscriptions;

    // ---- 回调(拷贝后调用,防止回调函数被并发替换造成数据竞争) ----
    std::mutex m_cbMutex;
    MqttMessageHandler m_msgHandler;
    MqttStateHandler m_stateHandler;

    // ---- 统计(原子) ----
    std::atomic<uint64_t> m_sentPackets{0};
    std::atomic<uint64_t> m_recvPackets{0};
    std::atomic<uint64_t> m_publishesSent{0};
    std::atomic<uint64_t> m_publishesAcked{0};
    std::atomic<uint64_t> m_reconnectCount{0};
    std::atomic<uint64_t> m_pingSent{0};
    std::atomic<uint64_t> m_droppedTx{0};      // 断线期间因队列满被丢弃的报文数(H4)

    // ---- 内部实现(网络线程执行) ----
    void networkThread();                    // 线程主循环
    bool onlineLoop();                       // 在线一轮: 冲刷队列/keepalive/重发/收发
    bool drainTxQueue();                     // 队列 → socket;QoS1 登记在途
    bool processIncoming();                  // recv + 拆包 + 逐包处理
    bool handlePacket(const mqtt::Packet& pkt);
    bool recvPacket(mqtt::Packet* out, uint64_t timeoutMs);  // 握手期: 等一个完整报文
    bool connectSocket();                    // 非阻塞 connect + poll(10s 超时)
    bool doHandshake();                      // CONNECT → 校验 CONNACK
    void onDisconnect(const std::string& reason);   // 关 socket + 按需通知状态
    void closeSocket();                      // 关 fd(仅网络线程调用)
    bool waitWake(uint32_t ms);              // 可中断睡眠;返回 true = 收到停止请求
    bool sendRaw(const std::vector<uint8_t>& data);    // 全量发送(MSG_NOSIGNAL)
    uint32_t nextBackoffDelay();             // 指数退避 + 0~30% 抖动,并推进退避状态
    uint16_t allocPacketId();
    void enqueuePacket(TxItem item);
    void resubscribeAll();                   // clean session 重连后重建订阅
    void notifyMessage(const MqttMessage& msg);
    void notifyState(bool connected, const std::string& reason);
    void wakePipe();
    void drainWakePipe();
    static bool setErr(std::string* err, const std::string& msg);
};

} // namespace es
