# EdgeGate 架构设计文档

> 对应代码: `.cluster/edge-sense-20260814/project/edge-gate/`;接口契约: `api-contract.md`(唯一事实源)。
> 本文档描述分层架构、线程模型、数据流、状态机、Modbus/MQTT 协议细节、PTY 演示原理与交叉编译部署细节。

---

## 1. 总体架构(分层视图)

```
┌────────────────────────────────────────────────────────────────────┐
│ 应用层          main / CLI(可选 Qt GUI)                              │
│                 Gateway: 配置装配 · 主状态机 · 线程生命周期管理       │
│                 CmdServer(TCP 命令) · JSONL 本地落盘                 │
├────────────────────────────────────────────────────────────────────┤
│ 边缘计算层      SignalFilter(滑动平均滤波)   RuleEngine(阈值+迟滞告警)│
│                 Quality(Good/Bad/Stale)     EventBus(事件总线)      │
├────────────────────────────────────────────────────────────────────┤
│ 协议栈          es::modbus: CRC16 · RTU 编解码 · 主站 · 软件从站      │
│                 es::mqtt:  报文编解码 · 客户端(心跳/QoS1/退避重连)    │
├────────────────────────────────────────────────────────────────────┤
│ 串口 HAL        ISerialDevice(抽象接口)                              │
│                 PosixSerial(termios 真实串口)  PtyPair(虚拟串口对)   │
├────────────────────────────────────────────────────────────────────┤
│ 内核 / POSIX    termios 行规程 · PTY 驱动 · RS485 ioctl · socket     │
│                 poll · std::thread · mutex · condition_variable     │
└────────────────────────────────────────────────────────────────────┘
```

**分层原则**:

1. **协议栈与 IO 分离**: `es::modbus::modbus_rtu` / `es::mqtt::mqtt_packets` 是纯编解码,不碰任何 IO,可独立单元测试——协议正确性在这两层闭环;主站/客户端只做"会话与状态管理",字节编解码全部委托纯模块。
2. **面向接口编程**: 主站/从站只依赖 `ISerialDevice` 抽象,真实串口与 PTY 虚拟串口对上层无差别——这是 x86 上闭环验证能够成立的结构前提。
3. **静态库分层**: `es_core`(core+util)→ `es_modbus`(hal+modbus+edge)→ `es_net`(net),依赖单向、无环;上层仅依赖下层的公开头文件。
4. **零外部依赖**: core/hal/modbus/edge/net 仅使用 C++17 标准库 + POSIX;Qt 仅是可选 GUI 层,默认关闭。

---

## 2. 线程模型与生命周期

### 2.1 五类线程

| # | 线程 | 归属 | 职责 | 退出机制 |
|---|---|---|---|---|
| 1 | **采集线程** | Gateway | `ModbusMaster::duePoints()` → 到期点 `readPoint/writeRegister` → 滤波 → 标度变换 → 更新点表 → 规则引擎 → 事件总线发布 | 停止标志 + 条件变量/轮询节拍 |
| 2 | **遥测线程** | Gateway | 每 `telemetryPeriodMs` 聚合全部点 → MQTT publish telemetry + JSONL 落盘 | PeriodicTask: 置停止标志 + cv 通知 + join |
| 3 | **MQTT 网络线程** | MqttClient 内部 | 连接/握手/读循环/keepalive/QoS1 重发/退避重连;用户线程只"入队 + 唤醒管道" | 停止标志 → 唤醒管道 → join(有界) |
| 4 | **命令服务器线程** | CmdServer 内部 | 单线程 poll() 多路复用,处理 snapshot / set_period / write_reg / inject_fault / recover / ping | 停止标志 → 唤醒管道 → join(poll 上限 1s) |
| 5 | **模拟从站线程** | ModbusSlave 内部 | 按 3.5T 静默切帧解析请求并应答(仅 `slaveSim.enabled` 时存在) | 轮询周期内响应退出标志,stop() join,无阻塞读 |

main 线程负责: 参数解析 → 配置加载 → `Gateway::start()` → 前台 CLI(或 Qt GUI)。

### 2.2 停止顺序(优雅停机)

```
采集 → 遥测 → MQTT → CmdServer → 从站;全部 join;两次 Ctrl-C 强制退出兜底
```

设计意图:

