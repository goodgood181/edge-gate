// 文件路径: src/app/gateway.cpp
// 职责: Gateway 实现 —— 线程体、状态机接线、命令处理、报文构建。
// 设计要点与线程模型见 gateway.h;本文件补充实现级要点:
//  - m_serialMutex: 采集线程的读事务与命令线程的写事务互斥,防止两帧
//    在 RS485 半双工线上交错(ModbusMaster 自身不做事务级串行化);
//  - 写寄存器(write_reg)可能阻塞命令线程最长 ~1-2s(等串口事务完成),
//    运维通道低频,可接受;若需严格异步可改为命令队列,演示不必要;
//  - set_period 通过"重新 configure 主站"实现 —— 副作用是主站统计清零
//    与全点立即到期,已注释说明取舍;
//  - 告警评估仅在数据质量 Good 时进行(rule_engine.h 质量位约定)。
#include "gateway.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/stat.h>

#include "../core/logger.h"
#include "../core/time_utils.h"
#include "../hal/posix_serial.h"
#include "../hal/pty_pair.h"
#include "../modbus/modbus_rtu.h"

namespace es {
namespace {

// 逐级创建目录(如 "a/b/c"): 不依赖 std::filesystem,零额外依赖,
// 兼容老交叉工具链(GCC 8 的 filesystem 需 -lstdc++fs)
bool ensureDir(const std::string& path, std::string* err) {
    if (path.empty()) {
        return true;
    }
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] == '/' || last) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                if (err) {
                    *err = "创建目录失败: " + cur + " (" + std::strerror(errno) + ")";
                }
                return false;
            }
        }
    }
    return true;
}

std::string parentDir(const std::string& path) {
    const size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

} // namespace

Gateway::Gateway() = default;

