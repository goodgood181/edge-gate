// 文件路径: src/ui/qt/main_window.h
// 职责: 网关 Qt 监控主窗口 —— 点表(QTableWidget) + 实时曲线(自绘) +
//       告警列表 + 状态栏。数据来源两条通道:
//         a) UiWorker 轮询快照(500ms)→ 点表/曲线/状态栏;
//         b) Gateway::EventBus "event" 订阅 → 告警列表(低延迟、低频)。
// 设计要点:
// 1) 高频数据轮询 + 低频事件订阅的组合: 点表 500ms 刷新足够且流量可控;
//   告警是低频高价值事件,走总线订阅即时弹出 —— 两条通道各取所长,
//   也展示了 Gateway 对外接口设计(snapshot() 与 eventBus())的配合;
// 2) 跨线程信号: EventBus 回调在发布者线程(采集线程)执行,通过
//    emit alarmArrived → 信号槽 AutoConnection 自动转为队列连接投递到
//    GUI 线程,无锁无竞态;
// 3) 生命周期: 析构时先停 UiWorker 并 wait()(join),再退订总线,
//    保证不再有回调引用本窗口。
#pragma once

#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QTableWidget>

#include <cstddef>

#include "curve_widget.h"
#include "ui_worker.h"

namespace es {
class Gateway;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(es::Gateway* gw, QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void alarmArrived(const QString& payload); // 采集线程 → GUI 线程(队列连接)

private slots:
    void onSnapshotReady(const QString& json);
    void onAlarm(const QString& payload);

private:
    void setupUi();

    es::Gateway* m_gw;
    QTableWidget* m_pointTable;
    CurveWidget* m_curve;
    QListWidget* m_alarmList;
    QLabel* m_statusLabel;
    UiWorker* m_worker;
    std::size_t m_busId = 0;  // EventBus 订阅号(析构时退订)
    long long m_sample = 0;   // 曲线 X 轴样本计数
};