- **先停生产者、后停消费者**: 采集线程先停,不再产生新事件;遥测线程随后停,不再聚合上行;MQTT 先断开上行通道(退出前尽力 DISCONNECT),再停命令通道;从站最后停,保证演示链路中的请求方全部退出后模拟从站才收工,避免任何一方在等待对端。
- **为什么两次 Ctrl-C 强制退出**: 所有线程都以协作方式退出且 join 有界,正常情况下一次 Ctrl-C 即可完成;第二次 Ctrl-C 作为异常兜底(如某线程意外阻塞),避免进程僵死。
- **协作退出的三个原则**(贯穿全部线程):
  1. 停止标志用 `std::atomic`(免锁读);
  2. 所有阻塞点(条件变量等待、poll、connect 等待、退避睡眠)必须可被"停止标志/唤醒管道"打断——**join 一定有界**;
  3. fd 的关闭只由拥有它的线程自己做,禁止跨线程 close(防止 fd 号复用后另一线程 poll 到错误对象的经典竞态)。

### 2.3 竞态防护清单

| 共享数据 | 保护方式 | 说明 |
|---|---|---|
| 点表 value/quality/errCount | mutex(ModbusMaster 内部 + 网关点表锁) | 采集线程写、遥测线程/命令线程读 |
| 发送队列(publish/subscribe) | mutex + self-pipe 唤醒 | 用户线程入队,网络线程独占消费 |
| 订阅表 | mutex | 用户线程写,网络线程重连后读 |
| 在途 QoS1 表 / 退避状态 / socket | 仅网络线程访问 | 通过"单线程拥有"消除锁 |
| 回调函数指针 | 拷贝后调用(cbMutex) | 防回调被并发替换造成数据竞争 |
| 统计计数 | std::atomic | 任意线程可安全读取 |
| 日志 | 单例内部 mutex,整行原子写 | 锁内格式化 + 落盘 |

---

## 3. 数据流(轮询 → 滤波 → 规则 → 遥测)

```
                    ┌──────────────────────────────────────────────┐
  采集线程            │  ModbusMaster 事务管线(每点)                   │
 ┌──────────┐        │  flush → encodeFrame → write(1s 超时)         │
 │duePoints │        │  → 收帧: 首字节 3.5T+1000ms / 后续字节 3.5T    │
 │(按周期到期)│───────▶│  → CRC 校验 → 地址/功能码/格式分类校验          │
 └──────────┘        │  失败: 计入 stats 并更新 quality=Bad           │
                     └──────────────────────────────────────────────┘
                                    │ raw 寄存器值(成功时)
                                    ▼
                          SignalFilter(滑动平均,窗口可配)
                                    ▼
                          convertRaw(标度变换):
                          16/32 位组合 · 符号扩展 · IEEE754 位搬移 · scale/offset
                                    ▼
                          更新点表: value / quality=Good / lastUpdateMs / errCount
                                    ▼
                          RuleEngine(高/低限 + 迟滞)
                          仅状态翻转(Normal↔Active)时产生 AlarmEvent
                                    ▼
                          EventBus.publish(point.* / alarm.*)
                                    │
        ┌───────────────────────────┼──────────────────────────────┐
        ▼                           ▼                               ▼
   遥测线程(1s)              MQTT 网络线程                 命令服务器/CLI
   聚合全部点 →              publish telemetry(QoS1)      snapshot / write_reg /
   JSONL 落盘 + MQTT         event(QoS1)/ status(retained) inject_fault / recover / ping
```

**关键语义**:

- **只发翻转事件**: 告警事件只在 Normal↔Active 翻转时产生,遥测周期 1s 而告警一天几个,上云流量差 4~5 个数量级。
- **质量位贯穿**: `quality != Good` 时不参与告警判定、遥测中标记为 bad,避免"坏数据触发假告警"。
- **写点也走轮询调度**: writable 点(功能码 06)同样按 pollPeriodMs 到期,由采集线程执行写事务。
- **JSONL 落盘**: 与 MQTT 平行的本地通道,便于离线回放/审计,也是断网数据恢复(见 Roadmap)的基础。

---

## 4. 主状态机(默认转移表)

```
         ConfigLoaded         Start
 Init ──────────────▶ Configuring ────────────▶ Running
                                                  │  Stop
                                                  ▼
        SerialRecovered                        Stopped
 Fault ───────────────▶ Running                 ▲
   ▲                                              │ Stop
   │ SerialError                                  │
   └──────────────── Running ────────────────────┘
   任意态 + Fatal ─────────────────────────────▶ Stopped
```

