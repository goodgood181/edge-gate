// 文件路径: src/ui/qt/ui_worker.cpp
// 职责: UiWorker 实现(设计要点见 ui_worker.h)。
#include "ui_worker.h"

#include "../../app/gateway.h"

UiWorker::UiWorker(es::Gateway* gw, QObject* parent)
    : QThread(parent), m_gw(gw) {}

UiWorker::~UiWorker() {
    stop();
    wait(); // 析构前 join,防泄漏
}

void UiWorker::stop() {
    m_stop = true;
}

void UiWorker::run() {
    while (!m_stop.load()) {
        if (m_gw) {
            emit snapshotReady(QString::fromStdString(m_gw->snapshotJson()));
        }
        msleep(500); // 轮询周期 500ms(与 1s 遥测周期错开,界面更顺滑)
    }
}
