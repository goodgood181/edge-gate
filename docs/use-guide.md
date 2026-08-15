# EdgeGate 分步骤使用说明与原理解释

> 适用读者:拿到本项目后想跑起来、并想彻底讲清原理的人(自学 / 快速上手)。
> 环境前提:一台 x86 Linux(或 WSL Ubuntu 24.04),联网安装工具。

---

# 第一部分 分步骤使用说明

## 1. 环境准备(一次性的)

```bash
sudo apt update
sudo apt install -y g++ cmake make git mosquitto mosquitto-clients netcat-openbsd
# 可选(交叉编译到 IMX6ULL 时需要):
sudo apt install -y g++-arm-linux-gnueabihf
# 可选(Qt GUI 需要):
sudo apt install -y qtbase5-dev
```

验证:`g++ --version`(≥ 9)、`cmake --version`(≥ 3.16;≥ 3.20 才能用 `ctest --test-dir`,低版本用 `cd build && ctest`)、`mosquitto -h`、`nc -h`。

## 2. 获取代码并构建

```bash
cd edge-gate                      # 项目根目录
./scripts/build.sh                # 一键: cmake 配置(Release)→ 编译 → ctest
```

等价手动步骤(了解每步在干什么):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 1. 生成构建系统
cmake --build build -j4                          # 2. 编译(静态库 es_core/es_modbus/es_net + 可执行 edge-gate + 测试)
ctest --test-dir build --output-on-failure       # 3. 跑全部测试,预期 11/11 Passed(CMake ≥ 3.20;低版本: cd build && ctest --output-on-failure)
```

产物说明:
- `build/edge-gate` —— 网关主程序
- `build/tests/test_*` —— 11 个测试可执行文件
- `build/libes_*.a` —— 三个静态库

常用构建开关(CMake 选项):
| 选项 | 默认 | 作用 |
|---|---|---|
| `-DES_BUILD_TESTS=OFF` | ON | 关测试(交叉编译时自动 OFF) |
| `-DES_BUILD_QT_GUI=ON` | OFF | 开 Qt GUI(需已装 qtbase5-dev) |
| `-DES_BUILD_SIM=OFF` | ON | 关掉 PTY 虚拟串口与软件从站(真机部署可关) |

## 3. 一键闭环演示(核心体验,约 5 分钟)

**前置知识**:默认配置 `config/edge-gate.json` 里 `serial.device = "pty-sim"`——网关启动时会自动创建一对虚拟串口(openpty),主站连 master 端、软件从站连 slave 端,模拟一台挂在 RS485 总线上的 Modbus 仪表。

**第 1 步:启动 MQTT 代理(broker)**

```bash
mosquitto -d                      # 后台运行,默认监听 127.0.0.1:1883
# 或: sudo systemctl start mosquitto
```

**第 2 步:启动网关**

```bash
./build/edge-gate --config config/edge-gate.json
```

预期日志(节选):
```
[INFO] [gw] 模拟从站已启动: slaveId=1 registers=64
[INFO] [gw] 串口就绪: device=pty-sim baud=9600 pty-sim=yes
[INFO] [gw] MQTT 客户端启动: 127.0.0.1:1883 clientId=edge-gate-01
[INFO] [gw] 网关启动完成: edge-gate-01 (state=Running)
[mqtt] connected to 127.0.0.1:1883 ...
```

**第 3 步:观察遥测(另开一个终端)**

```bash
mosquitto_sub -t 'edge-gate/edge-gate-01/#' -v -q 1
```

每秒收到一条 telemetry(JSON),形如:
```json
{"device":"edge-gate-01","points":[
  {"id":"temp1","name":"1号炉温度","quality":"good","unit":"C","value":30},
  {"id":"press1","name":"主管道压力","quality":"good","unit":"MPa","value":3},
  {"id":"flow1","name":"冷却水流量","quality":"good","unit":"m3/h","value":20}
],"stats":{"crcErrors":0,"exceptions":0,"rx":6,"timeouts":0,"tx":6},
 "ts":"2026-08-14T13:31:35.656+08:00"}