Gateway::~Gateway() {
    stop(); // 兜底: 析构前保证线程全部 join(与 stop() 幂等)
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Gateway::start(const Config& cfg, std::string* err) {
    if (m_running.load()) {
        if (err) *err = "网关已在运行";
        return false;
    }
    m_cfg = cfg;

    // ---- 固化公共配置(启动后只读;getXxx 缺失时回退默认值) ----
    m_deviceId = cfg.getString("device", "id", "edge-gate-01");
    m_deviceName = cfg.getString("device", "name", "EdgeGate 工业数据采集网关");
    m_topicPrefix = cfg.getString("mqtt", "topicPrefix", "edge-gate/" + m_deviceId);
    m_mqttEnabled = cfg.getBool("mqtt", "enabled", true);
    m_mqttQos = (uint8_t)cfg.getInt("mqtt", "qos", 1);
    if (m_mqttQos > 1) m_mqttQos = 1; // 客户端仅支持 QoS0/1
    m_mqttRetainStatus = cfg.getBool("mqtt", "retainStatus", true);
    m_telemetryPeriodMs = (uint32_t)cfg.getInt("mqtt", "telemetryPeriodMs", 1000);
    if (m_telemetryPeriodMs == 0) m_telemetryPeriodMs = 1000;
    m_jsonlEnabled = cfg.getBool("jsonl", "enabled", false);
    m_jsonlPath = cfg.getString("jsonl", "path", "./data/gateway.jsonl");
    // 扩展键(非契约 §12 必填): 缺失时用默认值,配置文件中可不写
    m_acqTickMs = (uint32_t)cfg.getInt("gateway", "acqTickMs", 10);
    if (m_acqTickMs == 0) m_acqTickMs = 10;
    m_filterWindow = (uint32_t)cfg.getInt("gateway", "filterWindow", 4);
    if (m_filterWindow == 0) m_filterWindow = 1;

    m_startedMs = steadyMillis();

    // 1) 配置已加载: Init → Configuring(状态机进入装配阶段)
    m_sm.dispatch(Event::ConfigLoaded);

    // 2) 串口 + 点表 + (pty-sim)模拟从站
    if (!setupSerial(err)) {
        m_sm.dispatch(Event::Fatal);
        return false;
    }

    // 3) JSONL 落盘(遥测线程使用;失败仅警告)
    setupJsonl();

    // 4) MQTT(先于 CmdServer 启动: status/event 发布依赖客户端就绪;
    //    断线期间 publish 入队,重连后自动冲刷 —— MqttClient 内部语义)
    if (m_mqttEnabled) {
        if (!setupMqtt(err)) {
            m_sm.dispatch(Event::Fatal);
            stop(); // 回滚已启动的部分(幂等)
            return false;
        }
    } else {
        ES_LOGI("gw", "MQTT 未启用(配置 mqtt.enabled=false)");
    }

    // 5) CmdServer(TCP 运维通道)
    if (!setupCmdServer(err)) {
        m_sm.dispatch(Event::Fatal);
        stop();
        return false;
    }

    // 5.5) 内置 HTTP 监控页(浏览器访问;失败不致命,仅告警)
    if (!setupHttp(err)) {
        ES_LOGW("gw", "HTTP 监控页启动失败(继续运行): %s", err->c_str());
        err->clear();
    }

    // 6) 采集 / 遥测线程(生产者在消费者之前启动: 数据先积累,无丢失窗口)
    m_stopAcq = false;
    m_stopTelem = false;
    // 注意: std::thread 创建失败会抛 std::system_error(资源耗尽),
    // 本工程视为致命错误(进程终止),与 thread_utils.h 同约定。
    m_acqThread = std::thread([this] { acquisitionLoop(); });
    m_telemThread = std::thread([this] { telemetryLoop(); });

    // 7) Configuring → Running(转移回调发布 retained status)
    m_sm.dispatch(Event::Start);
    m_running = true;
    ES_LOGI("gw", "网关启动完成: %s (state=%s)",
            m_deviceId.c_str(), StateMachine::stateName(m_sm.state()).c_str());
    return true;
}

void Gateway::stop() {
    // 从未启动过(无线程、无子模块): 直接返回
    if (!m_running.load() && !m_acqThread.joinable() && !m_telemThread.joinable() &&
        !m_mqtt && !m_cmdServer) {
        return;
    }
    ES_LOGI("gw", "网关停止: 按 采集→遥测→MQTT→Cmd→从站 顺序 join");
    m_sm.dispatch(Event::Stop); // Running/Fault → Stopped(发布最终 status)

    // 1) 采集线程: 置位 + 唤醒 + join(其内部最多再等一个串口超时周期)
    m_stopAcq = true;
    m_acqCv.notify_all();
    if (m_acqThread.joinable()) {
        m_acqThread.join();
    }
    // 2) 遥测线程
    m_stopTelem = true;
    m_telemCv.notify_all();
    if (m_telemThread.joinable()) {
        m_telemThread.join();
    }
    // 3) MQTT 网络线程(内部置位→唤醒管道→join;幂等)
    if (m_mqtt) {
        m_mqtt->stop();
    }
    // 4) CmdServer 线程(内部 poll 有界退出;幂等)
    if (m_cmdServer) {
        m_cmdServer->stop();
    }
    // 5) HTTP 监控页(浏览器访问;线程内部 poll 有界退出;幂等)
    if (m_http) {
        m_http->stop();
    }
    // 6) 模拟从站线程(应答线程;幂等)
#if defined(ES_BUILD_SIM)
    if (m_slave) {
        m_slave->stop();
    }
#endif
    // 7) 关串口: 线程已全部 join,此刻无并发读写,可安全 close
    if (m_masterPort) {
        m_masterPort->close();
    }
    if (m_slavePort) {
        m_slavePort->close();
    }
    // 7) JSONL
    if (m_jsonlFile) {
        std::fclose(m_jsonlFile);
        m_jsonlFile = nullptr;
    }
    m_running = false;
    ES_LOGI("gw", "网关已停止");
}

bool Gateway::running() const {
    return m_running.load();
}

State Gateway::state() const {
    return m_sm.state();
}

const Config& Gateway::config() const {
    return m_cfg;
}

// ---------------------------------------------------------------------------
// 装配
// ---------------------------------------------------------------------------

std::vector<modbus::ModbusPoint> Gateway::parsePoints() const {
    std::vector<modbus::ModbusPoint> pts;
    const Json arr = m_cfg.getSection("points");
    if (arr.type() != Json::Type::Array) {
        return pts; // 无点表: 由主站 configure 报"点表为空"类错误
    }
    for (const Json& item : arr.items()) {
        if (item.type() != Json::Type::Object) {
            continue;
        }
        modbus::ModbusPoint p;
        p.id = item.get("id", Json("")).asString("");
        p.name = item.get("name", Json("")).asString("");
        p.slaveId = (uint8_t)item.get("slaveId", Json((int64_t)1)).asInt(1);
        p.func = (uint8_t)item.get("func", Json((int64_t)3)).asInt(3);
        p.startAddr = (uint16_t)item.get("startAddr", Json((int64_t)0)).asInt(0);
        p.count = (uint16_t)item.get("count", Json((int64_t)1)).asInt(1);
        p.writable = item.get("writable", Json(false)).asBool(false);
        p.is32Bit = item.get("is32Bit", Json(false)).asBool(false);
        p.bigEndian = item.get("bigEndian", Json(true)).asBool(true);
        p.dataType = item.get("dataType", Json("u16")).asString("u16");
        p.scale = item.get("scale", Json(1.0)).asNumber(1.0);
        p.offset = item.get("offset", Json(0.0)).asNumber(0.0);
        p.unit = item.get("unit", Json("")).asString("");
        p.pollPeriodMs = (uint32_t)item.get("pollPeriodMs", Json((int64_t)1000)).asInt(1000);
        p.highAlarm = item.get("highAlarm", Json(0.0)).asNumber(0.0);
        p.lowAlarm = item.get("lowAlarm", Json(0.0)).asNumber(0.0);
        p.highAlarmEnabled = item.get("highAlarmEnabled", Json(false)).asBool(false);
        p.lowAlarmEnabled = item.get("lowAlarmEnabled", Json(false)).asBool(false);
        p.hysteresis = item.get("hysteresis", Json(0.0)).asNumber(0.0);
        pts.push_back(p);
    }
    return pts;
}

bool Gateway::setupSerial(std::string* err) {
    SerialConfig scfg;
    scfg.device = m_cfg.getString("serial", "device", "pty-sim");
    scfg.baud = (int)m_cfg.getInt("serial", "baud", 9600);
    scfg.dataBits = (int)m_cfg.getInt("serial", "dataBits", 8);
    const std::string parity = m_cfg.getString("serial", "parity", "N");
    scfg.parity = parity.empty() ? 'N' : parity[0];
    scfg.stopBits = (int)m_cfg.getInt("serial", "stopBits", 1);
    // 3.5T 帧间隔按配置波特率注入主站/从站(PTY 无真实波特率,只能应用层指定,
    // 见 ModbusMaster::setCharTimeUs 契约扩展注释)
    const double charUs = modbus::charTimeUs(scfg.baud);

    m_ptySim = (scfg.device == "pty-sim");
    if (m_ptySim) {
#if defined(ES_BUILD_SIM)
        // pty-sim: 建 PTY 对 —— 主站连 master 端、软件从站连 slave 端,
        // 两侧走完全真实的 termios 时序,零硬件闭环(见 pty_pair.h 设计要点)
        PtyPair pp;
        if (!createPtyPair(&pp, err)) {
            return false;
        }
        m_masterPort = std::make_shared<PosixSerial>(scfg, pp.master); // 包装主端 fd
        SerialConfig sscfg = scfg;
        sscfg.device = pp.slaveName;
        m_slavePort = std::make_shared<PosixSerial>(sscfg, pp.slave); // 包装从端 fd
#else
        if (err) *err = "配置 serial.device=pty-sim 需要 ES_BUILD_SIM=ON 重新编译";
        return false;
#endif
    } else {
        // 真实串口(/dev/ttyUSB0、/dev/ttymxc2): 仅主站,无模拟从站
        m_masterPort = std::make_shared<PosixSerial>(scfg);
    }

    std::string oerr;
    if (!m_masterPort->open(&oerr)) {
        if (err) *err = "打开主站串口失败: " + oerr;
        return false;
    }
    if (m_slavePort && !m_slavePort->open(&oerr)) {
        if (err) *err = "打开从站串口失败: " + oerr;
        return false;
    }

    // 点表 → 主站(建索引/校验)+ 本地副本(供 duePoints 索引对齐)+ 滤波 + 告警规则
    const std::vector<modbus::ModbusPoint> points = parsePoints();
    if (points.empty()) {
        if (err) *err = "点表为空: 配置 points 至少需要一个采集点";
        return false;
    }
    m_master = std::make_unique<modbus::ModbusMaster>(m_masterPort);
    m_master->setCharTimeUs(charUs);
    if (!m_master->configure(points, err)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_points = points; // 与主站内部表同源、同序 —— duePoints 返回的索引可直用
        m_filters.clear();
        for (size_t i = 0; i < points.size(); ++i) {
            // 每点独立滤波窗口: 不同物理量的噪声特性不同,互不污染
            m_filters.push_back(std::make_unique<edge::SignalFilter>(m_filterWindow));
        }
        m_rules.configure(points); // 提取阈值/迟滞配置
    }

#if defined(ES_BUILD_SIM)
    if (m_slavePort) {
        const uint8_t slaveId = (uint8_t)m_cfg.getInt("slaveSim", "slaveId", 1);
        const uint16_t regCount = (uint16_t)m_cfg.getInt("slaveSim", "registerCount", 64);
        m_slave = std::make_unique<modbus::ModbusSlave>(m_slavePort, slaveId);
        m_slave->setCharTimeUs(charUs);
        m_slave->setRegisterCount(regCount);
        // 预置演示数据(与 config/edge-gate.json 点表对应):
        //   addr0  = 300    → temp1  0.1 标度 → 30.0 C
        //   addr2-3= f32 20.0 → flow1 冷却水流量 20.0 m3/h
        (void)m_slave->setRegister(0, 300);
        (void)m_slave->setRegister(2, 0x41A0); // 20.0f 高字(bigEndian 惯例)
        (void)m_slave->setRegister(3, 0x0000); // 20.0f 低字
        if (!m_slave->start(err)) {
            return false;
        }
        ES_LOGI("gw", "模拟从站已启动: slaveId=%u registers=%u", slaveId, regCount);
    }
#endif
    ES_LOGI("gw", "串口就绪: device=%s baud=%d pty-sim=%s",
            scfg.device.c_str(), scfg.baud, m_ptySim ? "yes" : "no");
    return true;
}

bool Gateway::setupMqtt(std::string* err) {
    MqttConfig mcfg;
    mcfg.host = m_cfg.getString("mqtt", "host", "127.0.0.1");
    mcfg.port = (uint16_t)m_cfg.getInt("mqtt", "port", 1883);
    mcfg.clientId = m_cfg.getString("mqtt", "clientId", m_deviceId);
    mcfg.keepaliveSec = (uint16_t)m_cfg.getInt("mqtt", "keepaliveSec", 30);
    mcfg.cleanSession = m_cfg.getBool("mqtt", "cleanSession", true);
    mcfg.username = m_cfg.getString("mqtt", "username", "");
    mcfg.password = m_cfg.getString("mqtt", "password", "");

    m_mqtt = std::make_unique<MqttClient>(mcfg);
    // 回调在 MQTT 网络线程内执行: 必须快速返回、禁阻塞(见 mqtt_client.h 权衡)
    m_mqtt->setStateHandler([this](bool connected, const std::string& reason) {
        onMqttState(connected, reason);
    });
    m_mqtt->setMessageHandler([this](const MqttMessage& msg) {
        onMqttMessage(msg);
    });
    if (!m_mqtt->start(err)) {
        return false;
    }
    // 订阅命令主题: 断线时 SUBSCRIBE 入队,重连后客户端自动重建订阅
    // (m_subscriptions 由 subscribe() 记录,resubscribeAll 重连时重发)
    std::string subErr;
    if (!m_mqtt->subscribe(m_topicPrefix + "/cmd", 0, &subErr)) {
        ES_LOGW("gw", "MQTT 订阅命令主题失败: %s", subErr.c_str());
    }
    ES_LOGI("gw", "MQTT 客户端启动: %s:%u clientId=%s",
            mcfg.host.c_str(), mcfg.port, mcfg.clientId.c_str());
    return true;
}

bool Gateway::setupCmdServer(std::string* err) {
    if (!m_cfg.getBool("cmdServer", "enabled", true)) {
        ES_LOGI("gw", "CmdServer 未启用(配置 cmdServer.enabled=false)");
        return true;
    }
    // 每次 start 新建实例: CmdServer stop() 后不可复用(见 cmd_server.h)
    m_cmdServer = std::make_unique<CmdServer>();
    const uint16_t port = (uint16_t)m_cfg.getInt("cmdServer", "port", 19000);
    // 服务器线程内执行: 处理须快速;write_reg 例外(最长 ~1-2s 串口事务,可接受)
    const CmdHandler handler = [this](const std::string& line) {
        return handleCommand(line);
    };
    if (!m_cmdServer->start(port, handler, err)) {
        return false;
    }
    ES_LOGI("gw", "CmdServer 启动: 端口 %u", m_cmdServer->port());
    return true;
}

bool Gateway::setupHttp(std::string* err) {
    if (!m_cfg.getBool("httpServer", "enabled", true)) {
        ES_LOGI("gw", "HTTP 监控页未启用(配置 httpServer.enabled=false)");
        return true;
    }
    // 每次 start 新建实例: HttpServer stop() 后不可复用(见 http_server.h)
    m_http = std::make_unique<HttpServer>();
    const uint16_t port = (uint16_t)m_cfg.getInt("httpServer", "port", 18080);
    // 快照提供者: 复用 snapshotJson()(与 CLI/Qt/CmdServer 同一数据源)
    const SnapshotProvider provider = [this]() { return snapshotJson(); };
    if (!m_http->start(port, provider, err)) {
        return false;
    }
    ES_LOGI("gw", "HTTP 监控页启动: http://localhost:%u", m_http->port());
    return true;
}

void Gateway::setupJsonl() {
    if (!m_jsonlEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_jsonlMutex);
    std::string derr;
    if (!ensureDir(parentDir(m_jsonlPath), &derr)) {
        ES_LOGW("gw", "JSONL 目录创建失败,落盘停用: %s", derr.c_str());
        return;
    }
    m_jsonlFile = std::fopen(m_jsonlPath.c_str(), "a"); // 追加写,逐条 flush
    if (!m_jsonlFile) {
        ES_LOGW("gw", "JSONL 打开失败(%s),落盘停用", m_jsonlPath.c_str());
        return;
    }
    ES_LOGI("gw", "JSONL 落盘: %s", m_jsonlPath.c_str());
}

// ---------------------------------------------------------------------------
// 线程体
// ---------------------------------------------------------------------------

void Gateway::acquisitionLoop() {
    ES_LOGI("gw", "采集线程启动(单线程轮询串口;RS485 单工瓶颈见 gateway.h)");
    std::unique_lock<std::mutex> lk(m_cvMutex);
    while (!m_stopAcq.load()) {
        // 空闲节拍 = m_acqTickMs: duePoints 内部计时到期才产生 IO,
        // 此睡眠只为"及时"发现到期点,同时让 stop() 能唤醒退出
        m_acqCv.wait_for(lk, std::chrono::milliseconds(m_acqTickMs),
                         [this] { return m_stopAcq.load(); });
        if (m_stopAcq.load()) {
            break;
        }
        pollDuePoints();
    }
    ES_LOGI("gw", "采集线程退出");
}

void Gateway::pollDuePoints() {
    const std::vector<size_t> due = m_master->duePoints();
    if (due.empty()) {
        return;
    }
    size_t okCount = 0;
    for (size_t idx : due) {
        if (processPoint(idx)) {
            ++okCount;
        }
    }
    // 状态机接线: 连续整轮失败 → SerialError(Fault);
    // 任一成功 → 复位计数,若在 Fault 则 SerialRecovered(Running)
    if (okCount == 0) {
        ++m_consecutiveFailRounds;
        if (m_consecutiveFailRounds >= kSerialFailRounds &&
            m_sm.state() == State::Running) {
            ES_LOGW("gw", "连续 %llu 轮采集全部失败,判定串口链路故障 → Fault",
                    (unsigned long long)kSerialFailRounds);
            m_sm.dispatch(Event::SerialError);
        }
    } else {
        if (m_consecutiveFailRounds >= kSerialFailRounds &&
            m_sm.state() == State::Fault) {
            ES_LOGI("gw", "串口链路恢复 → Running(告警引擎已复位)");
            m_sm.dispatch(Event::SerialRecovered);
        }
        m_consecutiveFailRounds = 0;
    }
}

bool Gateway::processPoint(size_t idx) {
    modbus::ModbusPoint p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (idx >= m_points.size()) {
            return false;
        }
        p = m_points[idx]; // 局部拷贝: 串口事务期间不持点表锁
    }
    if (p.writable) {
        // 写点没有"读"语义(06 仅写单寄存器): 到期只是调度心跳,不产生 IO,
        // 其值/质量由 write_reg 命令维护(写成功 → Good)。
        // 若需回读验证,应另配一个 03 读镜像点,协议层不隐式回读。
        return true;
    }

    std::vector<uint16_t> regs;
    std::string err;
    const uint64_t now = steadyMillis();
    {
        // 串口事务串行化: 与 write_reg 命令互斥,防两帧交错(见 gateway.h 要点 1c)
        std::lock_guard<std::mutex> slk(m_serialMutex);
        if (!m_master->readPoint(p, &regs, &err)) {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (idx >= m_points.size()) {
                return false;
            }
            m_points[idx].quality = modbus::Quality::Bad; // 质量位降级,遥测携带
            ++m_points[idx].errCount;
            if (now - m_lastWarnLogMs > kErrLogIntervalMs) {
                ES_LOGW("gw", "读点 %s 失败: %s", p.id.c_str(), err.c_str());
                m_lastWarnLogMs = now;
            }
            return false;
        }
    }

    bool ok = false;
    const double raw = m_master->convertRaw(p, regs, &ok); // 寄存器 → 物理量
    if (!ok) {
        return false; // 配置错误(类型/宽度不匹配),convertRaw 置 ok=false
    }
    double filtered = raw;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (idx >= m_points.size()) {
            return false;
        }
        modbus::ModbusPoint& cur = m_points[idx];
        cur.rawValue = raw;
        if (m_filters[idx]) {
            filtered = m_filters[idx]->push(raw); // 滑动平均压随机噪声
        }
        cur.value = filtered;
        cur.quality = modbus::Quality::Good;
        cur.lastUpdateMs = now;
        cur.errCount = 0;
    }

    // 告警评估: 仅 Good 数据参与(质量位过滤);迟滞状态机只输出翻转事件
    std::vector<edge::AlarmEvent> events;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        events = m_rules.evaluate(p.id, filtered, epochMillis());
    }
    for (const auto& ev : events) {
        onAlarmEvent(ev); // 锁外: 内部会发布 MQTT/EventBus,避免持锁做 IO
    }
    return true;
}

