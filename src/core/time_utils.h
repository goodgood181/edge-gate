// 文件路径: src/core/time_utils.h
// 职责: 时间工具 —— ISO8601 时间戳(本地时区 + 毫秒)、单调时钟毫秒、纪元毫秒。
// 典型用途: 遥测/告警 JSON 的 ts 字段(nowIso8601)、超时与周期计算(steadyMillis)、
//           数据新鲜度判断(epochMillis)。
//
// 设计要点:
// 1) steady_clock vs system_clock: 单调时钟不受系统时间调整(NTP/手动改时间)影响,
//    只用于"测量间隔"(超时/周期/轮询节拍);system_clock 用于"墙上时间"打点。
//    Linux 上 steady_clock 基于 CLOCK_MONOTONIC,与 CLOCK_REALTIME 明确分工。
// 2) nowIso8601 是简化实现: 用一次 tm_gmtoff 计算时区偏移,不处理历史时区规则
//    (如 DST 切换瞬间的 ±1h 偏差);对本项目(日志/遥测时间戳)足够 ——
//    要点: 知道简化在哪、什么时候需要 zoneinfo 级别的库。
// 3) 跨平台: 线程安全的 localtime_r(glibc)/localtime_s(MSVC) 分支,Windows 可移植编译。
#pragma once

#include <cstdint>
#include <string>

namespace es {

// 本地时间 ISO8601,毫秒精度,如 "2026-08-14T12:00:00.123+08:00"(简化实现,见文件头注释)
std::string nowIso8601();

// 单调时钟毫秒(系统时间调整不影响;用于间隔测量)
uint64_t steadyMillis();

// 自 Unix 纪元起的毫秒(墙上时间;用于时间戳/新鲜度)
uint64_t epochMillis();

} // namespace es
