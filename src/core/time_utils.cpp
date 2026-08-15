// 文件路径: src/core/time_utils.cpp
// 职责: time_utils.h 的实现,设计要点见头文件注释。
#include "time_utils.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace es {

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt); // 线程安全版本(glibc)
#endif

    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &lt);

    long offSec = 0;
#if defined(__GLIBC__) || defined(__linux__)
    offSec = lt.tm_gmtoff; // glibc 扩展: 秒级 UTC 偏移
#else
    // 回退: 用 gmtime 与 localtime 的差值近似(不处理跨日/DST 历史规则,注释说明取舍)
    struct tm gt;
#if defined(_WIN32)
    gmtime_s(&gt, &t);
#else
    gmtime_r(&t, &gt);
#endif
    offSec = (lt.tm_hour - gt.tm_hour) * 3600L + (lt.tm_min - gt.tm_min) * 60L + (lt.tm_sec - gt.tm_sec);
#endif

    // 偏移格式 "+08:00"/"-05:30"(offM 按正余数处理,负偏移时分钟不出现负号)
    const long offH = offSec / 3600;
    const long offM = ((offSec % 3600) + 3600) % 3600 / 60;
    char out[96];
    std::snprintf(out, sizeof(out), "%s.%03d%+03ld:%02ld",
                  date, static_cast<int>(ms.count()), offH, offM);
    return std::string(out);
}

uint64_t steadyMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t epochMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace es