void Gateway::telemetryLoop() {
    ES_LOGI("gw", "遥测线程启动(周期 %u ms)", m_telemetryPeriodMs);
    std::unique_lock<std::mutex> lk(m_cvMutex);
    while (!m_stopTelem.load()) {
        // wait_for 返回 false = 周期到(正常聚合);true = 被唤醒且停止标志置位
        if (!m_telemCv.wait_for(lk, std::chrono::milliseconds(m_telemetryPeriodMs),
                                [this] { return m_stopTelem.load(); })) {
            const std::string payload = buildTelemetryJson();
            if (m_mqttEnabled && m_mqtt) {
                std::string err;
                if (!m_mqtt->publish(m_topicPrefix + "/telemetry", payload,
                                     m_mqttQos, false, &err)) {
                    ES_LOGW("gw", "遥测发布失败: %s", err.c_str());
                }
            }
            appendJsonl(payload); // JSONL 落盘(遥测审计/离线分析)
        }
    }
    ES_LOGI("gw", "遥测线程退出");
}

// ---------------------------------------------------------------------------
// 事件与状态
// ---------------------------------------------------------------------------

void Gateway::onAlarmEvent(const edge::AlarmEvent& ev) {
    Json j;
    j["device"] = m_deviceId;
    j["ts"] = nowIso8601();
    j["type"] = "alarm";
    j["pointId"] = ev.pointId;
    j["level"] = ev.level;
    j["value"] = ev.value;
    j["threshold"] = ev.threshold;
    j["active"] = ev.active;
    const std::string payload = j.dump();
    m_bus.publish("event", payload); // 进程内: Qt 告警列表 / 未来扩展订阅者
    if (m_mqttEnabled && m_mqtt) {
        std::string err;
        (void)m_mqtt->publish(m_topicPrefix + "/event", payload, m_mqttQos, false, &err);
    }
    ES_LOGI("gw", "告警[%s] %s %s: value=%.3f threshold=%.3f",
            ev.pointId.c_str(), ev.level.c_str(), ev.active ? "进入" : "恢复",
            ev.value, ev.threshold);
}

