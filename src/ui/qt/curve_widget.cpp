// 文件路径: src/ui/qt/curve_widget.cpp
// 职责: 自绘折线图实现(设计要点见 curve_widget.h)。
#include "curve_widget.h"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace {

// 序列配色板: 按序列索引循环取用(与告警"高/低限"语义无关,仅区分曲线)
QColor colorFor(int idx) {
    static const QColor kPalette[] = {
        QColor(0xE7, 0x4C, 0x3C), // 红
        QColor(0x34, 0x98, 0xDB), // 蓝
        QColor(0x2E, 0xCC, 0x71), // 绿
        QColor(0xF1, 0xC4, 0x0F), // 黄
        QColor(0x9B, 0x59, 0xB6), // 紫
        QColor(0x1A, 0xBC, 0x9C)  // 青
    };
    return kPalette[idx % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

} // namespace

CurveWidget::CurveWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(320, 200);
    // 深色底,工业监控风格
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x16, 0x1A, 0x24));
    setPalette(pal);
}

void CurveWidget::appendPoint(int seriesIdx, double x, double y) {
    while (m_series.size() <= static_cast<int>(seriesIdx)) {
        Series s;
        s.color = colorFor(static_cast<int>(m_series.size()));
        m_series.append(s);
    }
    Series& s = m_series[seriesIdx];
    s.pts.append(QPointF(x, y));
    while (s.pts.size() > kMaxPoints) {
        s.pts.remove(0); // 滚动丢弃最旧样本(容量固定,内存有界)
    }
    update(); // 请求重绘(由事件循环调度,不主动刷屏)
}

void CurveWidget::setSeriesName(int seriesIdx, const QString& name) {
    while (m_series.size() <= seriesIdx) {
        Series s;
        s.color = colorFor(static_cast<int>(m_series.size()));
        m_series.append(s);
    }
    m_series[seriesIdx].name = name;
    update();
}

void CurveWidget::clear() {
    for (Series& s : m_series) {
        s.pts.clear();
    }
    update();
}

void CurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0x16, 0x1A, 0x24));

    const int w = width();
    const int h = height();
    // 绘图区(顶部留图例行)
    const QRect plotRect(kMargin, kMargin + 14, w - 2 * kMargin, h - 2 * kMargin - 14);
    if (plotRect.width() <= 0 || plotRect.height() <= 0) {
        return;
    }

    // ---- 自动量程: 全部序列可见点 min/max + 10% 边距 ----
    double yMin = 0.0;
    double yMax = 1.0;
    bool has = false;
    for (const Series& s : m_series) {
        for (const QPointF& pt : s.pts) {
            if (!has) {
                yMin = yMax = pt.y();
                has = true;
            } else {
                yMin = std::min(yMin, pt.y());
                yMax = std::max(yMax, pt.y());
            }
        }
    }
    if (yMax - yMin < 1e-9) {
        yMax += 1.0; // 恒值曲线: 退化为对称窗口,防除零
        yMin -= 1.0;
    }
    const double pad = (yMax - yMin) * 0.1;
    yMin -= pad;
    yMax += pad;

    // ---- 网格 + 数值刻度(4 条水平线) ----
    p.setPen(QPen(QColor(0x2A, 0x30, 0x3E), 1));
    for (int i = 0; i <= 4; ++i) {
        const double fy = plotRect.top() + plotRect.height() * i / 4.0;
        p.drawLine(plotRect.left(), static_cast<int>(fy), plotRect.right(), static_cast<int>(fy));
        const double val = yMax - (yMax - yMin) * i / 4.0;
        p.setPen(QColor(0x8A, 0x93, 0xA6));
        p.drawText(plotRect.left() - 2, static_cast<int>(fy) - 2,
                   QString::number(val, 'f', 1));
        p.setPen(QPen(QColor(0x2A, 0x30, 0x3E), 1));
    }

    // ---- 序列折线: X 按样本时间戳归一化到可见窗口 ----
    for (const Series& s : m_series) {
        if (s.pts.size() < 2) {
            continue;
        }
        const double x0 = s.pts.first().x();
        const double x1 = s.pts.last().x();
        const double xSpan = (x1 - x0 > 1e-9) ? (x1 - x0) : 1.0;
        p.setPen(QPen(s.color, 1.6));
        // 逐段 drawLine 而非 QPainterPath: 最小重现验证 Qt 5.15 xcb 后端在
        // QWidget 上 drawPath 整条曲线不渲染(同一 painter 下 drawLine 正常),
        // 疑为 Qt 平台后端 bug;折线语义完全等价,300 点×6 序列 = 最多
        // 1800 段/帧(500ms 刷新),开销可忽略。
        for (int i = 1; i < s.pts.size(); ++i) {
            const QPointF& a = s.pts[i - 1];
            const QPointF& b = s.pts[i];
            const double ax = plotRect.left() + (a.x() - x0) / xSpan * plotRect.width();
            const double ay = plotRect.bottom() -
                              (a.y() - yMin) / (yMax - yMin) * plotRect.height();
            const double bx = plotRect.left() + (b.x() - x0) / xSpan * plotRect.width();
            const double by = plotRect.bottom() -
                              (b.y() - yMin) / (yMax - yMin) * plotRect.height();
            p.drawLine(QPointF(ax, ay), QPointF(bx, by));
        }
    }

    // ---- 图例(顶部,按序列名) ----
    int lx = plotRect.left();
    for (const Series& s : m_series) {
        if (s.name.isEmpty()) {
            continue;
        }
        p.setPen(s.color);
        p.drawText(lx, 12, s.name);
        lx += p.fontMetrics().horizontalAdvance(s.name) + 24;
    }
}
