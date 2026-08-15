// 文件路径: src/ui/qt/main_window.cpp
// 职责: 主窗口实现(设计要点见 main_window.h)。
#include "main_window.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>

#include "../../app/gateway.h"
#include "../../util/json.h"

namespace {

// 质量 → 前景色(与 CLI 配色一致: good 绿 / stale 黄 / bad 红)
QString qualityColor(const QString& q) {
    if (q == "good") {
        return "#2ecc71";
    }
    if (q == "stale") {
        return "#f1c40f";
    }
    return "#e74c3c";
}

} // namespace

MainWindow::MainWindow(es::Gateway* gw, QWidget* parent)
    : QMainWindow(parent), m_gw(gw), m_worker(nullptr) {
    setupUi();

    // 告警订阅: 回调在发布者线程(采集线程)执行 → 经 alarmArrived 信号
    // 队列连接到 GUI 线程,天然线程安全
    m_busId = m_gw->eventBus().subscribe("event",
        [this](const std::string&, const std::string& payload) {
            emit alarmArrived(QString::fromStdString(payload));
        });
    connect(this, &MainWindow::alarmArrived, this, &MainWindow::onAlarm);

    // 快照轮询线程
    m_worker = new UiWorker(m_gw, this);
    connect(m_worker, &UiWorker::snapshotReady, this, &MainWindow::onSnapshotReady);
    m_worker->start();
}

MainWindow::~MainWindow() {
    m_worker->stop();
    m_worker->wait(); // join: 保证轮询线程已退出,不再发射信号
    m_gw->eventBus().unsubscribe(m_busId);
}

void MainWindow::setupUi() {
    const std::string devId =
        m_gw->config().getString("device", "id", "edge-gate-01");
    setWindowTitle(QString("EdgeGate 监控 — %1").arg(QString::fromStdString(devId)));
    resize(1100, 640);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // 左: 点表
    m_pointTable = new QTableWidget(this);
    m_pointTable->setColumnCount(6);
    m_pointTable->setHorizontalHeaderLabels(
        {"ID", "名称", "值", "单位", "质量", "轮询ms"});
    m_pointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    splitter->addWidget(m_pointTable);

    // 右: 曲线 + 告警 + 状态
    auto* right = new QWidget(this);
    auto* rl = new QVBoxLayout(right);
    m_statusLabel = new QLabel("连接中...", right);
    m_curve = new CurveWidget(right);
    m_alarmList = new QListWidget(right);
    m_alarmList->setMaximumHeight(160);
    rl->addWidget(m_statusLabel, 0);
    rl->addWidget(m_curve, 3);
    rl->addWidget(m_alarmList, 1);
    splitter->addWidget(right);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    setCentralWidget(splitter);
}

void MainWindow::onSnapshotReady(const QString& json) {
    std::string perr;
    const es::Json snap = es::Json::parse(json.toStdString(), &perr);
    if (!perr.empty()) {
        return;
    }

    // 状态栏: 状态机 + MQTT + 事务统计
    const QString state = QString::fromStdString(snap.get("state", es::Json("?")).asString("?"));
    const bool mqtt = snap.get("mqttConnected", es::Json(false)).asBool(false);
    const es::Json st = snap.get("stats");
    m_statusLabel->setText(
        QString("状态: %1 | MQTT: %2 | tx=%3 rx=%4 timeout=%5")
            .arg(state, mqtt ? "已连接" : "未连接")
            .arg((long long)st.get("tx").asInt(0))
            .arg((long long)st.get("rx").asInt(0))
            .arg((long long)st.get("timeouts").asInt(0)));

    // 点表 + 曲线(序列 = 行号)
    const es::Json pts = snap.get("points");
    m_pointTable->setRowCount(static_cast<int>(pts.size()));
    int row = 0;
    for (const es::Json& p : pts.items()) {
        const QString id = QString::fromStdString(p.get("id", es::Json("")).asString(""));
        const QString name = QString::fromStdString(p.get("name", es::Json("")).asString(""));
        const double value = p.get("value", es::Json(0.0)).asNumber(0.0);
        const QString unit = QString::fromStdString(p.get("unit", es::Json("")).asString(""));
        const QString q = QString::fromStdString(p.get("quality", es::Json("bad")).asString("bad"));
        const long long poll = (long long)p.get("pollPeriodMs", es::Json((int64_t)0)).asInt(0);

        m_pointTable->setItem(row, 0, new QTableWidgetItem(id));
        m_pointTable->setItem(row, 1, new QTableWidgetItem(name));
        m_pointTable->setItem(row, 2, new QTableWidgetItem(QString::number(value, 'f', 3)));
        m_pointTable->setItem(row, 3, new QTableWidgetItem(unit));
        auto* qi = new QTableWidgetItem(q);
        qi->setForeground(QBrush(QColor(qualityColor(q))));
        m_pointTable->setItem(row, 4, qi);
        m_pointTable->setItem(row, 5, new QTableWidgetItem(QString::number(poll)));

        // 曲线: bad 质量不绘制(断线直观可见)
        m_curve->setSeriesName(row, id);
        if (q != "bad") {
            m_curve->appendPoint(row, static_cast<double>(m_sample), value);
        }
        ++row;
    }
    ++m_sample;
}

void MainWindow::onAlarm(const QString& payload) {
    std::string perr;
    const es::Json j = es::Json::parse(payload.toStdString(), &perr);
    if (!perr.empty()) {
        return;
    }
    const QString level =
        QString::fromStdString(j.get("level", es::Json("?")).asString("?")).toUpper();
    const QString pointId =
        QString::fromStdString(j.get("pointId", es::Json("?")).asString("?"));
    const double value = j.get("value", es::Json(0.0)).asNumber(0.0);
    const double thr = j.get("threshold", es::Json(0.0)).asNumber(0.0);
    const bool active = j.get("active", es::Json(false)).asBool(false);
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_alarmList->insertItem(0,
        QString("[%1] %2 %3 %4: %5 (阈值 %6)")
            .arg(ts)
            .arg(active ? "进入告警" : "恢复")
            .arg(level)
            .arg(pointId)
            .arg(value, 0, 'f', 3)
            .arg(thr, 0, 'f', 3));
    while (m_alarmList->count() > 200) {
        delete m_alarmList->takeItem(m_alarmList->count() - 1); // 列表上限,防内存增长
    }
}