void Gateway::onMqttState(bool connected, const std::string& reason) {
    ES_LOGI("gw", "MQTT %s: %s", connected ? "已连接" : "断开", reason.c_str());
    if (connected) {
        // 连接(含重连)成功 → 立即发 retained status:
        // 云端新订阅者/控制台立刻拿到网关当前状态,无需等下一个遥测周期
        publishStatus(StateMachine::stateName(m_sm.state()));
    }
}

void Gateway::onMqttMessage(const MqttMessage& msg) {
    // 只处理本网关命令主题;语义与 CmdServer 完全一致(共用 handleCommand)
    if (msg.topic != m_topicPrefix + "/cmd") {
        return;
    }
    const std::string ack = handleCommand(msg.payload);
    if (!ack.empty() && m_mqtt) {
        std::string err;
        (void)m_mqtt->publish(m_topicPrefix + "/ack", ack, m_mqttQos, false, &err);
    }
}

void Gateway::onStateChanged(State from, State to, Event ev) {
    ES_LOGI("gw", "状态机: %s --%s--> %s",
            StateMachine::stateName(from).c_str(),
            StateMachine::eventName(ev).c_str(),
            StateMachine::stateName(to).c_str());
    if (to == State::Running) {
        // 恢复后清告警状态: 下一轮采样按当前值重新评估(旧告警不延续)
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rules.reset();
    }
    publishStatus(StateMachine::stateName(to)); // 状态机转移时发 status
}

