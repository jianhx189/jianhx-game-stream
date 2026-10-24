#pragma once

#include <QtWidgets>
#include <cmath>

namespace Ui {
static const QColor Canvas("#0b1020");
static const QColor Surface("#131e33");
static const QColor SurfaceAlt("#1b2942");
static const QColor Border("#2c3c5a");
static const QColor Text("#f0f5ff");
static const QColor Secondary("#b1c2dd");
static const QColor Muted("#7f95b7");
static const QColor Primary("#66c9f2");
static const QColor Accent("#a899ff");
static const QColor Success("#4ddab3");
static const QColor Warning("#f5bc65");
static const QColor Danger("#ff8296");

inline QLabel *text(const QString &value, const char *role = nullptr) {
    auto *label = new QLabel(value);
    if (role) label->setObjectName(role);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

inline QFrame *card() {
    auto *frame = new QFrame;
    frame->setObjectName("card");
    return frame;
}

class Metric final : public QFrame {
    QLabel *m_value;
    QLabel *m_detail;
public:
    Metric(const QString &title, const QString &detail, const QColor &tone = Primary) {
        setObjectName("card");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(18, 15, 18, 14);
        layout->setSpacing(6);
        auto *titleLabel = text(title, "muted");
        auto *stripe = new QFrame;
        stripe->setFixedSize(3, 18);
        stripe->setStyleSheet(QString("background:%1;border-radius:1px").arg(tone.name()));
        auto *heading = new QHBoxLayout;
        heading->addWidget(stripe);
        heading->addWidget(titleLabel, 1);
        layout->addLayout(heading);
        m_value = text("—", "metric");
        layout->addWidget(m_value);
        m_detail = text(detail, "small");
        m_detail->setWordWrap(true);
        layout->addWidget(m_detail);
        setMinimumHeight(116);
    }
    void set(const QString &value, const QString &detail = {}) {
        m_value->setText(value);
        if (!detail.isEmpty()) m_detail->setText(detail);
    }
};

class Trend final : public QWidget {
public:
    struct Sample { double time = 0; double value = std::numeric_limits<double>::quiet_NaN(); };
private:
    QVector<Sample> m_samples;
    QColor m_color;
    QString m_unit;
public:
    explicit Trend(const QColor &color = Primary, const QString &unit = "Mbps") : m_color(color), m_unit(unit) {
        setMinimumHeight(170);
    }
    void clear() { m_samples.clear(); update(); }
    void append(double time, double value) {
        m_samples.append({time, value});
        while (m_samples.size() > 300) m_samples.removeFirst();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = rect().adjusted(16, 20, -18, -30);
        painter.setPen(QPen(QColor("#20304b"), 1));
        for (int i = 0; i < 4; ++i) {
            const double y = plot.top() + plot.height() * i / 3;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        double maximum = 1;
        int valid = 0;
        for (const auto &sample : m_samples) {
            if (std::isfinite(sample.value)) {
                maximum = qMax(maximum, sample.value * 1.15);
                ++valid;
            }
        }
        if (!valid) {
            painter.setPen(Muted);
            painter.drawText(plot, Qt::AlignCenter, "等待真实采样 · 不显示演示曲线");
            return;
        }
        QPainterPath path;
        bool started = false;
        const double newest = m_samples.last().time;
        const double oldest = qMax(m_samples.first().time, newest - 300000.0);
        for (const auto &sample : m_samples) {
            if (!std::isfinite(sample.value)) { started = false; continue; }
            const double x = plot.left() + plot.width() * (sample.time - oldest) / qMax(1.0, newest - oldest);
            const double y = plot.bottom() - qMin(1.0, sample.value / maximum) * plot.height();
            if (started) path.lineTo(x, y);
            else { path.moveTo(x, y); started = true; }
        }
        painter.setPen(QPen(m_color, 2.2));
        painter.drawPath(path);
        painter.setPen(Muted);
        painter.drawText(16, height() - 7, "最近 300 次采样");
        painter.drawText(width() - 130, height() - 7, QString("%1 %2").arg(maximum, 0, 'f', 1).arg(m_unit));
    }
};

inline QPushButton *button(const QString &text, const char *role = nullptr) {
    auto *button = new QPushButton(text);
    if (role) button->setObjectName(role);
    button->setMinimumHeight(38);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

inline QString bytes(qint64 value) {
    if (value < 1024) return QString("%1 B").arg(value);
    if (value < 1024 * 1024) return QString("%1 KiB").arg(value / 1024.0, 0, 'f', 1);
    return QString("%1 MiB").arg(value / 1048576.0, 0, 'f', 2);
}
}
