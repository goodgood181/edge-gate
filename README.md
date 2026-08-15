# EdgeGate — 工业数据采集与边缘计算网关

> **一句话定位**: EdgeGate 是一个基于嵌入式 Linux(C++17)的工业数据采集网关——从零实现 **Modbus RTU 协议栈** 与 **MQTT 3.1.1 客户端**,通过 RS485/串口轮询现场设备,在边缘完成滤波与迟滞告警后上云,内置 **PTY 虚拟串口 + 软件从站**(寄存器堆可读写、支持故障注入)用于链路自测与演示,支持 CMake 交叉编译到 ARM Cortex-A7 与 x86 双平台。

---

## 1. 特性列表

**双协议栈,全部从零实现、零外部依赖(仅 std + POSIX)、禁异常(C++17)**

| 特性 | 说明 |
|---|---|
| **Modbus RTU 主站协议栈** | CRC16-Modbus(多项式/反射/初值/字节序全部按规范)、功能码 03/04/06/16(见下方功能码说明)、异常应答(0x80\|func + 异常码)、3.5T 静默间隔帧分隔、寄存器映射与标度变换(u16/i16/u32/i32/f32、大小端字序、scale/offset)、按 pollPeriodMs 的轮询调度、从站离线检测与错误五分类统计(超时/CRC 错/异常应答/地址不匹配/格式不符) |
| **MQTT 3.1.1 客户端** | 报文字节级编解码(固定头/剩余长度变长编码/2 字节长度前缀字符串)、CONNECT 握手与 CONNACK 校验、QoS0/QoS1 收发(10s 未确认 DUP 重发一次)、keepalive(0.7× 空闲阈值发 PINGREQ)、指数退避重连(1s→30s + 0~30% 抖动)、clean session 重建订阅、优雅停止(退出前尽力 DISCONNECT) |
| **软件从站 + 故障注入** | 按 3.5T 静默切帧应答的模拟从站(寄存器堆可读写),支持 5 种故障注入:none / crc / no_response / exception / wrong_slave——演示 30 秒出效果,主站统计立刻可见 |
| **PTY 虚拟串口闭环** | openpty 创建内核虚拟终端对,主站连 master、软件从站连 slave,走**真实 termios/poll/字节流路径**(只差物理电平),x86 上即可演示完整 RS485 轮询链路 |
| **边缘计算** | 滑动平均滤波(环形缓冲 + 运行和,O(1))、阈值 + 迟滞告警状态机(只发状态翻转事件,上云流量 = 事件数而非采样数)、数据质量 Good/Bad/Stale 全链路透传 |
| **线程生命周期管理** | 5 类线程(采集/遥测/MQTT 网络/CmdServer/模拟从站),全部"停止标志 + 唤醒"协作退出、有序 join,两次 Ctrl-C 强制退出兜底;RAII 线程封装、周期任务"超周期跳过补偿" |
| **工程化基础设施** | 手写最小 JSON DOM(带错误定位)、两级配置(严格整体/宽容局部)、事件总线(快照回调防死锁)、主状态机(默认转移表 + 64 条历史)、TCP 命令服务器(poll 多路复用、行协议防粘包)、**内置 Web 监控页(浏览器 localhost:18080 实时查看点表与曲线)**、CLI、可选 Qt GUI |
| **交叉编译** | CMake 静态库划分(es_core/es_modbus/es_net)+ 工具链文件交叉编译 ARM Cortex-A7(IMX6ULL)与 x86 双平台 |
| **测试** | 注册式极简单测框架 + PTY 全链路集成测试(主站↔从站真实字节流)+ 用自身编解码自举的 fake broker 测试 MQTT 客户端 |

> **功能码说明(重要)**: 写多个寄存器功能码采用 **Modbus 标准值 0x10**。开发过程中曾发现契约笔误写成 0x16,已在审稿轮修正为 0x10 并新增流式字节回归测试(9600 波特逐字节注入),与真实设备可直接互通。

---

## 2. 技术特点

① 协议**从零实现**的深度(帧结构/时序/错误处理逐字节可控,而非调用库);② 协议栈配套 **PTY 虚拟串口 + 软件从站**闭环验证(真实字节流路径,支持故障注入);③ **双平台工程化**(交叉编译 + 可移植代码 + 测试闭环)。

---

## 3. 快速开始(WSL / Ubuntu x86)

> 前置: WSL(Ubuntu 22.04/24.04)或原生 Linux,需 g++(≥9,支持 C++17)、cmake(≥3.16)、mosquitto。