void Gateway::publishStatus(const std::string& stateName) {
    const std::string payload = buildStatusJson(stateName);
    if (m_mqttEnabled && m_mqtt) {
        std::string err;
        if (!m_mqtt->publish(m_topicPrefix + "/status", payload,
                             m_mqttQos, m_mqttRetainStatus, &err)) {
            ES_LOGW("gw", "status 发布失败: %s", err.c_str());
        }
    }
    m_bus.publish("status", payload); // 进程内状态广播(Qt 状态栏等)
    ES_LOGI("gw", "发布 status: %s", payload.c_str());
}

// ---------------------------------------------------------------------------
// 命令处理(§12 命令表;CmdServer 与 MQTT cmd 共用)
// ---------------------------------------------------------------------------

std::string Gateway::handleCommand(const std::string& jsonCmd) {
    Json ack;
    ack["ts"] = nowIso8601();
    std::string parseErr;
    const Json j = Json::parse(jsonCmd, &parseErr);
    if (!parseErr.empty() || j.type() != Json::Type::Object) {
        ack["cmd"] = parseErr.empty() ? "(malformed)" : "(parse error)";
        ack["ok"] = false;
        ack["err"] = parseErr.empty() ? "命令必须是 JSON 对象" : parseErr;
        return ack.dump();
    }
    const std::string cmd = j.get("cmd", Json("")).asString("");
    ack["cmd"] = cmd;

    if (cmd == "snapshot") {
        ack["ok"] = true;
        ack["data"] = snapshot();
    } else if (cmd == "set_period") {
        const std::string id = j.get("id", Json("")).asString("");
        const int64_t periodMs = j.get("periodMs", Json((int64_t)0)).asInt(0);
        std::string err;
        const bool ok = setPeriodMs(id, (uint32_t)periodMs, &err);
        ack["ok"] = ok;
        if (ok) {
            ack["id"] = id;
            ack["periodMs"] = periodMs;
        } else {
            ack["err"] = err;
        }
    } else if (cmd == "write_reg") {
        const std::string id = j.get("id", Json("")).asString("");
        const int64_t value = j.get("value", Json((int64_t)0)).asInt(0);
        std::string err;
        const bool ok = writeRegister(id, (uint16_t)value, &err);
        ack["ok"] = ok;
        if (ok) {
            ack["id"] = id;
            ack["value"] = value;
        } else {
            ack["err"] = err;
        }
    } else if (cmd == "inject_fault") {
        const std::string fault = j.get("fault", Json("")).asString("");
        std::string err;
        const bool ok = injectFault(fault, &err);
        ack["ok"] = ok;
        if (ok) {
            ack["fault"] = fault;
        } else {
            ack["err"] = err;
        }
    } else if (cmd == "recover") {
        recover();
        ack["ok"] = true;
    } else if (cmd == "ping") {
        ack["ok"] = true;
        ack["state"] = StateMachine::stateName(m_sm.state());
    } else {
        ack["ok"] = false;
        ack["err"] = "未知命令: " + cmd +
                     "(支持 snapshot/set_period/write_reg/inject_fault/recover/ping)";
    }
    return ack.dump();
}