```
观察点:`tx`/`rx` 每周期递增且相等、`crcErrors/timeouts/exceptions` 全 0,说明 RS485 轮询链路零错误。

**第 4 步:用命令通道远程运维(再开一个终端)**

```bash
printf '{"cmd":"snapshot"}\n' | nc 127.0.0.1 19000
printf '{"cmd":"set_period","id":"temp1","periodMs":200}\n' | nc 127.0.0.1 19000
printf '{"cmd":"ping"}\n' | nc 127.0.0.1 19000
```
`snapshot` 返回全点表实时值 + 质量 + 统计;`set_period` 动态调整某个点的轮询周期(不重启生效,内部重新加载主站点表,**副作用是主站统计计数清零**——这点值得注意)。

**第 5 步:故障注入(演示 30 秒出效果)**

```bash
printf '{"cmd":"inject_fault","fault":"crc"}\n'        | nc 127.0.0.1 19000
printf '{"cmd":"inject_fault","fault":"no_response"}\n'| nc 127.0.0.1 19000
printf '{"cmd":"inject_fault","fault":"exception"}\n'  | nc 127.0.0.1 19000
printf '{"cmd":"inject_fault","fault":"wrong_slave"}\n'| nc 127.0.0.1 19000
printf '{"cmd":"inject_fault","fault":"none"}\n'       | nc 127.0.0.1 19000
```
每条注入后等 2 秒再看 `mosquitto_sub`,可观察到:`crc` / `no_response` / `exception` 分别使 `stats` 中 `crcErrors` / `timeouts` / `exceptions` 递增,点表质量变 `bad`;`wrong_slave`(从站用错误地址应答)不递增任何错误计数,表现为 `rx` +1 且对应点质量变 `bad`;注入 `none` 后链路自动恢复。这演示了:协议栈对错误的分分类统计能力 + 自动恢复能力。

**第 6 步:优雅停止**

按一次 `Ctrl-C` → 观察日志按"采集→遥测→MQTT→命令服务器→从站"顺序退出;连按两次强制退出(兜底)。

一键脚本:`./scripts/run-demo.sh`(自动起 broker → 起网关 → 打印日志;订阅演示仍需手动 mosquitto_sub)。

## 4. 交互式 CLI(不带 --daemon 启动即进入)

```bash
./build/edge-gate --config config/edge-gate.json
```
stdin 为终端时自动进入交互模式,命令:`help`(帮助)/ `status`(点表与统计)/ `watch`(每秒刷新实时表格,**任意键**退出)/ `set_period <id> <ms>` / `write_reg <id> <value>` / `quit`。

## 5. 配置文件逐段解读

`config/edge-gate.json` 结构:

| 段 | 关键字段 | 说明 |
|---|---|---|
| `device` | `id` | 设备标识,同时是 MQTT clientId 与 topic 前缀的一部分 |
| `log` | `level/file/console` | 日志级别与落盘(空=仅控制台) |
| `serial` | `device` | **`pty-sim` = 演示模式;`/dev/ttyUSB0`、`/dev/ttymxc2` = 真机模式** |
| `serial` | `baud/dataBits/parity/stopBits` | 9600/8/N/1 是 Modbus RTU 最常见配置 |
| `slaveSim` | `enabled/slaveId/registerCount` | 演示用软件从站(真机部署设为 `enabled=false`) |
| `points` | `id/name/slaveId/func/startAddr/count` | 采集点表:地址映射 + 功能码(03 保持寄存器可读写 / 04 输入寄存器只读 / 06 写单寄存器) |
| `points` | `dataType/is32Bit/scale/offset`(及可选 `bigEndian`) | 数据解析:u16/i16/u32/i32/f32,16/32 位组合,`value = raw×scale + offset`;bigEndian 默认 true,可省 |
| `points` | `pollPeriodMs` | 该点轮询周期(每个点可不同) |
| `points` | `highAlarm/lowAlarm/highAlarmEnabled/lowAlarmEnabled/hysteresis` | 告警阈值与迟滞(见原理 §5.2) |
| `mqtt` | `host/port/clientId/keepaliveSec/cleanSession` | broker 连接参数 |
| `mqtt` | `topicPrefix` | 所有主题前缀: `{prefix}/telemetry`、`{prefix}/event`、`{prefix}/cmd`、`{prefix}/ack`、`{prefix}/status` |
| `mqtt` | `telemetryPeriodMs/qos/retainStatus` | 遥测聚合周期(默认 1s)、QoS、status 是否 retained |
| `cmdServer` | `enabled/port` | 命令服务器(默认绑定 127.0.0.1,详见原理 §4.6) |
| `jsonl` | `enabled/path` | 本地 JSONL 落盘(数据审计 / 断线补传的底稿) |

**切到真机**(`config/edge-gate-arm.json` 已给好示例):
```json
"serial": { "device": "/dev/ttymxc2", ... },
"slaveSim": { "enabled": false, ... },
"mqtt": { "host": "192.168.1.100", ... }
```

## 6. 交叉编译到 IMX6ULL 并部署

```bash
# 1) 交叉配置与构建(工具链文件已提供)
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j4
# 产物: build-arm/edge-gate(ARM 二进制,file 命令可验证架构)

