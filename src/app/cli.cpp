// 文件路径: src/app/cli.cpp
// 职责: 交互终端实现(ANSI 转义序列;设计要点见 cli.h)。
#include "cli.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <sys/select.h>
#include <unistd.h>

#include "../core/logger.h"
#include "../core/time_utils.h"
#include "gateway.h"

namespace es {
namespace {

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiRed = "\033[31m";
constexpr const char* kAnsiGreen = "\033[32m";
constexpr const char* kAnsiYellow = "\033[33m";
constexpr const char* kAnsiCyan = "\033[36m";
constexpr const char* kClearScreen = "\033[2J\033[H"; // 清屏 + 光标归位
constexpr const char* kHome = "\033[H";               // 光标归位
constexpr const char* kClearLine = "\033[2K";         // 清除整行

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 按空白切词(命令 + 参数)
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

} // namespace

Cli::Cli(Gateway* gw, int signalFd)
    : m_gw(gw), m_signalFd(signalFd) {}

Cli::~Cli() = default;

void Cli::requestStop() {
    m_stop = true;
}

void Cli::run() {
    printBanner();
    for (;;) {
        if (m_stop.load()) {
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;
        if (m_signalFd >= 0) {
            FD_SET(m_signalFd, &rfds);
            if (m_signalFd > maxfd) {
                maxfd = m_signalFd;
            }
        }
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms 轮询: 及时响应 requestStop
        const int r = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // 致命 select 错误(极少见)
        }
        if (r == 0) {
            continue;
        }
        if (m_signalFd >= 0 && FD_ISSET(m_signalFd, &rfds)) {
            char buf[16];
            while (::read(m_signalFd, buf, sizeof(buf)) > 0) {
            } // 排空信号管道
            printf("\n[收到退出信号,正在停止网关...]\n");
            break;
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[256];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                break; // EOF(Ctrl-D / 管道关闭)
            }
            m_lineBuf.append(buf, (size_t)n);
            // 按 '\n' 切行(跨 select 周期累积,防半行)
            for (;;) {
                const size_t pos = m_lineBuf.find('\n');
                if (pos == std::string::npos) {
                    break;
                }
                std::string line = m_lineBuf.substr(0, pos);
                m_lineBuf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back(); // 兼容 Windows 粘贴的 CRLF
                }
                if (!handleLine(line)) {
                    return; // quit
                }
            }
        }
    }
}

bool Cli::handleLine(const std::string& raw) {
    const std::string line = trim(raw);
    if (line.empty()) {
        return true;
    }
    const std::vector<std::string> tok = split(line);
    const std::string& cmd = tok[0];

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        printHelp();
    } else if (cmd == "status" || cmd == "st") {
        printStatus();
    } else if (cmd == "sensors" || cmd == "ls") {
        printSensors();
    } else if (cmd == "watch" || cmd == "w") {
        watchLoop();
    } else if (cmd == "set_period") {
        if (tok.size() < 3) {
            printf("用法: set_period <id> <periodMs>\n");
            return true;
        }
        char* end = nullptr;
        const long ms = ::strtol(tok[2].c_str(), &end, 10);
        if (end == tok[2].c_str() || ms <= 0) {
            printf("periodMs 必须为正整数\n");
            return true;
        }
        std::string err;
        if (m_gw->setPeriodMs(tok[1], (uint32_t)ms, &err)) {
            printf("%s已设置%s %s 轮询周期 = %ld ms%s\n",
                   kAnsiGreen, kAnsiReset, tok[1].c_str(), ms, kAnsiReset);
        } else {
            printf("%s失败:%s %s\n", kAnsiRed, kAnsiReset, err.c_str());
        }
    } else if (cmd == "write_reg") {
        if (tok.size() < 3) {
            printf("用法: write_reg <id> <value(0..65535)>\n");
            return true;
        }
        char* end = nullptr;
        const long value = ::strtol(tok[2].c_str(), &end, 10);
        if (end == tok[2].c_str() || value < 0 || value > 65535) {
            printf("value 必须为 0..65535 的整数\n");
            return true;
        }
        std::string err;
        if (m_gw->writeRegister(tok[1], (uint16_t)value, &err)) {
            printf("%s已写入%s %s = %ld\n", kAnsiGreen, kAnsiReset, tok[1].c_str(), value);
        } else {
            printf("%s失败:%s %s\n", kAnsiRed, kAnsiReset, err.c_str());
        }
    } else if (cmd == "inject_fault") {
        if (tok.size() < 2) {
            printf("用法: inject_fault <none|crc|no_response|exception|wrong_slave>\n");
            return true;
        }
        std::string err;
        if (m_gw->injectFault(tok[1], &err)) {
            printf("%s已注入故障:%s %s(观察 sensors 的错误计数与状态机)\n",
                   kAnsiYellow, kAnsiReset, tok[1].c_str());
        } else {
            printf("%s失败:%s %s\n", kAnsiRed, kAnsiReset, err.c_str());
        }
    } else if (cmd == "recover") {
        m_gw->recover();
        printf("%s已恢复:%s 清除故障注入并复位链路判定\n", kAnsiGreen, kAnsiReset);
    } else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        printf("bye\n");
        return false;
    } else {
        printf("未知命令: %s(输入 help 查看帮助)\n", cmd.c_str());
    }
    return true;
}