bool Gateway::setPeriodMs(const std::string& id, uint32_t periodMs, std::string* err) {
    if (periodMs == 0) {
        if (err) *err = "periodMs 必须 > 0";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& p : m_points) {
        if (p.id != id) {
            continue;
        }
        p.pollPeriodMs = periodMs;
        // 主站无"按点改周期"接口 → 重新 configure 同步内部点表。
        // 代价(可接受并已注释): 主站统计清零 + 全点立即到期(重新开始调度)
        std::string cerr;
        if (!m_master->configure(m_points, &cerr)) {
            if (err) *err = "同步主站点表失败: " + cerr;
            return false;
        }
        ES_LOGI("gw", "set_period: %s → %u ms", id.c_str(), periodMs);
        return true;
    }
    if (err) *err = "点不存在: " + id;
    return false;
}

bool Gateway::writeRegister(const std::string& id, uint16_t value, std::string* err) {
    modbus::ModbusPoint p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& pt : m_points) {
            if (pt.id == id) {
                p = pt;
                break;
            }
        }
    }
    if (p.id.empty()) {
        if (err) *err = "点不存在: " + id;
        return false;
    }
    if (!p.writable) {
        if (err) *err = "点 " + id + " 不可写(仅 func=06 写点)";
        return false;
    }
    std::string werr;
    const uint64_t now = steadyMillis();
    {
        // 串口事务串行化(与采集线程互斥);write_reg 可能等待一个读事务完成
        std::lock_guard<std::mutex> slk(m_serialMutex);
        if (!m_master->writeRegister(p, value, &werr)) {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& pt : m_points) {
                if (pt.id == id) {
                    pt.quality = modbus::Quality::Bad;
                    ++pt.errCount;
                    break;
                }
            }
            if (err) *err = "写寄存器失败: " + werr;
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& pt : m_points) {
            if (pt.id == id) {
                // 06 写单寄存器: 寄存器值即物理值(无标度),写成功即更新
                pt.rawValue = value;
                pt.value = value;
                pt.quality = modbus::Quality::Good;
                pt.lastUpdateMs = now;
                pt.errCount = 0;
                break;
            }
        }
    }
    ES_LOGI("gw", "write_reg: %s = %u (addr=0x%04X)", id.c_str(), value, p.startAddr);
    return true;
}