# 2) 部署(推荐直接用脚本,内部路径已与 systemd 单元对齐)
./scripts/deploy-arm.sh          # scp 二进制 + ARM 配置 + systemd 单元 → ssh 安装启动

# 3) 板卡上(等效手动步骤 —— 路径必须与 edge-gate.service 的 ExecStart 一致!)
sudo mkdir -p /usr/local/bin /etc/edge-gate /var/lib/edge-gate
sudo cp build-arm/edge-gate /usr/local/bin/
sudo cp config/edge-gate-arm.json /etc/edge-gate/edge-gate.json
sudo cp scripts/edge-gate.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now edge-gate
journalctl -u edge-gate -f      # 看网关日志
```
> 注意:systemd 单元的 ExecStart 为 `/usr/local/bin/edge-gate --config /etc/edge-gate/edge-gate.json --daemon`,WorkingDirectory 为 `/var/lib/edge-gate`(JSONL 落盘目录)——手动部署路径必须与此一致,否则服务起不来。

**真机注意清单**(重要):
1. 串口设备名按板卡改(`IMX6ULL` 常见 `/dev/ttymxc2`、USB 转串口 `/dev/ttyUSB0`);权限不足时把用户加进 `dialout` 组。
2. RS485 半双工方向控制:代码尝试 `TIOCSRS485` ioctl(内核自动切换收发方向),失败仅告警——真机需确认驱动支持。
3. 3.5T 帧间隔按实际波特率自动计算(9600→≈4ms),高波特率(≥115200)时 3.5T < 1ms,超出 poll 毫秒精度,建议 ≤38400 波特(详见原理 §2.3 与 §6 局限)。
4. 交叉编译默认关闭测试(ES_BUILD_TESTS=OFF),ARM 上如需自检可开 `ES_BUILD_SIM=ON` 做 PTY 回环自检。

## 7.(可选)Qt GUI

```bash
sudo apt install -y qtbase5-dev
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release -DES_BUILD_QT_GUI=ON
cmake --build build-qt -j4
./build-qt/edge-gate --config config/edge-gate.json   # 启动后出现主窗口:实时曲线 + 点表 + 告警列表
```
> 注意:Qt GUI 模块未纳入默认测试,首次使用建议在 x86 上自测通过后再谈。

## 8. 单元测试清单(ctest 11/11 各测什么)

| 测试 | 类型 | 覆盖点 |
|---|---|---|
| test_json | 单元 | 解析/回环/错误定位/转义/嵌套 |
| test_ring_buffer | 单元 | 顺序/满/空/阻塞超时 |
| test_state_machine | 单元 | 默认转移表/非法转移/历史记录 |
| test_modbus_crc | 单元 | CRC16 已知向量(线路字节序) |
| test_modbus_rtu | 单元 | 编解码回环/半包/异常帧/帧长推导/3.5T 计算 |
| test_modbus_link | **集成(PTY 真实字节流)** | 主站↔从站全链路:03/04/06/0x10、标度变换、4 类故障注入、**流式字节回归(F1)** |
| test_rule_engine | 单元 | 迟滞进入/恢复边界 |
| test_signal_filter | 单元 | 滑动平均数值/窗口 |
| test_mqtt_packets | 单元 | 变长编码 7 边界/CONNECT 字节比对/半包/非法报文 |
| test_mqtt_client | **集成(fake broker)** | 握手/订阅/发布 QoS0+1/PINGREQ/断线重连 |
| test_main | 占位 | 框架自检(0 用例) |

---

# 第二部分 原理解释

## 1. 整体数据流(先看这一张图)

```
真实仪表 / 软件从站(寄存器堆)
        │  Modbus RTU 帧(RS485 / PTY)
        ▼
