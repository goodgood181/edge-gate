// 文件路径: src/app/main.cpp
// 职责: 进程入口 —— 参数解析、配置加载、daemonize、信号处理、
//       Gateway 生命周期编排、交互 CLI / Qt GUI 前端选择。
// 设计要点:
// 1) daemonize 双 fork 四步(见 daemonize() 注释): fork → setsid → 二次
//    fork → stdio 重定向,使进程脱离控制终端;刻意不 chdir("/") ——
//    配置文件/日志/JSONL 均使用相对路径(默认 ./config、./data);
// 2) 信号自管道(self-pipe): 信号处理器只做异步信号安全操作 —— 置
//    atomic 标志 + 写 1 字节管道。主循环 select 监听管道读端,信号到达
//    立即感知;二次 Ctrl-C 直接 _exit(1) 强制退出(首次优雅停止可能因
//    串口超时最长等 1s+,运维人员不想等);
// 3) 停止: 网关内部按 采集→遥测→MQTT→Cmd→从站 顺序 join(gateway.h);
//    SIGPIPE 一律忽略 —— 网络/串口对端关闭时写失败返回 EPIPE 而非杀进程。
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "../core/logger.h"
#include "../util/config.h"
#include "cli.h"
#include "gateway.h"

#if defined(ES_BUILD_QT_GUI)
#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include "../ui/qt/main_window.h"
#endif

namespace {

constexpr const char* kVersion = "1.0.0";
constexpr const char* kDefaultConfig = "./config/edge-gate.json";
constexpr const char* kPidFile = "/run/edge-gate.pid";

std::atomic<bool> g_stop{false};
int g_sigPipe[2] = {-1, -1};

// 信号处理器(仅异步信号安全操作)。
// 第一次信号 → 置标志 + 写管道(主循环感知后优雅停止);
// 第二次信号 → 立即强制退出: 优雅停止可能被串口超时拖住,用户再按一次
// Ctrl-C 表示"不等了"。sig_atomic_t 保证计数读写原子性(信号处理器内合法)。
void handleSignal(int sig) {
    static volatile sig_atomic_t count = 0;
    ++count;
    if (count >= 2) {
        ::_exit(1); // 强制退出(POSIX 允许 _exit 用于信号处理器)
    }
    g_stop.store(true);
    if (g_sigPipe[1] >= 0) {
        // glibc fortify 对 write 标注 warn_unused_result,先存后弃以消告警
        const char c = static_cast<char>(sig);
        const ssize_t wr = ::write(g_sigPipe[1], &c, 1);
        (void)wr;
    }
}

// daemonize: 双 fork + setsid + stdio 重定向 + PID 文件。
// 步骤说明:
//   1) 第一次 fork: 父进程退出,子进程成为孤儿被 init 收养 —— 关键点:
//      子进程不再是进程组组长,后续 setsid 才可能成功;
//   2) setsid: 创建新会话,子进程成为会话首进程,脱离控制终端 ——
//      否则终端关闭时内核向进程组发 SIGHUP,网关被杀死;
//   3) 第二次 fork: 会话首进程若再 open 终端设备,仍可能重新获得控制
//      终端;再 fork 一次让子进程不是会话首进程,从根上杜绝;
//   4) 重定向 stdin/stdout/stderr → /dev/null: 守护进程没有终端,
//      任何 printf/fprintf 都会失败甚至触发 SIGPIPE。
bool daemonize(std::string* err) {
    const pid_t pid1 = ::fork();
    if (pid1 < 0) {
        if (err) *err = "fork 失败: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid1 > 0) {
        ::_exit(0); // 父进程立即退出(shell 侧命令返回)
    }
    if (::setsid() < 0) {
        if (err) *err = "setsid 失败: " + std::string(std::strerror(errno));
        return false;
    }
    const pid_t pid2 = ::fork();
    if (pid2 < 0) {
        if (err) *err = "第二次 fork 失败: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid2 > 0) {
        ::_exit(0);
    }
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        (void)::dup2(devnull, STDIN_FILENO);
        (void)::dup2(devnull, STDOUT_FILENO);
        (void)::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            (void)::close(devnull);
        }
    }
    // 注意: 刻意不 chdir("/")(守护进程惯例) —— 配置文件/日志/JSONL 使用
    // 相对路径,chdir 会破坏它们;代价是 cwd 挂载点不可卸载,嵌入式可接受。
    // PID 文件: systemd Type=forking + PIDFile 依赖它;写失败不致命
    FILE* f = std::fopen(kPidFile, "w");
    if (f) {
        std::fprintf(f, "%ld\n", (long)::getpid());
        std::fclose(f);
    } else {
        ES_LOGE("main", "写 PID 文件失败: %s", kPidFile);
    }
    return true;
}

void printUsage(FILE* out) {
    std::fprintf(out,
        "EdgeGate %s — 工业数据采集网关(Modbus RTU/RS485 → 边缘处理 → MQTT 上云)\n"
        "用法: edge-gate [选项]\n"
        "  --config <path>   配置文件(默认 %s)\n"
        "  --daemon          以守护进程运行(双 fork/setsid/stdio 重定向 + PID 文件)\n"
        "  --verbose         调试级别日志\n"
        "  --version         打印版本后退出\n"
        "  -h, --help        显示本帮助\n",
        kVersion, kDefaultConfig);
}