bool Gateway::injectFault(const std::string& fault, std::string* err) {
#if defined(ES_BUILD_SIM)
    if (!m_slave) {
        if (err) *err = "仅 pty-sim 模拟链路支持故障注入(当前为真实串口)";
        return false;
    }
    if (fault != "none" && fault != "crc" && fault != "no_response" &&
        fault != "exception" && fault != "wrong_slave") {
        if (err) *err = "未知故障模式: " + fault +
                        "(支持 none/crc/no_response/exception/wrong_slave)";
        return false;
    }
    m_slave->injectFault(fault); // 原子切换从站应答行为,立即生效
    ES_LOGW("gw", "故障注入: %s(观察主站 Stats 变化)", fault.c_str());
    return true;
#else
    (void)fault;
    if (err) *err = "未编译模拟链路(ES_BUILD_SIM=OFF)";
    return false;
#endif
}

void Gateway::recover() {
#if defined(ES_BUILD_SIM)
    if (m_slave) {
        m_slave->injectFault("none"); // 清除故障注入
    }
#endif
    m_consecutiveFailRounds = 0; // 复位失败计数,避免立即再次判定 Fault
    if (m_sm.state() == State::Fault) {
        m_sm.dispatch(Event::SerialRecovered); // 手动拉回 Running
    }
    ES_LOGI("gw", "recover: 已清除故障注入并复位链路判定");
}