| from | event | to | 语义 |
|---|---|---|---|
| Init | ConfigLoaded | Configuring | 配置加载成功 |
| Configuring | Start | Running | 资源就绪,采集/遥测启动 |
| Running | Stop | Stopped | 用户主动停止(优雅停机流程) |
| Running | SerialError | Fault | 串口/链路致命错误(如设备消失) |
| Fault | SerialRecovered | Running | 链路恢复,重新进入运行 |
| Fault | Stop | Stopped | 故障中停止 |
| 任意态 | Fatal | Stopped | 不可恢复错误,直接进入停止 |

实现要点(es::core::StateMachine):

- 集中式转移表(声明式)而非散落 if-else: 5 状态 × 6 事件,新增事件不必翻遍调用点,非法转移直接返回 false 且状态不变;
- 保留最近 64 条 `(from, event, to)` 历史,排障时回看"怎么进 Fault 的";
- 回调时机: 先更新状态、再解锁、最后回调 `onTransition`,回调内可安全再 dispatch;
- 默认表可被 `addTransition` 增量覆盖(测试注入/扩展)。

---

## 5. Modbus RTU 协议细节

### 5.1 ADU 帧结构(无帧头/帧尾,靠时序定界)

```
请求(读保持寄存器 03):  [从站地址 1B][功能码 1B][起始地址 2B][寄存器数 2B][CRC16 2B]
应答(03/04):            [从站地址 1B][功能码 1B][字节数 1B][数据 2N B][CRC16 2B]
异常应答:                [从站地址 1B][功能码|0x80][异常码 1B][CRC16 2B]   ← 固定 5 字节
```

**完整 ADU 长度推导**(重点):

| 方向 | 长度 |
|---|---|
| 请求 03/04 | 8 字节固定(1+1+2+2+2) |
| 请求 06 | 8 字节固定(1+1+2+2+2) |
| 请求 16(写多) | 9 + N×2(1+1+2 起始+2 数量+1 字节数+2N 数据+2 CRC) |
| 应答 03/04 | 5 + N×2(1+1+1 字节数+2N 数据+2 CRC) |
| 应答 06 / 应答 16 | 8 字节(请求回显) |
| 异常应答 | 5 字节固定 |

**奇偶性消歧技巧**: 03/04 请求恒 8 字节(偶)、应答恒 5+2N(奇);16 请求恒 9+2N(奇)、应答恒 8(偶)。编解码器在不知道收发方向时,凭长度奇偶性即可无歧义区分请求与应答。

### 5.2 功能码与异常码

| 功能码 | 名称 | 语义 |
|---|---|---|
| 0x03 | 读保持寄存器 | 可读写区(PLC 保持区),本项目读路径之一 |
| 0x04 | 读输入寄存器 | 只读区(传感器/采集通道) |
| 0x06 | 写单个寄存器 | 写保持区单寄存器(应答为请求回显) |
| 0x10 | 写多个寄存器 | 批量写(Modbus 标准功能码;曾因契约笔误写过 0x16,审稿轮已修正并加回归测试) |

| 异常码 | 含义 | 典型触发 |
|---|---|---|
| 0x01 | Illegal Function | 未知功能码 |
| 0x02 | Illegal Data Address | 寄存器地址越界 |
| 0x03 | Illegal Data Value | 数量/值非法(如 0 个寄存器) |
| 0x04 | Slave Device Failure | 从站内部故障 |

异常应答 = 功能码最高位置 1(`0x80 | func`)+ 异常码。主站侧收到 5 字节异常应答会**提前收尾**,不必等满正常帧长。

### 5.3 CRC16-Modbus(重点)

- 多项式 `x^16 + x^15 + x^2 + 1` = 0x8005,按 LSB 优先反射后为 **0xA001**;
- 初始值 **0xFFFF**,无最终异或(与 CRC-16/XMODEM 等变体的关键差异);
- 线路字节序: **低字节先发**(先 CRC_Lo 后 CRC_Hi)——与多字节寄存器"高字节在前"的惯例相反;
- 本实现用位运算(逐 bit,8×len 次循环,零查表内存);查表法预计算 256 项余数表(512B ROM),每字节一次查表+异或,速度约快 8 倍,MCU 上无刷写成本时常用查表;
- 已知向量: `01 03 00 00 00 0A` → 0xCDC5(线路 `C5 CD`);`01 06 00 01 00 17` → 0x0498(线路 `98 04`)。