┌─ 采集线程 ─────────────────────────────────────────┐
│ 轮询调度(duePoints 按周期)→ 事务(清缓冲→发→收→CRC→分类) │
│ → 标度变换(convertRaw)→ 滤波(SignalFilter)            │
│ → 告警规则(RuleEngine,仅翻转上抛)→ 更新点表             │
└──────────────────────────────────────────────────┘
        │ 点表(值/质量/时间戳)
        ▼
┌─ 遥测线程 ─────────────────────────────────────────┐
│ 每 telemetryPeriodMs 聚合全部点 → JSON → MQTT publish │
│ → 同时写 JSONL 本地落盘                              │
└──────────────────────────────────────────────────┘
        │
        ▼
Mosquitto Broker ← 订阅端(mosquitto_sub / 上位机)

反向链路: TCP 命令服务器(nc)→ snapshot/set_period/write_reg/inject_fault → 点表/寄存器
```

一句话:现场数据从 Modbus 寄存器出发,经"协议栈→边缘处理→上云"三段,全程 JSON 化、可审计、可控制。

## 2. Modbus RTU 协议原理

### 2.1 帧结构
RTU 帧无帧头帧尾标记,靠**字符间隔**定界:
```
[从站地址 1B][功能码 1B][数据 N B][CRC16 2B,低字节在前]
```
- 地址:1..247(0 为广播,本项目不支持)
- 功能码:03 读保持寄存器(可读写区)、04 读输入寄存器(只读区,传感器常用)、06 写单寄存器、0x10 写多寄存器
- CRC16-Modbus:多项式 0x8005 的反射形式(0xA001),初值 0xFFFF,结果低字节在前。位运算实现,`01 03 00 00 00 0A` → `0xCDC5`(线上 `C5 CD`)

### 2.2 3.5T 静默间隔(本项目最值得深挖的点之一)
- 串口没有"包"概念,只有字节流;Modbus 规定**帧内相邻字节间隔 < 3.5 个字符时间,帧间静默 ≥ 3.5T**,以此切帧。
- 1 个字符 = 起始 1 + 数据 8 + 校验 1 + 停止 1 = **11 bit**;9600 波特下 1 字符 ≈ 1.146ms,3.5T ≈ 4.01ms。
- 主站侧用法:收应答时,帧内字节间隔超过 3.5T 即判"帧不完整";首字节等待 = 3.5T + 1s 兜底(从站掉线快速失败)。
- 从站侧用法:轮询超时(静默 ≥ 3.5T)即判定一帧结束。**锚点必须是"最后收到的字节"**——本项目审稿时曾发现从站误用"帧首字节"计时,真机上 9600 波特 8 字节帧历时 ≈9.2ms > 4ms,会在第 4~5 字节处误切帧导致整帧 CRC 失败、链路全断;而 PTY 字节瞬时到齐会掩盖该缺陷。已修复并新增"逐字节注入"回归测试锁定(可讲这个"PTY 掩盖真机问题"的第一手教训)。
- 帧长推导技巧:读类请求固定 8 字节(偶数)、应答 5+2N(奇数);写多请求 9+2N(奇数)、应答 8 字节(偶数)——借**长度奇偶性**可在不知道收发方向时无歧义区分请求/应答。

### 2.3 事务管线与错误五分类
每笔事务:清缓冲(RS485 半双工方向切换纪律)→ 发请求 → 收齐应答 → CRC 校验 → 地址核对 → 异常应答识别(功能码 `0x80|原功能码`)→ 格式校验。错误统计口径(以代码为准):**超时 / CRC 错 / 异常应答**三类有独立计数(timeouts / crcErrors / exceptions);**错地址与长度不符**不设独立计数,计入 rx 并以 err 字符串区分,点表质量置 bad——这正是工业网关可运维性的体现。
**事务级互斥**:采集线程与命令线程(write_reg)并发时,必须串行化整笔"清缓冲→发→收",否则两笔请求的字节会在半双工总线上交错撕裂(锁粒度是整笔事务,最坏阻塞 ≈ 超时 1s,可接受)。

### 2.4 RS485
半双工总线:同一时刻只能一端发送。代码通过 `TIOCSRS485` ioctl 让内核自动控制收发方向(失效时静默降级为手动时序)。演示用的 PTY 没有真实电平,但**字节流时序、termios 配置、poll 超时全是真实的**。

## 3. MQTT 3.1.1 原理

### 3.1 报文结构
- 固定头 1 字节:类型(高 4 位)+ 标志位(低 4 位)
- **剩余长度变长编码**:每字节 7 位有效位 + 最高位为续延标志,最多 4 字节(127 → `7F`;128 → `80 01`;16383 → `FF 7F`;16384 → `80 80 01`)。为什么变长?小报文省字节,大报文仍可表达 256MB。
- 字符串统一 2 字节长度前缀 + UTF-8。
- CONNECT 标志位(OASIS 位序):bit0 保留恒 0、bit1 CleanSession、bit2 Will、bit3-4 WillQoS、bit5 WillRetain、bit6 Password、bit7 UserName。**开发中契约曾把位序写反,实现按规范对齐并修正契约**——与真实 broker 互通的前提。

### 3.2 QoS0/1 与"尽力至少一次"
- QoS0:发出去不管,最多一次。
- QoS1:带包 ID 发送 → 等 PUBACK;10s 未确认 → 同包 ID 置 DUP 位重发一次;再 10s 未确认 → 丢弃并告警。断线即清在途表(无持久化),是"连接存活期内的至少一次"——诚实声明这点,并说明 JSONL 落盘为 Roadmap 的断点续传打底。
- QoS2(PUBREC/PUBREL/PUBCOMP 状态机)不在范围;订阅 QoS≤1 时真实 broker 不会下发 QoS2。

### 3.3 keepalive 与 PINGRESP 看门狗
- 空闲超过 **0.7×keepalive** 发 PINGREQ(留 0.3 余量给调度与网络);broker 侧 1.5×keepalive 未收到任何报文即断连。
- **看门狗闭环**:发出 PINGREQ 后 1.5×keepalive 内无任何入站报文 → 判 broker 假死(连接不 FIN 不 RST 的网络黑洞),主动断开重连,避免"看起来在线、实际全丢"。任何入站报文都清除看门狗。

### 3.4 重连与订阅
- 指数退避 1s→30s 封顶 + 0~30% 抖动(抖动防多设备同时重连的"惊群")。
- cleanSession=true:每次(重)连接后按订阅表重建 SUBSCRIBE;false:信任 broker 会话。
- 断线期间入队的订阅/退订由重连后的订阅重建统一处理,队列里的旧副本跳过(避免重复 SUBSCRIBE)。

### 3.5 Topic 设计
`edge-gate/{deviceId}/` 前缀下五类:telemetry(遥测,QoS1)/ event(告警等事件)/ cmd(下行命令)/ ack(命令应答)/ status(状态机转移时发,retained 保留最后状态,新订阅者立刻拿到设备在线状态)。

### 3.6 为什么从零实现(要点)
- 理解到字节级(剩余长度、标志位、QoS 流程),而不是调库;
- 零外部依赖,交叉编译无包袱;
- 代价:只实现 QoS0/1、无 TLS——主动说清取舍。

## 4. 多线程架构原理

### 4.1 线程清单与职责
| 线程 | 职责 | 说明 |
|---|---|---|
| 采集线程 ×1 | 轮询调度所有点 | **单线程**:串口是单工瓶颈,单线程天然串行,无需锁;线程数 = 采集 1 + 遥测 1 + MQTT 1 + 命令 1 + 从站 1(演示) |
| 遥测线程 ×1 | 聚合点表 → MQTT + JSONL | 与采集解耦,采集抖动不丢遥测周期 |
| MQTT 网络线程 ×1 | 收发/心跳/重连(客户端内部) | 用户线程只入队,不碰 socket |
| 命令服务器线程 ×1 | poll 多路复用 ≤8 客户端 | 低频短报文,单线程足够 |
| 软件从站线程 ×1 | 应答 Modbus 请求(仅演示) | 独立于主站侧 |

### 4.2 环形缓冲:为什么双条件变量
满/空两个条件变量分别通知生产者/消费者,避免单条件变量的"惊群"与"丢失唤醒";push/pop 均带超时,永不无限阻塞;容量固定,天然背压。

### 4.3 事件总线:为什么快照后解锁回调
publish 时先持锁拷贝 listener 列表、释放锁再逐个回调——回调里再 publish/subscribe 不会死锁;状态机回调同理(锁内拷贝 action)。

### 4.4 状态机
五个状态(Init/Configuring/Running/Fault/Stopped)+ 默认转移表(ConfigLoaded→Configuring→Start→Running;Running+SerialError→Fault;Fault+SerialRecovered→Running)。为什么状态机而不是散落 if-else:非法转移显式拒绝、转移历史可审计(最近 64 条)、与 MQTT status 发布天然挂钩。

### 4.5 停止顺序:先停生产者,再停消费者
采集 → 遥测 → MQTT → 命令服务器 → 从站,全部 join 有界;停止标志 + 唤醒管道(写 1 字节打断 poll)保证线程必然退出;两次 Ctrl-C 兜底强杀。顺序错会怎样?先停消费者,生产者还在写,数据堆积或写阻塞。

### 4.6 命令服务器:为什么单线程 poll
低频短报文场景,每连接一线程带来线程创建/切换开销与共享状态锁;单线程 + 非阻塞 fd + poll(≤8 客户端)确定、省资源、天然串行。默认**只绑定 127.0.0.1**(命令通道无鉴权,绑 0.0.0.0 等于把写寄存器/注入故障的能力暴露给局域网);需远程运维时改绑并自行加鉴权——这是安全意识的加分点。

### 4.7 禁异常与 RAII
嵌入式工程常见约束:禁异常(错误一律 bool + err 出参,代码路径显式),fd/线程用 RAII 管理(析构 join、关 fd),`std::thread` 构造的资源耗尽异常作为已知边界注释说明。

## 5. 边缘计算原理

### 5.1 滑动平均
环形窗口 + 运行和,O(1) 更新;与 EMA(省内存但 α 与采样周期耦合)和中值滤波(抗脉冲但需排序)的取舍写进了注释。

### 5.2 迟滞告警状态机
进入告警需 `value ≥ highAlarm`(或 ≤ lowAlarm),恢复需 `value < highAlarm - hysteresis`(或 > lowAlarm + hysteresis)——迟滞带防抖(临界值附近不会反复翻转);只有状态翻转时才上抛事件,上云流量 = 事件数而不是采样数。

### 5.3 数据质量
Good(正常采集)/ Bad(最近一次事务失败)/ Stale(超时未更新),遥测 JSON 带 quality 字段,上位机可直接按质量过滤。

## 6. PTY 与闭环验证原理

- openpty 创建一对虚拟终端:master 端(主站)与 slave 端(从站)共享同一终端状态,**数据通路像管道但带完整终端语义**(termios 配置、poll、字节流全真实)。
- 为什么能"闭环":协议栈的组帧/拆帧/CRC/3.5T 超时/错误分类全部走真实代码路径,只差物理电平与真实波特率节流。
- 局限(要点):无真实波特率(3.5T 是按配置波特率注入的逻辑值)、poll 毫秒粒度(高波特率 3.5T<1ms 会超精度)、RS485 方向控制未真机验证。所以本项目配套了"逐字节注入"回归测试,专门在 x86 上模拟真实波特率的字节到达节奏。

## 7. 测试原理

- **fake broker 自举**:测试里用项目自己的 mqtt 编解码实现一个迷你 broker(CONNACK/SUBACK/PUBACK/主动断连),用"自己解析自己"验证客户端行为——dogfooding,同时反向验证编解码。
- **PTY 集成测试**:主站连 master、从站连 slave,真实字节流全链路(03/04/06/0x10、故障注入、标度变换)。
- **流式回归测试**:逐字节写入 + 1.2ms 间隔,复现 9600 波特时序,锁定 3.5T 切帧行为(F1 缺陷的回归护栏)。
- 测试分层:纯逻辑单元测试(不碰 IO)+ 集成测试(真实 IO 路径),被问"11/11 是单元测试吗"时如实拆解。

## 8. 项目亮点故事(两分钟版)

1. **3.5T 锚点缺陷**:PTY 演示全绿,但审稿发现真机必现的半帧切分缺陷——用"字节间隔"视角重查代码才发现。修复 + 流式回归测试。教训:仿真环境会掩盖时序类缺陷,真机前要按波特率逐项核对。
2. **功能码 0x16→0x10**:契约笔误,审稿轮抓出,修正 + 回归锁定。协议细节以标准为准,不以文档为准。
3. **CONNECT 标志位位序**:契约描述与 OASIS 规范不一致,实现按规范(否则连不上真实 broker),并回头修正契约。

---

# 第三部分 常见问题排查

> 实战排错记录(WSL + Qt GUI 环境),按现象查表。

## 9. 常见问题排查

### 9.1 Qt GUI 启动了但"看不到窗口"(任务栏只有小企鹅图标)

**现象**:`./build-qt/edge-gate` 日志正常(模拟从站/MQTT 都起来了),但桌面没看到窗口;Windows 任务栏出现一个小企鹅图标,悬停标题为 "[WARN: COPY MODE] EdgeGate 监控 — edge-gate-01"。

**原因**:WSLg 的 Linux GUI 窗口在 Windows 任务栏的默认图标就是企鹅(Tux),窗口本身是正常创建的——这不是程序问题,是显示环境的表现形式。

**关于 "[WARN: COPY MODE]" 前缀**:这是 **WSLg 自身的警告**(microsoft/wslg 已知行为),不是 EdgeGate 的问题。WSLg 窗口本应走 VAIL 模式(Windows 与 Linux 共享内存传画面,高效);VAIL 不可用时降级为 COPY MODE(RDP 逐像素拷贝,慢),并在窗口标题前加该前缀。窗口功能不受影响,只是渲染走慢路径。想恢复 VAIL:管理员 PowerShell 执行 `wsl --shutdown` 后重开 WSL(若仍复现,可 `wsl --update`)。

**排查步骤**:
```bash
# 1) 确认显示环境正常(应为 :0;能列出 /mnt/wslg 说明 WSLg 可用)
echo $DISPLAY
ls -d /mnt/wslg