// ---------------------------------------------------------------------------
// 报文构建
// ---------------------------------------------------------------------------

std::string Gateway::buildTelemetryJson() {
    Json j;
    j["device"] = m_deviceId;
    j["ts"] = nowIso8601();
    Json pts;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint64_t now = steadyMillis();
        for (const auto& p : m_points) {
            Json pj;
            pj["id"] = p.id;
            pj["name"] = p.name;
            pj["value"] = p.value;
            pj["unit"] = p.unit;
            pj["quality"] = qualityString(p.quality, now, p.pollPeriodMs, p.lastUpdateMs);
            pts.pushBack(pj);
        }
    }
    j["points"] = pts;
    Json st;
    if (m_master) {
        const modbus::ModbusMaster::Stats ms = m_master->stats();
        st["tx"] = (int64_t)ms.txFrames;
        st["rx"] = (int64_t)ms.rxFrames;
        st["timeouts"] = (int64_t)ms.timeouts;
        st["crcErrors"] = (int64_t)ms.crcErrors;
        st["exceptions"] = (int64_t)ms.exceptions;
    }
    j["stats"] = st;
    return j.dump();
}

std::string Gateway::buildStatusJson(const std::string& stateName) {
    Json j;
    j["device"] = m_deviceId;
    j["ts"] = nowIso8601();
    j["state"] = stateName;
    j["uptimeMs"] = (int64_t)(steadyMillis() - m_startedMs);
    j["mqttConnected"] = m_mqttEnabled && m_mqtt && m_mqtt->isConnected();
    return j.dump();
}

std::string Gateway::qualityString(modbus::Quality q, uint64_t nowMs,
                                   uint32_t pollMs, uint64_t lastMs) {
    if (q == modbus::Quality::Bad) {
        return "bad";
    }
    // "软过期"只读判定(不写回点表): Good 但超过 3 个轮询周期未更新 → stale。
    // 与 RuleEngine 的配合: stale 仍按 Good 处理(值未失效,只是老),
    // 若产品要求"过期不参与告警",可在 processPoint 中叠加质量位过滤。
    if (q == modbus::Quality::Good && pollMs > 0 && nowMs - lastMs > 3ull * pollMs) {
        return "stale";
    }
    return "good";
}

Json Gateway::snapshot() const {
    Json snap;
    snap["device"] = m_deviceId;
    snap["name"] = m_deviceName;
    snap["state"] = StateMachine::stateName(m_sm.state());
    snap["ts"] = nowIso8601();
    snap["uptimeMs"] = (int64_t)(steadyMillis() - m_startedMs);
    snap["mqttEnabled"] = m_mqttEnabled;
    snap["mqttConnected"] = m_mqttEnabled && m_mqtt && m_mqtt->isConnected();
    Json pts;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint64_t now = steadyMillis();
        for (const auto& p : m_points) {
            Json pj;
            pj["id"] = p.id;
            pj["name"] = p.name;
            pj["value"] = p.value;
            pj["rawValue"] = p.rawValue;
            pj["unit"] = p.unit;
            pj["quality"] = qualityString(p.quality, now, p.pollPeriodMs, p.lastUpdateMs);
            pj["lastUpdateMs"] = (int64_t)p.lastUpdateMs;
            pj["errCount"] = (int64_t)p.errCount;
            pj["pollPeriodMs"] = (int64_t)p.pollPeriodMs;
            pj["writable"] = p.writable;
            pts.pushBack(pj);
        }
    }
    snap["points"] = pts;
    Json st;
    if (m_master) {
        const modbus::ModbusMaster::Stats ms = m_master->stats();
        st["tx"] = (int64_t)ms.txFrames;
        st["rx"] = (int64_t)ms.rxFrames;
        st["timeouts"] = (int64_t)ms.timeouts;
        st["crcErrors"] = (int64_t)ms.crcErrors;
        st["exceptions"] = (int64_t)ms.exceptions;
    }
    if (m_mqtt) {
        const MqttStats mst = m_mqtt->stats();
        st["mqttSent"] = (int64_t)mst.sentPackets;
        st["mqttRecv"] = (int64_t)mst.recvPackets;
        st["mqttReconnects"] = (int64_t)mst.reconnectCount;
        st["mqttPublishes"] = (int64_t)mst.publishesSent;
    }
    snap["stats"] = st;
    return snap;
}

std::string Gateway::snapshotJson() const {
    return snapshot().dump();
}

void Gateway::appendJsonl(const std::string& line) {
    if (!m_jsonlEnabled || !m_jsonlFile) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_jsonlMutex);
    std::fwrite(line.data(), 1, line.size(), m_jsonlFile);
    std::fwrite("\n", 1, 1, m_jsonlFile);
    std::fflush(m_jsonlFile); // 逐条 flush: 断电/崩溃最多丢最后一条,审计语义
}

} // namespace es
