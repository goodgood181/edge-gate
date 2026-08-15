// 文件路径: src/app/gateway.h
// 职责: 网关应用层总装 —— 把 core/util/hal/modbus/edge/net 各模块组装成
//       可运行的工业数据采集网关: 点表配置、线程生命周期、状态机接线、
//       命令路由、遥测聚合上云与 JSONL 落盘、pty-sim 模拟链路装配。
//       CLI / Qt GUI / CmdServer(TCP) / MQTT 命令四类前端全部经由本类
//       对外接口访问网关,是应用层唯一的门面。
//
// 设计要点:
// 1) 为什么单采集线程轮询串口(单工瓶颈与取舍):
//    RS485 是共享半双工总线 —— 同一时刻线上只允许一个主站发起事务,
//    任何时刻最多一个请求在传输。若开"每点一线程"并发读:
//    a) 两帧在线上交错,双方都收不到合法应答,协议直接撕裂;
//    b) 线程数与点表规模成正比,嵌入式上不可扩展;
//    c) 同一 fd 的多线程读写必须加锁串行化,加了锁又回到单线程。
//    因此采集线程是"单线程 + 到期点调度"(ModbusMaster::duePoints 按各点
//    pollPeriodMs 返回到期索引,500ms/1000ms/2000ms 的点天然交错,无需
//    每点一个定时器)。代价即"单工瓶颈": 单点超时(3.5T+1s 兜底)期间
//    其余到期点排队等待,业界通用解法是"轮询周期错峰 + 分时",而不是多线程。
// 2) 线程模型(契约 §11): 采集 / 遥测由本类直接管理;MQTT / CmdServer /
//    模拟从站各自内部带线程。所有跨线程共享状态(点表/滤波/告警引擎)由
//    m_mutex 保护;串口事务由 m_serialMutex 二次串行化(命令线程的写寄存
//    与采集线程的读不得交错 —— 见设计要点 1c)。
// 3) 停止顺序(契约 §11): 采集 → 遥测 → MQTT → CmdServer → 从站,全部 join。
//    先停生产者(采集),再停消费者(遥测/MQTT),杜绝"线程仍在往销毁中的
//    对象里发数据";从站最后停,模拟链路不再产生新数据。stop() 幂等,
//    任意时刻可调用(含 start 中途失败后的回滚清理)。
// 4) 状态机接线(契约 §10/§12): SerialError/SerialRecovered 由采集线程判定
//    (连续整轮失败 ≥ 阈值 → Fault;恢复 → Running),每次转移发布 retained
//    status;MQTT 连接成功也发 retained status;recover 命令可手动从 Fault
//    拉回;恢复后 RuleEngine reset(清告警状态,下一轮按当前值重新评估)。
// 5) 命令路由统一: CmdServer(TCP) 与 MQTT cmd 订阅共用 handleCommand(),
//    同一份语义(§12 命令表),ack 格式一致 —— 运维通道无论走哪个入口,
//    行为完全相同,不重复实现。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/event_bus.h"
#include "../core/state_machine.h"
#include "../edge/rule_engine.h"
#include "../edge/signal_filter.h"
#include "../hal/serial_device.h"
#include "../modbus/modbus_master.h"
#include "../modbus/modbus_slave.h"
#include "../net/cmd_server.h"
#include "../net/http_server.h"
#include "../net/mqtt_client.h"
#include "../util/config.h"
#include "../util/json.h"

namespace es {

class Gateway {
public:
    Gateway();
    ~Gateway(); // stop() 兜底: 确保全部线程 join,无泄漏

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // 启动网关: 配置 → 串口/点表/(pty-sim 从站) → MQTT → CmdServer →
    // 采集/遥测线程 → Running。失败返回 false 并回滚已启动的部分。
    bool start(const Config& cfg, std::string* err);
    void stop(); // 幂等;按契约 §11 顺序停止并 join 全部线程

    [[nodiscard]] bool running() const;
    [[nodiscard]] State state() const;
    [[nodiscard]] const Config& config() const;

    // 全量快照(线程安全): CLI / Qt / CmdServer 共用,不暴露内部指针
    [[nodiscard]] Json snapshot() const;
    [[nodiscard]] std::string snapshotJson() const;

    // 命令入口(CmdServer 与 MQTT cmd 订阅共用): 入参一行 JSON,返回单行 JSON ack
    std::string handleCommand(const std::string& jsonCmd);

    // 进程内事件总线(Qt 告警列表等订阅 "event" / "status" 主题)
    EventBus& eventBus() { return m_bus; }