### 5.4 3.5T 静默间隔

- 串口无帧头/帧尾,RTU 靠"字符间隔"定帧界: **两帧之间静默 ≥ 3.5 个字符时间**才认为上一帧结束;小于 3.5T 的间隙视为帧内字符间隔,防止把慢速到达的字节拆成两帧。
- 1 字符 = 1 起始位 + 8 数据位 + 1 校验位 + 1 停止位 = **11 bit**(8N1 无校验时理论为 10 bit,Modbus 规范与主流实现按 11 bit 计,差异 <10%,对帧分隔判定无实质影响);
- `charTimeUs(baud) = 11 * 1e6 / baud`;9600 波特 → 1145.83µs/字符,3.5T ≈ **4010µs**;115200 → 95.49µs,3.5T ≈ 334µs。
- 实现落点: 软件从站以 `ceil(3.5T)` 为 poll 轮询粒度(9600 波特下 5ms),poll 超时返回即"静默 ≥3.5T"→ 判定帧结束,**不按字节数猜帧**,与真实从站行为一致;主站帧内相邻字节间隔超过 3.5T 即判"帧不完整"。

### 5.5 主站事务管线与错误分类

```
flush(tcflush, RS485 半双工方向切换纪律)
  → encodeFrame(组帧 + CRC)
  → write(1s 发送超时)
  → 收帧: 首字节等待 = 3.5T + 1000ms 兜底(从站掉线时快速失败)
          后续字节等待 = 3.5T(超过即"帧不完整")
          5 字节异常应答提前收尾
  → CRC 校验 → 从站地址核对 → 异常应答识别 → 功能码/格式校验
```

错误五分类(Stats 五字段): `timeouts`(首字节超时/帧不完整)、`crcErrors`、`exceptions`(异常应答)、地址不匹配与格式不符(计入 rxFrames,以 err 字符串区分)。**轮询调度**: `duePoints()` 按各点 pollPeriodMs 返回到期索引并立即记时(防下轮重复),不同周期(500ms 温度 / 2000ms 泵速)天然交错,无需定时器。

### 5.6 软件从站与故障注入

- 行为还原: 3.5T 静默切帧 → CRC 错帧/地址不符帧**静默丢弃**(真实从站行为)→ 合法请求按功能码应答(03/04 读堆、06/16 写堆,越界回 0x02、非法数量回 0x03、未知功能码回 0x01);
- 故障注入 5 模式,分别命中主站不同统计:
  | 注入 | 主站可见效果 |
  |---|---|
  | `crc` | crcErrors 增长 |
  | `no_response` | timeouts 增长 |
  | `exception` | exceptions 增长(回 0x02) |
  | `wrong_slave` | 地址不匹配(收到 0x02 从站、期望 0x01) |
  | `none` | 恢复,统计停止增长 |
- `requestCount` 只统计"CRC 通过且地址匹配"的合法请求。

---

## 6. MQTT 3.1.1 设计

### 6.1 报文编解码要点

- **固定头**: [类型+标志 1B][剩余长度 1..4B][可变头+载荷];类型在字节高 4 位(1..14,0/15 保留),标志在低 4 位——PUBLISH 承载 dup/qos/retain,其余报文多为 0000,而 PUBREL/SUBSCRIBE/UNSUBSCRIBE 固定 0010,解码端校验标志即可识别非法报文;
- **剩余长度变长编码**: 每字节低 7 位为数据、最高位为续延标志,128 进制小端序,最多 4 字节(上限 2^28-1 = 268435455)。边界: 127→`7F`、128→`80 01`、16383→`FF 7F`、16384→`80 80 01`、2097151→`FF FF 7F`;
- **2 字节长度前缀字符串**: 主题/ClientId/用户名/密码/Will 一律 [长度 2B 大端][UTF-8 字节],解码端按剩余长度切出报文后靠它精确拆字段;
- **CONNECT 标志位(以 OASIS 3.1.1 §3.1.2.5 为准)**: bit0 保留(必须 0)、**bit1 CleanSession、bit2 Will、bit3-4 WillQoS、bit5 WillRetain、bit6 Password、bit7 UserName**;密码标志置位则用户名必须同时置位[MQTT-3.1.2-22];
- **解码三态**: 数据不足 → false + err 空(调用方继续累积);协议错误 → false + err 非空。

