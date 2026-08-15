// 文件路径: src/ui/qt/ui_worker.h
// 职责: 后台轮询线程 —— 定时调用 Gateway::snapshot() 生成快照 JSON,
//       通过 Qt 信号(跨线程自动队列连接)投递到 GUI 线程刷新界面。
// 设计要点:
// 1) 为什么轮询而非推送: Gateway 内部是事件驱动(EventBus),但曲线需要
//    连续的数据源,逐点推送会产生大量信号;轮询 500ms 聚合一次快照,
//    流量小且天然带"心跳"语义(GUI 可据此判断网关存活/失联);
// 2) 跨线程安全: snapshot() 线程安全(网关内部 m_mutex);信号槽默认
//    AutoConnection —— 跨线程发射自动转为队列连接,数据经值拷贝传递,
//    无共享指针跨线程;告警类低频事件走 EventBus 订阅(见 main_window),
//    高频数据走轮询,两条通道各取所长;
// 3) 可停止: stop() 置原子标志,run() 每轮检查,最迟一个轮询周期内
//    退出;析构 stop()+wait() 保证 join,无泄漏。
#pragma once

#include <QString>
#include <QThread>

#include <atomic>

namespace es {
class Gateway;
}

class UiWorker : public QThread {
    Q_OBJECT

public:
    explicit UiWorker(es::Gateway* gw, QObject* parent = nullptr);
    ~UiWorker() override;

    void stop(); // 置停止标志;线程最迟一个轮询周期内退出

signals:
    void snapshotReady(const QString& json); // 每 500ms 一帧快照(GUI 线程槽)

protected:
    void run() override;

private:
    es::Gateway* m_gw;
    std::atomic<bool> m_stop{false};
};