```bash
# 1) 安装构建工具与 Mosquitto 代理(或运行 scripts/install-mosquitto.sh)
sudo apt update
sudo apt install -y g++ cmake make mosquitto mosquitto-clients

# 2) 构建(默认配置 + 单元测试,产物在 build/ 下)
./scripts/build.sh

# 3) 启动演示链路: 启动 mosquitto → 启动网关(pty-sim 虚拟串口 + 软件从站)
#    → 订阅 MQTT 主题观察遥测/告警/状态,另开终端可发命令(snapshot / inject_fault ...)
./scripts/run-demo.sh
```

- 默认配置 `config/edge-gate.json`: 串口设备为 `pty-sim`(openpty 虚拟串口,主站连 master、软件从站连 slave),点表含温度/压力/流量/电压/泵速 5 个采集控制点,遥测每 1s 上云。
- 演示中的交互: 遥测 JSON 每周期出现在 `edge-gate/edge-gate-01/telemetry`;用 `inject_fault` 注入故障后,主站统计(timeouts/crcErrors/exceptions)立刻变化;`write_reg` 可把泵速写入从站寄存器并验证。
- 构建结果/演示输出截图见交付说明(由主线回填)。

---

## 4. 交叉编译到 I.MX6ULL(ARM Cortex-A7)

**硬件事实**: I.MX6ULL 为 NXP 单核 **Cortex-A7**,常见主频 **528MHz**(最高 800MHz,以实际板卡为准);板载 UART 对应设备节点通常为 `/dev/ttymxc2`、`/dev/ttymxc3`(以实际板卡为准);ARMv7-A 硬浮点,交叉工具链用 `arm-linux-gnueabihf-` 前缀。

```bash
# 1) 安装交叉工具链(WSL Ubuntu)
sudo apt install -y g++-arm-linux-gnueabihf

# 2) 交叉配置 + 构建(工具链文件: cmake/toolchain-arm-linux-gnueabihf.cmake)
cmake -B build-arm \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-linux-gnueabihf.cmake \
      -DES_BUILD_TESTS=OFF \
      -DES_BUILD_SIM=ON          # 模拟从站可保留: 可在 ARM 板上做回环自检
cmake --build build-arm -j$(nproc)

# 3) 部署(以实际板卡 IP/用户为准)
scp build-arm/edge-gate        root@<板子IP>:/usr/local/bin/
scp config/edge-gate-arm.json  root@<板子IP>:/etc/edge-gate.json
scp scripts/edge-gate.service  root@<板子IP>:/etc/systemd/system/

# 4) 安装为系统服务并启动(开机自启)
ssh root@<板子IP> "systemctl daemon-reload && systemctl enable --now edge-gate"
ssh root@<板子IP> "journalctl -u edge-gate -f"   # 查看运行日志
```

**ARM 板部署配置要点**(`config/edge-gate-arm.json`):
- `serial.device` 改为真实串口 `/dev/ttymxc2`(RS485 需经收发器芯片,方向控制可走内核 TIOCSRS485 或 GPIO,以实际硬件为准);
- `slaveSim.enabled` 置 `false`(关闭软件从站,连接真实现场设备);
- `mqtt.host` 指向实际 MQTT 服务器(如 `192.168.1.10`)。
- 板载时间若无 RTC/NTP,遥测时间戳可能不准,建议部署时同步时间(见故障排查表)。

---

## 5. 目录结构

```
edge-gate/
├── CMakeLists.txt                       # 顶层: 静态库 es_core / es_modbus / es_net + 可执行 edge-gate
├── cmake/
│   ├── es_options.cmake                 # 构建选项: ES_BUILD_TESTS / ES_BUILD_QT_GUI / ES_BUILD_SIM
│   └── toolchain-arm-linux-gnueabihf.cmake  # ARM Cortex-A7 交叉编译工具链文件
├── config/
│   ├── edge-gate.json                   # x86 演示默认配置(pty-sim + 软件从站)
│   └── edge-gate-arm.json               # ARM 板部署示例(真实串口 + 关从站)
├── scripts/
│   ├── build.sh                         # 一键构建(x86)
│   ├── run-demo.sh                      # 一键演示(pty-sim + 软件从站)
│   ├── install-mosquitto.sh             # 安装/启动 mosquitto 代理
│   ├── deploy-arm.sh                    # 交叉编译 + 部署到 ARM 板
│   └── edge-gate.service                # systemd 单元(ARM 板开机自启)
├── src/
│   ├── core/    logger · ring_buffer · event_bus · state_machine · thread_utils · time_utils
│   ├── util/    json(手写 DOM) · config(两级配置)
│   ├── hal/     serial_device(接口) · posix_serial(termios) · pty_pair(虚拟串口)
│   ├── modbus/  modbus_crc · modbus_rtu(编解码) · modbus_master(主站) · modbus_slave(软件从站)
│   ├── edge/    signal_filter(滑动平均) · rule_engine(阈值+迟滞告警)
│   ├── net/     mqtt_packets(字节级编解码) · mqtt_client(客户端) · cmd_server(TCP 命令)
│   ├── app/     gateway(装配与线程管理) · cli · main
│   └── ui/qt/   curve_widget · main_window · ui_worker     # 可选 GUI(ES_BUILD_QT_GUI)
├── tests/       framework(注册式单测) + 各 test_*.cpp + fake_broker.h + CMakeLists.txt
├── docs/
│   ├── architecture.md                  # 架构设计
│   ├── overview.html                    # 项目总览页
│   ├── use-guide.md                     # 分步骤使用说明与原理解释
│   └── code-map.md                      # 代码导航(概念→文件→函数)
└── README.md
```