# 2) 确认窗口真实存在(有输出 = 窗口活着)
xwininfo -root -tree | grep -i edge
#   输出示例: 0x600006 "EdgeGate 监控 — edge-gate-01" ...
```

**解决**:点击任务栏企鹅图标,或 `Alt+Tab` 切到 "EdgeGate 监控" 窗口。若点击后窗口内容不渲染(黑屏/不出现):
1. 管理员 PowerShell 执行 `wsl --update`,再 `wsl --shutdown`,重开终端重试;
2. 仍不行重启电脑(重置 WSLg 服务)。

**注意**:WSL 内 SSH 会话没有图形转发,窗口只会出现在启动它的图形会话里。

### 9.2 启动报 `bind: Address already in use`(端口被残留进程占用)

**现象**:第二次运行 `./build-qt/edge-gate` 立即失败,日志:`网关启动失败: bind: Address already in use`。

**原因**:上一次运行(尤其是 `&` 后台方式或没关窗口)的网关实例还活着,19000 命令端口被它占着。

**排查与解决**:
```bash
# 1) 确认占用者
ss -tlnp | grep 19000        # 会显示残留的 edge-gate 进程 PID
pgrep -af edge-gate

# 2) 清理残留实例
pkill -f 'edge-gate'

# 3) 确认端口已释放后再启动
ss -tlnp | grep 19000        # 无输出 = 已释放
./build-qt/edge-gate --config config/edge-gate.json
```

**预防**:后台方式演示完记得 `kill %1`;关闭 GUI 窗口即优雅停止网关(等价 Ctrl-C),不会留残留。

### 9.3 不要手动改 DISPLAY(默认 :0 就是对的)

WSLg 会自动注入 `DISPLAY=:0`。`export DISPLAY=$(hostname).local:0` 是 VcXsrv(Windows 侧第三方 X server)的用法,在 WSLg 环境下手动改反而会连不上显示服务器。只有 WSLg 不可用、改用 VcXsrv 时才需要。

**典型故障现象**(就是改过 DISPLAY 的会话):
```
qt.qpa.xcb: could not connect to display Tina.local:0
This application failed to start because no Qt platform plugin could be initialized.
Aborted (core dumped)
```

**修复**:
```bash
# 方法一: 当前终端把 DISPLAY 改回 WSLg 的地址
 export DISPLAY=:0