### 6.2 QoS1 完整流程

```
发布方                                  接收方(broker/客户端)
  │  PUBLISH(qos=1, packetId=N)            │
  │──────────────────────────────────────▶│  校验 + 投递
  │  PUBACK(packetId=N)                    │
  │◀──────────────────────────────────────│
  │  10s 内未收到 PUBACK → 重发(packetId=N, dup=1)
  │  再 10s 未确认 → 丢弃并断开重连(至少一次语义)
```

- 包 ID 由发送方递增分配(1..65535 循环),**必须非 0**(0 保留给无包 ID 的 QoS0);
- 在途表按包 ID 匹配 PUBACK;重发时置 DUP 位、原样重发已编码报文(不回头解析字节);
- 收 QoS1 发布: 先回调应用、后回 PUBACK;QoS2 不在本项目范围(收到 QoS2 发布告警后忽略,不假确认)。

### 6.3 keepalive / clean session / 重连退避

- **keepalive**: 空闲超过 **0.7×keepalive** 即发 PINGREQ(给调度与网络留出余量;broker 侧 1.5× 窗口);任意出站报文刷新空闲计时;keepaliveSec=0 禁用。
- **clean session**: true 时每次(重)连接后按订阅表重建 SUBSCRIBE(不依赖 broker 残留状态);false 时信任 broker 会话不重订阅——broker 重启会丢会话,需权衡。
- **重连退避**: 失败后等待"上一延迟×2"(1s→2s→4s→…→**30s 封顶**)再叠加 **0~30% 随机抖动**(xorshift32 自实现伪随机,避免依赖 rand 全局状态);握手成功退避清零。抖动防止多设备同时重连造成"惊群风暴"。

### 6.4 为什么从零实现(要点)

1. **展示协议理解深度**: 固定头/变长编码/QoS 状态机/会话语义这些"库替你做的事",逐字节可控是嵌入式协议岗的核心加分项;
2. **零依赖、可裁剪**: 工业网关资源受限,自研客户端体积/依赖可控,不引入 Paho/Mosquitto 的运行时负担;
3. **可测试**: 编解码纯模块可单测,测试用 fake broker 用自身编解码自举,形成闭环;
4. **互通有保障**: 字节级对齐 OASIS 3.1.1,已与真实 mosquitto broker 语义对齐(标志位按规范实现而非契约中的简化描述,见风险清单)。

### 6.5 Topic 设计(前缀 `edge-gate/edge-gate-01`)

| Topic | 方向 | QoS | 说明 |
|---|---|---|---|
| `{prefix}/telemetry` | pub | 1 | 周期遥测(全点聚合,含 stats) |
| `{prefix}/event` | pub | — | 告警/状态翻转事件(具体 QoS 以配置与实现为准) |
| `{prefix}/cmd` | sub | — | 下行命令(snapshot / set_period / write_reg / inject_fault / recover / ping) |
| `{prefix}/ack` | pub | — | 命令应答(单行 JSON,含 ok/data/err) |
| `{prefix}/status` | pub | retained | 网关连接/状态(新订阅者立即可见) |

- telemetry JSON: `{"device":"edge-gate-01","ts":"...","points":[{"id":"temp1","name":"1号炉温度","value":126.5,"unit":"C","quality":"good"}],"stats":{"tx":123,"rx":120,"timeouts":1}}`
- event JSON: `{"device":"...","ts":"...","type":"alarm","pointId":"temp1","level":"high","value":86.2,"threshold":80,"active":true}`
- 命令服务器: TCP 端口 19000(可配),新行分隔 JSON 命令 ≤4096B,单线程 poll 多路复用(≤8 客户端),120s 空闲断开。

---

## 7. PTY 虚拟串口演示原理

```
 edge-gate 进程(单进程双端)
 ┌────────────────────────────────┐
 │ ModbusMaster(主站)              │
 │   PosixSerial(包装 master fd)   │
 │        │ master(fd)             │
 │        ▼                        │
 │  内核 PTY 对  (openpty)          │   数据通路: 主从端像管道,
 │        │ slave = /dev/pts/N     │   但带完整终端语义(行规程/流控)
 │        ▼                        │
 │ ModbusSlave(软件从站)            │
 │   PosixSerial(/dev/pts/N)       │
 └────────────────────────────────┘
```