> 状态说明: `src/core`、`src/util`、`src/hal`、`src/modbus`、`src/edge`、`src/net` 已实现落地;`src/app`、`cmake/`、`config/`、`scripts/`、`tests/` 由应用层/测试子任务交付中,目录与接口以本 README 及 `api-contract.md` 为准。

---

## 6. 测试说明

**测试分层**(详见 `docs/architecture.md` §9 与 `tests/` 源码):

| 层 | 用例 | 验证内容 |
|---|---|---|
| 单元测试 | test_json / test_ring_buffer / test_state_machine / test_modbus_crc / test_modbus_rtu / test_rule_engine / test_signal_filter / test_mqtt_packets | 纯逻辑闭环: 已知向量、边界、错误路径 |
| 链路集成测试 | test_modbus_link(PTY 对 + 主站 + 从站) | 真实字节流下 03/04/06/16 读写、CRC 错/异常/无响应/错地址注入后的统计行为 |
| 网络集成测试 | test_mqtt_client + fake_broker(用自身编解码自举) | 握手/订阅/QoS0·QoS1/断线重连/PINGREQ |
| 端到端 | test_gateway(pty-sim + 2s 运行) | 点表 quality=good、遥测事件 ≥1、write_reg 落寄存器 |

**已核对的标准已知向量**(模块自检确认):
- CRC16-Modbus: `01 03 00 00 00 0A` → 0xCDC5(线路序 `C5 CD`);`01 06 00 01 00 17` → 0x0498(线路序 `98 04`);
- 字符时间: 9600 波特 → 1145.83µs/字符(3.5T ≈ 4010µs);115200 波特 → 95.49µs;
- MQTT 剩余长度变长编码边界: 127→`7F`、128→`80 01`、16383→`FF 7F`、16384→`80 80 01`、2097151→`FF FF 7F`;
- CONNECT 字节级比对示例:`{clientId="test", cleanSession, keepalive=30}` → `10 10 00 04 4D 51 54 54 04 02 00 1E 00 04 74 65 73 74`。

**完整构建与 ctest 结果见交付说明(由主线回填)**: WSL 下 `cmake -B build && cmake --build build && ctest --test-dir build` 全绿为准;本文档不预填未经验证的输出。

---

## 7. Roadmap

| 优先级 | 方向 | 说明 |
|---|---|---|
| P1 | **Modbus TCP** | 复用现有 RTU 编解码/点表/轮询架构,新增 TCP 传输层(port 502),现场异构组网 |
| P1 | **MQTT TLS** | mqtt_client 接入 OpenSSL/mbedTLS,支持 CA 校验与双向认证;测试用本地自签证书 |
| P2 | **Web 配置页** | 复用 cmd_server 命令协议,扩展 HTTP/WebSocket 配置界面,免改 JSON 文件 |
| P2 | **OPC UA** | 面向上位机/SCADA 的标准数据接入(可评估 open62541 等),提升工业互操作性 |
| P2 | **看门狗** | 应用级软狗(遥测/采集心跳超时自恢复)+ 可选硬件看门狗 /dev/watchdog,提升无人值守可靠性 |
| P3 | 采集历史回补 / 断网本地缓存 | 断网时 JSONL 落盘数据断点续传,与 MQTT QoS1 形成端到端至少一次语义 |

---

*文档与代码/契约一致性核对、已知偏差与风险见交付说明(主线汇总)与 `docs/architecture.md` 附录。*