// 非交互模式: 阻塞等待信号(守护进程 / stdin 非终端场景)
void waitForSignal() {
    char buf[8];
    while (::read(g_sigPipe[0], buf, sizeof(buf)) < 0 && errno == EINTR) {
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string configPath = kDefaultConfig;
    bool daemonMode = false;
    bool verbose = false;
    bool showHelp = false;
    bool showVersion = false;

    // ---- 参数解析(禁异常;未知参数报错退出码 2) ----
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--config 缺少参数\n");
                printUsage(stderr);
                return 2;
            }
            configPath = argv[++i];
        } else if (arg == "--daemon") {
            daemonMode = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--version") {
            showVersion = true;
        } else if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage(stderr);
            return 2;
        }
    }
    if (showVersion) {
        std::printf("EdgeGate %s\n", kVersion);
        return 0;
    }
    if (showHelp) {
        printUsage(stdout);
        return 0;
    }

    // ---- 加载配置(在 daemonize 之前: 失败可直接向 stderr 报错) ----
    es::Config cfg;
    std::string err;
    if (!es::Config::loadFromFile(configPath, &cfg, &err)) {
        std::fprintf(stderr, "EdgeGate 启动失败: %s\n", err.c_str());
        return 1;
    }

    // ---- daemonize(可选) ----
    if (daemonMode && !daemonize(&err)) {
        std::fprintf(stderr, "daemonize 失败: %s\n", err.c_str());
        return 1;
    }

    // ---- 日志: 级别(verbose 覆盖配置文件);守护模式控制台无意义 ----
    es::LogLevel level = es::LogLevel::Info;
    const std::string lvlStr = cfg.getString("log", "level", "info");
    if (lvlStr == "trace") {
        level = es::LogLevel::Trace;
    } else if (lvlStr == "debug") {
        level = es::LogLevel::Debug;
    } else if (lvlStr == "warn") {
        level = es::LogLevel::Warn;
    } else if (lvlStr == "error") {
        level = es::LogLevel::Error;
    }
    if (verbose) {
        level = es::LogLevel::Debug;
    }
    es::Logger::instance().init(cfg.getString("log", "file", ""), level,
                                !daemonMode && cfg.getBool("log", "console", true));

    // ---- 信号处理: SIGINT/SIGTERM → 优雅停止;SIGPIPE 忽略 ----
    if (::pipe(g_sigPipe) != 0) {
        ES_LOGE("main", "创建信号管道失败(errno=%d)", errno);
        return 1;
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSignal;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不设 SA_RESTART: 让阻塞 syscall 返回 EINTR,便于主循环感知
    (void)::sigaction(SIGINT, &sa, nullptr);
    (void)::sigaction(SIGTERM, &sa, nullptr);
    (void)::signal(SIGPIPE, SIG_IGN);

    // ---- 网关 ----
    es::Gateway gw;
    if (!gw.start(cfg, &err)) {
        ES_LOGE("main", "网关启动失败: %s", err.c_str());
        if (!daemonMode) {
            std::fprintf(stderr, "EdgeGate 启动失败: %s\n", err.c_str());
        }
        return 1;
    }

#if defined(ES_BUILD_QT_GUI)
    // ---- Qt GUI 前端: 窗口关闭或收到信号 → 退出事件循环 ----
    if (daemonMode) {
        std::fprintf(stderr, "--daemon 与 Qt GUI 互斥(守护进程无显示环境)\n");
        gw.stop();
        return 2;
    }
    {
        QApplication app(argc, argv);
        MainWindow win(&gw);
        win.show();
        // Qt 不安装自己的 SIGINT 处理器: 我们的自管道处理器置 g_stop,
        // 用 200ms 轮询感知(避免 QSocketNotifier 跨 Qt5/Qt6 信号签名差异)
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&app]() {
            if (g_stop.load()) {
                app.quit();
            }
        });
        timer.start(200);
        (void)app.exec();
    }
#else
    // ---- CLI / 非交互前端 ----
    const bool interactive = ::isatty(STDIN_FILENO) != 0;
    if (!daemonMode && (interactive || std::getenv("EDGE_GATE_CLI") != nullptr)) {
        // 交互终端(或 EDGE_GATE_CLI=1 强制,便于管道/脚本测试)
        es::Cli cli(&gw, g_sigPipe[0]);
        cli.run();
    } else {
        // 守护进程 / stdin 非终端: 阻塞等待信号
        ES_LOGI("main", "非交互模式: 等待信号退出(SIGINT/SIGTERM)");
        waitForSignal();
    }
#endif

    // ---- 优雅停止(网关内部按 采集→遥测→MQTT→Cmd→从站 顺序 join) ----
    ES_LOGI("main", "正在停止网关...");
    gw.stop();
    if (daemonMode) {
        (void)::unlink(kPidFile);
    }
    ES_LOGI("main", "EdgeGate 已退出,bye");
    return 0;
}
