# EdgeGate 代码实现导航(概念 → 文件 → 关键函数)

> 用途:按概念快速定位代码;每个条目给"看什么 + 怎么讲"。
> 行号以 2026-08-15 提交为准,函数名稳定。

---

## 1. Modbus RTU(老设备的"方言")

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/modbus/modbus_rtu.h/.cpp` | `encodeFrame()` / `decodeFrame()` / `charTimeUs(baud)` | 帧组包与拆包;3.5T 计算(1 字符 11 bit);三态解码(半包/完整/错误);帧长奇偶性消歧 |
| `src/modbus/modbus_crc.h/.cpp` | `crc16(data, len)` | 位运算实现,多项式 0xA001 反射、初值 0xFFFF、低字节在前;测试向量 `01 03 00 00 00 0A → 0xCDC5` |
| `src/modbus/modbus_master.h/.cpp` | `configure()` / `readPoint()` / `writeRegister()` / `convertRaw()` / `duePoints()` / `doTransaction()` | 点表映射与标度变换;轮询调度;事务管线(清缓冲→发→收→CRC→分类);事务级互斥 |

**讲解要点**:帧无头无尾靠 3.5T 字符间隔定界;主站按功能码推导帧长收包、3.5T 兜底;错帧分五类入统计;单线程轮询是 RS485 半双工的必然取舍。

## 2. 网关(中转站/总装)

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/app/gateway.h/.cpp` | `start()` / `stop()` / `setupSerial()` / `setupMqtt()` / `setupCmdServer()` / `setupHttp()` / `acquisitionLoop()` / `telemetryLoop()` / `handleCommand()` / `snapshotJson()` | 装配顺序与失败回滚;线程模型与停止顺序(采集→遥测→MQTT→Cmd→从站);状态机接线;命令路由统一(TCP/MQTT 同源) |

**讲解要点**:应用层唯一门面,五个入口(Web/Qt/CLI/TCP/MQTT)共用同一数据源;先停生产者再停消费者的停止纪律。

## 3. MQTT(物联网"普通话")

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/net/mqtt_packets.h/.cpp` | `encodeRemainingLength()` / `encodeConnect()` / `encodePublish()` / `decodePacket()` | 变长编码 7 个边界值;CONNECT 标志位按 OASIS 位序;半包/协议错误区分 |
| `src/net/mqtt_client.h/.cpp` | `start()` / `stop()` / `publish()` / `subscribe()` / `onlineLoop()` / `resubscribeAll()` | 单网络线程+用户线程仅入队;keepalive+PINGRESP 看门狗;QoS1 10s DUP 重发;指数退避重连;clean session 订阅重建;发送队列有界(512) |

**讲解要点**:为什么单线程模型(避免字节交错、退出时序可推理);keepalive 0.7× 发 PINGREQ、1.5× 无入站判死;QoS1 是"连接存活期内的至少一次",断线窗口会丢,JSONL 兜底。

## 4. 命令服务器(远程调试口)

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/net/cmd_server.h/.cpp` | `start()` / `stop()` / `handleLine()` | 单线程 poll 多路复用(≤8 客户端);新行分隔 JSON 命令(≤4096B);粘包/拆包处理;空闲 120s 断开;默认绑 127.0.0.1 |

**讲解要点**:低频短报文不搞每连接一线程;行协议 vs 长度前缀帧的取舍;绑回环地址是安全意识(命令通道无鉴权)。

## 5. 模拟从站(假仪表)

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/modbus/modbus_slave.h/.cpp` | `start()` / `injectFault()` / `handleFrame()` / `threadLoop()` | 3.5T 静默切帧(锚点=最后字节,F1 修复);CRC 错帧静默丢弃(真实从站行为);5 种故障注入;寄存器堆读写 |
| `src/hal/pty_pair.h/.cpp` | `createPtyPair()` | openpty 创建虚拟串口对:主站连 master、从站连 slave,真实 termios 时序 |

**讲解要点**:闭环验证的关键——协议栈走真实字节流路径,只差波特率节流与电平;故障注入让"错误分类统计+自动恢复"现场可见;PTY 局限(3.5T 是逻辑值)正是审稿发现真机缺陷的教训。

## 6. 补充:内置 Web 监控页(浏览器演示入口)

| 文件 | 关键函数 | 看什么 |
|---|---|---|
| `src/net/http_server.h/.cpp` | `start()` / `handleClient()` / `kIndexHtml` | 极简 HTTP/1.1(仅 GET 两路由);页面自包含 HTML+CSS+JS(Canvas 曲线,1s 轮询 `/api/snapshot`);绑回环 |

**讲解要点**:浏览器方案绕开图形环境依赖,任何机器都能演示;页面与 Qt/CLI 共用 `snapshotJson()` 同一数据源。

---

## 阅读顺序建议(30 分钟速览)

1. `src/app/gateway.h` 文件头注释(整个项目的设计要点都在这里,4 页纸)
2. `src/modbus/modbus_rtu.h` + `modbus_master.h` 头注释(协议与事务设计)
3. `src/net/mqtt_client.h` 头注释(线程模型与可靠性设计)
4. 每个 .cpp 里看"为什么"注释,追问细节时能指到具体行

## 代码规模参考

| 模块 | 文件数 | 核心逻辑 |
|---|---|---|
| core/util | 12 | 环形缓冲/事件总线/状态机/JSON |
| hal | 6 | 串口抽象/PTY |
| modbus | 8 | 协议栈(主站+从站) |
| edge | 4 | 滤波/告警规则 |
| net | 6 | MQTT 协议栈+命令服务器+HTTP |
| app | 6 | 网关总装/CLI/入口 |
| ui/qt | 6 | 可选 GUI |
| tests | 14 | 11 个测试目标 |