**原理**: `openpty()`(或 posix_openpt/grantpt/unlockpt/ptsname 链)创建内核虚拟终端对——master 是应用侧 fd(无设备路径),slave 表现为 `/dev/pts/N`。主从端数据通路像管道,但携带完整终端语义: **主从端共享同一套 termios 设置,tcsetattr(master) 即配置整对**。

**为什么用它做链路闭环验证**:

1. **真实代码路径**: 主站/从站走真实的 termios 配置、poll 超时、字节流读写——协议栈只差物理电平,时序逻辑(3.5T、半双工、帧切分)全部真实运行;
2. **为什么不用 socketpair**: socketpair 没有终端语义(无 termios、无行规程),无法验证串口配置代码;
3. **结构零侵入**: 主站/从站只依赖 `ISerialDevice` 接口,PTY 只是"另一种端口实现";
4. **已知局限(必须诚实说明)**: PTY 无真实波特率,termios 波特率设置不产生实际节流,3.5T 是**逻辑计算值**(由网关按配置波特率经 `setCharTimeUs` 注入主站/从站);高波特率(115200,3.5T≈334µs)下 poll 1ms 粒度偏粗,PTY 内核一次投递整帧不受影响,真实硬件建议按波特率核对。

**演示链路**: 配置 `serial.device = "pty-sim"` → 网关创建 PTY 对 → 主站连 master、软件从站连 slave → 采集线程按点表轮询,从站真实应答 → `inject_fault` 让链路"坏掉",主站统计变化 30 秒内可见 → `recover` 恢复。

---

## 8. V4L2 无关说明

本项目**与 V4L2 完全无关**:

- 采集路径是 **RS485/串口寄存器轮询**(Modbus RTU),不是摄像头帧采集;数据形态是 16/32 位寄存器值,不是视频帧;
- 不依赖 V4L2、无任何视频编解码/显示模块;环境中存在 V4L2 头文件,但 EdgeGate 不引用,与项目无关;
- 被问到"视频项目 vs 数据采集项目"时,可对比: 帧数据(大块、周期性、带宽敏感)vs 寄存器数据(小块、事务式、时延敏感);V4L2 采集链路 vs Modbus 事务管线;两者的共同点在于 Linux 设备模型与轮询/异步 IO 思路,但协议深度与实时性要求不同。

---

## 9. 交叉编译与部署细节

### 9.1 工具链与 CMake

```bash
sudo apt install -y g++-arm-linux-gnueabihf        # Debian/Ubuntu 交叉工具链
cmake -B build-arm \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake \
      -DES_BUILD_TESTS=OFF \                       # 交叉编译默认关测试
      -DES_BUILD_SIM=ON                            # 模拟从站可保留,ARM 板可回环自检
cmake --build build-arm -j$(nproc)
```

工具链文件要点: `CMAKE_SYSTEM_NAME=Linux`、`CMAKE_SYSTEM_PROCESSOR=arm`、编译器 `arm-linux-gnueabihf-g++`、硬浮点 ABI(`-mfloat-abi=hard`,IMX6ULL 硬浮点 SoC;若板卡是软浮点需改,以实际板卡为准)。

### 9.2 I.MX6ULL 硬件事实(克制陈述)

- Cortex-A7 单核,常见主频 **528MHz**(最高 800MHz,**以实际板卡为准**);
- UART 设备节点通常为 `/dev/ttymxc2`、`/dev/ttymxc3`(以实际板卡为准);
- RS485 需外部收发器芯片(如 MAX485),方向控制可走内核 `TIOCGRS485/TIOCSRS485` ioctl(本项目实现为可选,失败仅警告)或 GPIO 翻转(以实际硬件为准);
- 板上无 RTC/NTP 时时间戳可能不准,建议部署时同步时间或接入 NTP。

### 9.3 部署(见 README §5)

```
edge-gate 二进制 → /usr/local/bin/
edge-gate-arm.json → /etc/edge-gate.json(serial.device=/dev/ttymxc2, slaveSim.enabled=false, mqtt.host=服务器)
edge-gate.service → /etc/systemd/system/(systemctl enable --now edge-gate)
```

ARM 板回环自检: `ES_BUILD_SIM=ON` 时可在 ARM 板上以 `pty-sim` 配置跑通主站↔软件从站闭环,验证协议栈在目标架构上的行为后再接真实设备。

