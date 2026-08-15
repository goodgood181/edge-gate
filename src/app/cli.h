// 文件路径: src/app/cli.h
// 职责: 交互式命令行前端(ANSI 终端): 状态查询、点表查看、实时监视、
//       运维命令(set_period / write_reg / inject_fault / recover)。
// 设计要点:
// 1) 与 select 混用时的输入读取: 不用 std::getline/stdio —— stdio 会提前把
//    内核数据读进自己的缓冲,之后 select(fd) 永远"无数据"而 getline 又
//    没有缓冲可读 → 界面冻结。这里直接 read(0) 累积按行切分,与 select
//    的可见性完全一致(ICANON 行规程已由内核完成行缓冲与回显);
// 2) 单线程事件循环: select 同时监听 stdin 与信号自管道读端,命令处理与
//    Ctrl-C 优雅退出在同一个循环内完成,无竞态;
// 3) watch 模式: 每秒重绘点表(清屏 + 光标归位 + 逐行覆盖),ANSI 颜色按
//    质量着色(good=绿 bad=红 stale=黄),任意键退出。
#pragma once

#include <atomic>
#include <string>

#include "../util/json.h"

namespace es {

class Gateway;

class Cli {
public:
    // signalFd: 信号自管道读端(main 安装 SIGINT/SIGTERM 处理器后传入);
    //           有信号到达时 CLI 立即退出。传 -1 表示无信号管道。
    explicit Cli(Gateway* gw, int signalFd);
    ~Cli();

    Cli(const Cli&) = delete;
    Cli& operator=(const Cli&) = delete;

    void run();          // 阻塞运行直到 quit / EOF / 信号
    void requestStop();  // 线程安全: 置停止标志(200ms 内响应)

private:
    bool handleLine(const std::string& line); // 处理一行命令;返回 false = 退出
    void printBanner() const;
    void printHelp() const;
    void printStatus() const;
    void printSensors() const;
    void printSensorsTable() const;
    void watchLoop() const;

    Gateway* m_gw;
    int m_signalFd;
    std::atomic<bool> m_stop{false};
    std::string m_lineBuf; // 跨 select 周期累积的输入(防半行粘包)
};

} // namespace es