    // 供 CLI 直接调用的命令原语(内部与 handleCommand 同源)
    bool setPeriodMs(const std::string& id, uint32_t periodMs, std::string* err);
    bool writeRegister(const std::string& id, uint16_t value, std::string* err);
    bool injectFault(const std::string& fault, std::string* err);
    void recover();

private:
    // ---- 装配 ----
    bool setupSerial(std::string* err);     // pty-sim / 真实串口 + 点表 + 从站
    bool setupMqtt(std::string* err);
    bool setupCmdServer(std::string* err);
    bool setupHttp(std::string* err);        // 内置 Web 监控页(浏览器访问)
    void setupJsonl();                      // JSONL 落盘(打不开仅警告,不致命)
    std::vector<modbus::ModbusPoint> parsePoints() const;

    // ---- 线程体 ----
    void acquisitionLoop();                 // 采集线程主循环(见文件头设计要点 1)
    void telemetryLoop();                   // 遥测线程主循环
    void pollDuePoints();                   // 一轮到期点采集 + 串口故障判定
    bool processPoint(size_t idx);          // 单点: 读 → 滤波 → 标度 → 点表 → 告警

    // ---- 事件与状态 ----
    void onAlarmEvent(const edge::AlarmEvent& ev);
    void onMqttState(bool connected, const std::string& reason);
    void onMqttMessage(const MqttMessage& msg);
    void onStateChanged(State from, State to, Event ev); // 状态机转移回调
    void publishStatus(const std::string& stateName);    // retained status 发布

    // ---- 报文构建 ----
    std::string buildTelemetryJson();
    std::string buildStatusJson(const std::string& stateName);
    static std::string qualityString(modbus::Quality q, uint64_t nowMs,
                                     uint32_t pollMs, uint64_t lastMs);
    void appendJsonl(const std::string& line);

    // ---- 配置副本(start 时固化;此后只读) ----
    Config m_cfg;
    std::string m_deviceId;
    std::string m_deviceName;
    std::string m_topicPrefix;              // {prefix}/telemetry|event|cmd|ack|status
    bool m_mqttEnabled = false;
    uint8_t m_mqttQos = 1;
    bool m_mqttRetainStatus = true;
    uint32_t m_telemetryPeriodMs = 1000;
    uint32_t m_acqTickMs = 10;              // 采集循环空闲节拍(扩展键,默认 10ms)
    uint32_t m_filterWindow = 4;            // 滑动平均窗口(扩展键,默认 4)
    bool m_jsonlEnabled = false;
    std::string m_jsonlPath;

    // ---- 串口与协议栈 ----
    bool m_ptySim = false;
    std::shared_ptr<ISerialDevice> m_masterPort;
    std::shared_ptr<ISerialDevice> m_slavePort;  // 仅 pty-sim 非空
    std::unique_ptr<modbus::ModbusMaster> m_master;
#if defined(ES_BUILD_SIM)
    std::unique_ptr<modbus::ModbusSlave> m_slave;
#endif
    std::unique_ptr<MqttClient> m_mqtt;
    std::unique_ptr<CmdServer> m_cmdServer;
    std::unique_ptr<HttpServer> m_http;     // 内置 Web 监控页(httpServer.port,默认 18080)

    // ---- 点表与边缘计算(采集线程写,查询线程读;m_mutex 保护) ----
    mutable std::mutex m_mutex;
    std::vector<modbus::ModbusPoint> m_points;
    std::vector<std::unique_ptr<edge::SignalFilter>> m_filters; // 与 m_points 对齐
    edge::RuleEngine m_rules;                // 无内部锁(单线程假设),锁内使用
    StateMachine m_sm;
    EventBus m_bus;

    // ---- 串口事务串行化(采集线程 vs 命令线程,见文件头设计要点 1c) ----
    std::mutex m_serialMutex;

    // ---- 线程生命周期 ----
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopAcq{false};
    std::atomic<bool> m_stopTelem{false};
    std::mutex m_cvMutex;
    std::condition_variable m_acqCv;        // 采集线程睡眠(可被 stop 打断)
    std::condition_variable m_telemCv;      // 遥测线程睡眠
    std::thread m_acqThread;
    std::thread m_telemThread;

    // ---- 运行期统计 ----
    uint64_t m_startedMs = 0;
    uint64_t m_consecutiveFailRounds = 0;   // 连续整轮失败计数(Fault 判定)
    uint64_t m_lastWarnLogMs = 0;           // 采集错误日志节流

    // ---- JSONL ----
    std::mutex m_jsonlMutex;
    FILE* m_jsonlFile = nullptr;

    static constexpr uint64_t kSerialFailRounds = 3;    // 整轮失败阈值 → Fault
    static constexpr uint64_t kErrLogIntervalMs = 10000; // 单点错误日志节流(10s)
};

} // namespace es