---

## 10. 故障排查表

| 现象 | 可能原因 | 排查手段 |
|---|---|---|
| 从站无响应(timeouts 持续增长) | 从站地址/波特率/校验位不符;接线/终端电阻;3.5T 切帧参数与波特率不匹配;从站未上电 | `snapshot` 看 stats 与各点 quality;查日志;minicom/串口助手抓原始字节;strace 看 ioctl/读写时序 |
| CRC 错(crcErrors 增长) | 波特率/数据位/校验位配置与从站不一致;线路干扰;帧被系统调度拆断 | 核对 termios 配置(如 8N1/9600);降波特率;检查接线屏蔽;确认无软件流控(IXON 会吞 0x11/0x13) |
| 重连风暴(reconnectCount 暴涨) | broker 未启动/地址端口错;keepalive 过短;网络抖动;客户端 ID 冲突被 broker 踢下线 | 检查 mosquitto 是否运行(`mosquitto_sub -t '#' -v` 验证);看 broker 日志;确认退避上限(1s→30s)生效;核对 clientId 唯一性 |
| 遥测时间戳异常 | 板卡无 RTC/NTP 未同步;时区不对;nowIso8601 为简化实现(不处理 DST 历史规则) | `date`/`timedatectl` 检查;部署时同步时间;接受毫秒级本地时间语义 |
| 遥测上行但数值不变 | 从站寄存器未更新;标度/字序错误;滤波窗口未满或过大;点未到期 | `snapshot` 对比 rawValue 与 value;`write_reg` 注入已知值验证标度;检查 pollPeriodMs;确认 is32Bit/word order/dataType 与从站手册一致 |
| 命令无应答 | cmdServer 未启用/端口占用;命令超 4096B;JSON 不合法 | `nc 127.0.0.1 19000` 手敲 `{"cmd":"ping"}` 验证;查日志;确认命令为单行 |
| 注入故障后统计不变 | 故障注入仅作用于模拟链路(pty-sim);真实串口下 inject_fault 无效果 | 确认 `slaveSim.enabled=true` 且 `serial.device=pty-sim`;`{"cmd":"inject_fault","fault":"crc"}` 后看 stats |
| 进程退出卡住 | 某线程未响应停止标志 | 第二次 Ctrl-C 强制退出;gdb 抓 `thread apply all bt` 看阻塞点;确认所有阻塞点可被唤醒打断 |
| 交叉编译产物在板卡上段错误 | ABI 不匹配(软/硬浮点);动态库缺失;glibc 版本不符 | `file` 查看 ELF 属性;`arm-linux-gnueabihf-readelf -d` 查依赖;优先静态链接核心模块或用板卡 SDK 工具链 |

---

## 附录 A: 与契约的已知偏差(以代码为准)

| # | 偏差 | 说明 |
|---|---|---|
| 1 | 写多个寄存器功能码 0x16→0x10(**已修复**) | 契约初版笔误为 0x16,实现按契约值;审稿轮修正为 Modbus 标准 0x10,新增逐字节流式回归测试验证 |
| 2 | CONNECT 标志位按 **OASIS 位序**实现(bit1 CleanSession … bit7 UserName) | 契约 §9 的简化描述与规范不一致;按规范实现以保证与真实 broker 互通 |
| 3 | `PosixSerial` 增加扩展构造 `PosixSerial(cfg, preopenedFd)`(包装 PTY master fd) | PTY 主端无路径,演示链路需要;契约原构造保留 |
| 4 | `ModbusMaster/ModbusSlave` 增加 `setCharTimeUs(double)` | ISerialDevice 不暴露波特率、PTY 无真实波特率,3.5T 由应用层注入;默认 9600 |
| 5 | `decodeFrame` 不做 CRC 校验(纯结构解析) | CRC 由主站(分类统计)与从站(静默丢弃)各自校验 |
| 6 | `MqttClient::stop()` 不跨线程 close/shutdown socket | 置标志 → 唤醒管道 → join 有界;fd 只由网络线程关闭,规避 fd 复用竞态 |
| 7 | QoS2 不在范围: 收到 QoS2 发布告警后忽略,不假确认 | 实现 PUBREC/PUBREL/PUBCOMP 状态机超出本项目范围 |

*构建/测试结果占位: 由主线 WSL 实编译 + ctest 后回填至交付说明,本文档不预填未经验证输出。*