void Cli::printBanner() const {
    printf("%sEdgeGate 工业数据采集网关 — 交互终端%s\n", kAnsiCyan, kAnsiReset);
    printf("输入 help 查看命令;Ctrl-C 优雅退出(再按一次强制退出)\n\n");
}

void Cli::printHelp() const {
    printf("%s可用命令:%s\n", kAnsiCyan, kAnsiReset);
    printf("  status                    网关状态(状态机/MQTT/事务统计)\n");
    printf("  sensors                   点表一览(值/质量/错误计数)\n");
    printf("  watch                     实时监视(每秒刷新,任意键退出)\n");
    printf("  set_period <id> <ms>      修改点轮询周期(如 set_period temp1 500)\n");
    printf("  write_reg <id> <value>    写寄存器(仅可写点,如 write_reg pump_speed 1200)\n");
    printf("  inject_fault <fault>      故障注入(none/crc/no_response/exception/wrong_slave)\n");
    printf("  recover                   恢复: 清除故障注入并复位链路判定\n");
    printf("  quit                      退出\n\n");
}

void Cli::printStatus() const {
    std::string perr;
    const Json snap = Json::parse(m_gw->snapshotJson(), &perr);
    if (!perr.empty()) {
        printf("快照解析失败: %s\n", perr.c_str());
        return;
    }
    printf("%s=== EdgeGate 状态 ===%s\n", kAnsiCyan, kAnsiReset);
    printf("  设备: %s (%s)\n",
           snap.get("device", Json("?")).asString("?").c_str(),
           snap.get("name", Json("?")).asString("?").c_str());
    printf("  状态: %s", snap.get("state", Json("?")).asString("?").c_str());
    if (snap.get("mqttEnabled", Json(false)).asBool(false)) {
        printf("  |  MQTT: %s",
               snap.get("mqttConnected", Json(false)).asBool(false) ? "已连接" : "未连接");
    }
    printf("\n");
    printf("  运行时长: %.1f s\n", snap.get("uptimeMs", Json((int64_t)0)).asInt(0) / 1000.0);
    const Json st = snap.get("stats");
    printf("  Modbus: tx=%lld rx=%lld timeout=%lld crc=%lld exc=%lld\n",
           (long long)st.get("tx").asInt(0), (long long)st.get("rx").asInt(0),
           (long long)st.get("timeouts").asInt(0), (long long)st.get("crcErrors").asInt(0),
           (long long)st.get("exceptions").asInt(0));
    printf("  MQTT: sent=%lld recv=%lld reconnect=%lld\n",
           (long long)st.get("mqttSent").asInt(0), (long long)st.get("mqttRecv").asInt(0),
           (long long)st.get("mqttReconnects").asInt(0));
    printf("\n");
}

void Cli::printSensors() const {
    printf("%s=== 点表 ===%s\n", kAnsiCyan, kAnsiReset);
    printSensorsTable();
}

void Cli::printSensorsTable() const {
    std::string perr;
    const Json snap = Json::parse(m_gw->snapshotJson(), &perr);
    if (!perr.empty()) {
        printf("快照解析失败: %s\n", perr.c_str());
        return;
    }
    printf("%-10s %-18s %12s %-8s %-6s %10s %8s\n",
           "ID", "名称", "值", "单位", "质量", "更新ms", "错误");
    const Json pts = snap.get("points");
    for (const Json& p : pts.items()) {
        const std::string q = p.get("quality", Json("bad")).asString("bad");
        const char* color = kAnsiGreen;
        if (q == "bad") {
            color = kAnsiRed;
        } else if (q == "stale") {
            color = kAnsiYellow;
        }
        printf("%s%-10s %-18s %12.3f %-8s %-6s %10lld %8lld%s\n",
               color,
               p.get("id", Json("?")).asString("?").c_str(),
               p.get("name", Json("")).asString("").c_str(),
               p.get("value", Json(0.0)).asNumber(0.0),
               p.get("unit", Json("")).asString("").c_str(),
               q.c_str(),
               (long long)p.get("lastUpdateMs", Json((int64_t)0)).asInt(0),
               (long long)p.get("errCount", Json((int64_t)0)).asInt(0),
               kAnsiReset);
    }
}

void Cli::watchLoop() const {
    printf("%s[实时监视] 每秒刷新,按任意键退出%s\n", kAnsiCyan, kAnsiReset);
    printf("%s", kClearScreen);
    for (;;) {
        printf("%s", kHome);
        printSensorsTable();
        printf("%s(任意键退出监视)%s", kAnsiYellow, kAnsiReset);
        fflush(stdout);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        const int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[64];
            while (::read(STDIN_FILENO, buf, sizeof(buf)) > 0) {
            } // 丢弃按键,退出监视
            break;
        }
        if (m_stop.load()) {
            break; // 信号到达(select 超时后检查)
        }
    }
    printf("%s", kClearScreen);
    printf("监视结束\n");
}

} // namespace es
