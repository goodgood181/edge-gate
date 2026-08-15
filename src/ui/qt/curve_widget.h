// 文件路径: src/ui/qt/curve_widget.h
// 职责: 自绘多序列折线图 QWidget(零外部图表库) —— 点表曲线实时监视。
// 设计要点:
// 1) 为什么自绘: 引入 QCustomPlot/QtCharts 会带来许可(GPL/商业)与依赖
//    体积问题;本组件只画"网格 + 折线 + 图例",QPainter 轻量实现,
//    完全可控、可讲解;
// 2) 数据结构: 每序列一个 QVector<QPointF>,容量上限 kMaxPoints 滚动
//    丢弃最旧样本,内存有界,长时间运行不增长;
// 3) 自动量程: 每帧按全部序列可见点求 min/max 并留 10% 边距;恒值曲线
//    时退化为对称窗口,避免量程为零导致除零;
// 4) 重绘策略: QWidget 默认按位图缓冲重绘,无闪烁,无需手动双缓冲;
//    仅 appendPoint 时 update() 请求重绘,不主动刷屏。
#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class CurveWidget : public QWidget {
    Q_OBJECT

public:
    explicit CurveWidget(QWidget* parent = nullptr);

    // 追加一个点;seriesIdx 从 0 起,自动扩容;超出容量滚动丢弃最旧样本
    void appendPoint(int seriesIdx, double x, double y);
    void setSeriesName(int seriesIdx, const QString& name);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Series {
        QString name;
        QColor color;
        QVector<QPointF> pts;
    };

    static constexpr int kMaxPoints = 300; // 每序列保留样本数(滚动窗口)
    static constexpr int kMargin = 8;      // 绘图区边距(px)

    QVector<Series> m_series;
};