# 方法二: 干脆新开一个 WSL 终端(环境变量自动恢复干净)
# 验证: echo $DISPLAY 应输出 :0
```

**怎么区分两种显示服务器**:WSLg 内置的地址是 `:0`(Unix 域套接字,`/tmp/.X11-unix/X0`);`Tina.local:0` 是 VcXsrv 的 TCP 地址形式,需要 Windows 侧安装并启动 VcXsrv 才有效。没有 VcXsrv 时用 `Tina.local:0` 必然报 above 错误。

### 9.4 网关启动失败通用排查顺序

| 现象 | 先查 | 后查 |
|---|---|---|
| `bind: Address already in use` | 残留进程(9.2) | 端口被其他程序占用(改 cmdServer.port) |
| MQTT 连不上 | mosquitto 是否启动(`pgrep mosquitto`)| broker 地址/端口配置 |
| 从站无响应(真机) | 串口设备名/权限(dialout 组)| RS485 方向控制与波特率(见 §2.4) |
| 日志乱码/中文异常 | 终端编码 UTF-8 | — |
| Qt GUI 窗口正常但曲线区域没有折线(只有网格/图例) | 见 9.5(已修复,重新编译 build-qt) | — |

### 9.5 Qt GUI 曲线不绘制(QPainterPath 平台 bug,已修复)

**现象**:窗口、点表、状态栏全部正常,但曲线区域只有网格、刻度、图例,没有折线。

**定位过程**(最小重现三步):① 直接用 `QPainter + QPainterPath` 画到 QImage → 正常;② 在 CurveWidget 的 paintEvent 里加黄色 `drawLine` 对照线 → 画出来了(1170 像素);③ 同一 painter 下 `drawPath` 画曲线 → 0 像素。结论:**Qt 5.15 xcb 后端在 QWidget 上 `drawPath` 整条不渲染**(疑平台后端 bug),`drawLine` 正常。

**修复**:`src/ui/qt/curve_widget.cpp` 折线绘制由 `QPainterPath + drawPath` 改为**逐段 `drawLine`**(语义完全等价;300 点 × 6 序列 = 最多 1800 段/帧,500ms 刷新,开销可忽略)。修复后真屏验证:红 738 / 蓝 776 像素(修复前 0)。

**生效方式**:重新编译 `cmake --build build-qt -j4`,重启应用。

