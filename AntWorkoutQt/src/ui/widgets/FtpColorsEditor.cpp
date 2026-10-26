#include "FtpColorsEditor.hpp"

#include <QColorDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

#include "AnalogGauge.hpp"
#include "AppState.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace ftpeditor {

QString zoneName(const QString& zone) {
    if (zone == u"free") {
        return QStringLiteral("Free ride  (0 W)");
    }
    const int z = zone.toInt();
    if (z == 0) {
        return QStringLiteral("Zone 1  (under %1%)").arg(kZoneLimits[0]);
    }
    if (z == 4) {
        return QStringLiteral("Zone 5  (%1% and up)").arg(kZoneLimits[3]);
    }
    return QStringLiteral("Zone %1  (%2–%3%)").arg(z + 1).arg(kZoneLimits[z - 1]).arg(kZoneLimits[z]);
}

namespace {

constexpr auto kGraphBackground = "#bdd6de";

ftpcolors::ZoneRanges rangesFor(int ftp) {
    ftpcolors::ZoneRanges ranges;
    for (const int limit : kZoneLimits) {
        ranges.limits.append(ftp * limit / 100.0);
    }
    return ranges;
}

QString shortName(const QString& zone) {
    return zoneName(zone).section(QStringLiteral("  "), 0, 0);
}

}  // namespace

}  // namespace ftpeditor

using namespace ftpeditor;

// ---- Swatch, GradientBar ----

Swatch::Swatch(const QString& tip, std::function<void(const QString&)> onPick) : onPick_(std::move(onPick)) {
    setFixedSize(40, 24);
    setToolTip(tip);
    connect(this, &QPushButton::clicked, this, &Swatch::pick);
}

void Swatch::setColor(const QString& color) {
    color_ = color;
    // (straight to Qt: these show the zone colors themselves, not the theme's color replacements)
    setStyleSheet(QStringLiteral("background-color:%1; border:1px solid #444; border-radius:4px;").arg(color));
}

void Swatch::pick() {
    const QString before = color_;
    QColorDialog dialog(QColor(before), this);
    dialog.setWindowTitle(toolTip());
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    connect(&dialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) { onPick_(c.name()); });
    if (dialog.exec() != QDialog::Accepted) {
        onPick_(before);
    }
}

GradientBar::GradientBar() {
    setMinimumSize(90, 24);
}

void GradientBar::setColors(const QList<QString>& colors) {
    colors_ = colors;
    update();
}

void GradientBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient gradient(rect.topLeft(), rect.topRight());
    for (int i = 0; i < colors_.size(); ++i) {
        gradient.setColorAt(i / static_cast<double>(std::max<qsizetype>(1, colors_.size() - 1)), QColor(colors_[i]));
    }
    painter.setBrush(gradient);
    painter.setPen(QPen(QColor(QStringLiteral("#444444"))));
    painter.drawRoundedRect(rect, 4, 4);
}

// ---- The tab ----

FtpColorsTab::FtpColorsTab() {
    setWidgetResizable(true);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    auto* note = new QLabel(QStringLiteral(
        "One color per power zone (by % of FTP; RPM in Musical Cadence Mode). Each zone fades from pale where it starts "
        "to its color where it ends, on the gauges, spiral, block labels and workout graph. A color too close to the "
        "theme's background is moved until it stands out (adjusted). The FTP Colors window shows them on the parts for "
        "any FTP and power. Saved with the theme."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* box = new QGroupBox(QStringLiteral("Zone colors"));
    auto* grid = new QGridLayout(box);
    const QStringList headers = {QStringLiteral("Color"), QStringLiteral("Looks like (on this theme)"), QString()};
    for (int col = 0; col < headers.size(); ++col) {
        grid->addWidget(new QLabel(QStringLiteral("<small>%1</small>").arg(headers[col])), 0, col + 1);
    }
    for (int zone = 0; zone < ftpcolors::kZones; ++zone) {
        const QString key = QString::number(zone);
        auto* swatch = new Swatch(zoneName(key), [zone](const QString& c) { zones::setZoneColor(zone, c); });
        auto* bar = new GradientBar;
        auto* adjusted = new QLabel;
        refreshers_.append([zone, swatch, bar, adjusted] {
            swatch->setColor(zones::zoneColors()[zone]);
            const ftpcolors::ZoneColors colors = zones::palette()[zone];
            bar->setColors({colors.pale, colors.color});
            adjusted->setText(colors.adjusted ? QStringLiteral("<small>adjusted</small>") : QString());
        });
        const int row = zone + 1;
        grid->addWidget(new QLabel(zoneName(key)), row, 0);
        grid->addWidget(swatch, row, 1);
        grid->addWidget(bar, row, 2);
        grid->addWidget(adjusted, row, 3);
        grid->addWidget(resetButton(zone), row, 4);
    }
    const int row = ftpcolors::kZones + 1;
    auto* swatch = new Swatch(zoneName(QStringLiteral("free")), [](const QString& c) { zones::setFreeColor(c); });
    auto* bar = new GradientBar;
    refreshers_.append([swatch, bar] {
        swatch->setColor(zones::freeColor());
        const auto [a, b] = zones::freeRideColors();
        bar->setColors({a, b});
    });
    grid->addWidget(new QLabel(zoneName(QStringLiteral("free"))), row, 0);
    grid->addWidget(swatch, row, 1);
    grid->addWidget(bar, row, 2);
    grid->addWidget(resetButton(-1), row, 4);
    grid->setColumnStretch(2, 1);
    layout->addWidget(box);

    auto* resetAllButton = new QPushButton(QStringLiteral("Reset all FTP colors"));
    connect(resetAllButton, &QPushButton::clicked, this, &FtpColorsTab::resetAll);
    layout->addWidget(resetAllButton);
    layout->addStretch(1);
    setWidget(body);
    connect(&zones::notifier(), &zones::Notifier::changed, this, &FtpColorsTab::refresh);
    refresh();
}

void FtpColorsTab::refresh() {
    for (const auto& refresher : refreshers_) {
        refresher();
    }
}

void FtpColorsTab::resetAll() {
    for (int zone = 0; zone < ftpcolors::kZones; ++zone) {
        zones::setZoneColor(zone, std::nullopt);
    }
    zones::setFreeColor(std::nullopt);
}

// zone: 0 - 4, or -1 for free ride
QPushButton* FtpColorsTab::resetButton(int zone) {
    auto* button = new QPushButton(QStringLiteral("↺"));
    button->setFixedWidth(28);
    button->setToolTip(QStringLiteral("Back to the original"));
    connect(button, &QPushButton::clicked, this, [zone] {
        if (zone < 0) {
            zones::setFreeColor(std::nullopt);
        } else {
            zones::setZoneColor(zone, std::nullopt);
        }
    });
    refreshers_.append([button, zone] { button->setEnabled(zone < 0 ? !zones::isDefaultFree() : !zones::isDefaultZone(zone)); });
    return button;
}

// ---- The mockup window ----

GraphMock::GraphMock() {
    setMinimumSize(420, 260);
}

void GraphMock::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(this->rect()).adjusted(1, 1, -1, -1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QString::fromLatin1(kGraphBackground)));
    painter.drawRoundedRect(rect, 15, 15);
    constexpr double labelWidth = 110;
    const QList<std::pair<ftpcolors::GraphState, QString>> states = {{ftpcolors::GraphState::Paused, QStringLiteral("Paused")},
                                                                     {ftpcolors::GraphState::Upcoming, QStringLiteral("Upcoming")},
                                                                     {ftpcolors::GraphState::Past, QStringLiteral("Past")}};
    const double rowHeight = (rect.height() - 20) / states.size();
    const ftpcolors::ZoneRanges ranges = rangesFor(kDefaultFtp);
    // four bars a zone, from a quarter of the way through it to its end
    QList<std::pair<int, double>> samples;
    for (int zone = 0; zone < 5; ++zone) {
        const auto [start, end] = ftpcolors::zoneSpan(zone, ranges);
        for (int k = 1; k < 5; ++k) {
            samples.append({zone, start + (end - start) * k / 4});
        }
    }
    const double barWidth = (rect.width() - labelWidth - 30) / samples.size();
    painter.setFont(QFont(font().family(), 10));
    for (int row = 0; row < states.size(); ++row) {
        const double top = rect.top() + 10 + row * rowHeight;
        painter.setPen(QColor(QStringLiteral("#000000")));
        painter.drawText(QRectF(rect.left() + 12, top, labelWidth, rowHeight), Qt::AlignVCenter, states[row].second);
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < samples.size(); ++i) {
            const auto [zone, watts] = samples[i];
            const double height = (rowHeight - 12) * (0.3 + 0.175 * zone);
            const double left = rect.left() + labelWidth + 10 + i * barWidth;
            painter.setBrush(zones::graphColor(watts, ranges, states[row].first));
            painter.drawRect(QRectF(left, top + rowHeight - 6 - height, barWidth - 1, height));
        }
    }
}

FtpMockupWindow::FtpMockupWindow() {
    setWindowTitle(QStringLiteral("Layout Viewer FTP Colors"));
    // (the layout scale doesn't apply here)
    theme::setExempt(this);
    setFont(theme::defaultFont());
    auto* outer = new QVBoxLayout(this);

    // the FTP and power
    auto* controls = new QGridLayout;
    controls->addWidget(new QLabel(QStringLiteral("FTP")), 0, 0);
    ftpSpin_ = new QSpinBox;
    ftpSpin_->setRange(40, 500);
    ftpSpin_->setSuffix(QStringLiteral(" W"));
    ftpSpin_->setMinimumWidth(110);
    controls->addWidget(ftpSpin_, 0, 1);
    ftpSlider_ = new QSlider(Qt::Horizontal);
    ftpSlider_->setRange(40, 500);
    controls->addWidget(ftpSlider_, 0, 2);
    controls->addWidget(new QLabel(QStringLiteral("Power")), 1, 0);
    powerSpin_ = new QSpinBox;
    powerSpin_->setRange(0, 1000);
    powerSpin_->setSuffix(QStringLiteral(" W"));
    powerSpin_->setMinimumWidth(110);
    controls->addWidget(powerSpin_, 1, 1);
    powerSlider_ = new QSlider(Qt::Horizontal);
    powerSlider_->setRange(0, 1000);
    controls->addWidget(powerSlider_, 1, 2);
    powerNote_ = new QLabel;
    controls->addWidget(powerNote_, 1, 3);
    stopButtons_ = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("Point at")), 2, 0);
    controls->addLayout(stopButtons_, 2, 1, 1, 3);
    controls->setColumnStretch(2, 1);
    outer->addLayout(controls);
    for (const auto& [spin, slider] : {std::pair{ftpSpin_, ftpSlider_}, std::pair{powerSpin_, powerSlider_}}) {
        connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, &QSpinBox::valueChanged, this, &FtpMockupWindow::refresh);
    }

    // the parts
    auto* parts = new QGridLayout;
    gauge_ = new Gauge;
    AnalogGauge* widget = gauge_->gauge();
    widget->setNeedleColor(QColor(kGaugeRed));
    widget->setGaugeTheme(3);
    widget->setMouseTracking(false);
    widget->setEnableBarGraph(false);
    widget->units = QStringLiteral("Watts");
    gauge_->setMinimumSize(420, 420);
    parts->addWidget(titled(QStringLiteral("Power gauge (target and power ridden both at the power above)"), gauge_), 0, 0, 2, 1);

    auto* blocks = new QWidget;
    auto* blockColumn = new QVBoxLayout(blocks);
    blockColumn->setContentsMargins(0, 0, 0, 0);
    blockColumn->setSpacing(4);
    for (int i = 0; i < 8; ++i) {
        auto* label = new QLabel;
        label->setFixedHeight(36);
        label->setFont(QFont(theme::current().fonts.value(theme::kRoleMain).family, 13));
        blockColumn->addWidget(label);
        blockLabels_.append(label);
    }
    parts->addWidget(titled(QStringLiteral("Workout blocks"), blocks), 0, 1, 2, 1);

    graph_ = new GraphMock;
    parts->addWidget(titled(QStringLiteral("Workout graph bars"), graph_), 2, 0, 1, 2);
    outer->addLayout(parts, 1);

    connect(&zones::notifier(), &zones::Notifier::changed, this, &FtpMockupWindow::refresh);
    const int ftp = appState().ftp;
    const int start = 40 <= ftp && ftp <= 500 ? ftp : kDefaultFtp;
    ftpSpin_->setValue(start);
    powerSpin_->setValue(static_cast<int>(py::round(start * 0.92)));
    refresh();
}

QWidget* FtpMockupWindow::titled(const QString& title, QWidget* widget) {
    auto* box = new QGroupBox(title);
    auto* layout = new QVBoxLayout(box);
    layout->addWidget(widget);
    return box;
}

void FtpMockupWindow::refresh() {
    const int ftp = ftpSpin_->value(), power = powerSpin_->value();
    const ftpcolors::ZoneRanges ranges = rangesFor(ftp);
    const QString zone = ftpcolors::zoneOf(power, ranges);
    powerNote_->setText(QStringLiteral("%1% of FTP, %2").arg(py::fixed(power / static_cast<double>(ftp) * 100, 0), shortName(zone)));

    // "point at" buttons, one where each zone ends (at its color)
    while (QLayoutItem* item = stopButtons_->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    const ftpcolors::Palette palette = zones::palette();
    for (int z = 0; z < 5; ++z) {
        const double end = ftpcolors::zoneSpan(z, ranges).second;
        const int watts = static_cast<int>(py::round(end)) - (z < 4 ? 1 : 0);
        auto* button = new QPushButton(QStringLiteral("%1%").arg(py::fixed(end / ftp * 100, 0)));
        button->setToolTip(QStringLiteral("%1 W: the needle where %2 is its color").arg(watts).arg(shortName(QString::number(z))) +
                           (watts <= 1000 ? QString() : QStringLiteral(" (past the dial)")));
        button->setStyleSheet(QStringLiteral("QPushButton { border:2px solid %1; border-radius:4px; padding:2px 8px; }")
                                  .arg(palette[z].color));
        button->setEnabled(watts <= 1000);
        connect(button, &QPushButton::clicked, this, [this, watts] { powerSpin_->setValue(watts); });
        stopButtons_->addWidget(button);
    }
    stopButtons_->addStretch(1);

    // the gauge
    AnalogGauge* widget = gauge_->gauge();
    widget->setValueColors(zones::zoneStops(ranges));
    widget->setValue(power);
    const auto [c0, c1] = zones::gaugeArc(power, ranges);
    widget->setArc(power, {{0, QColor(QStringLiteral("#88") + c0)}, {.5, QColor(QStringLiteral("#88") + c1)}});
    widget->setOuterCircleBg({{0.25, QColor(0, 0, 0, 0)}, {0.18, QColor(QStringLiteral("#44") + zones::gaugeRing(power, ranges))}});
    widget->update();

    // the block labels: a single block a zone, a range and free ride
    QList<std::pair<int, int>> samples;
    for (const int pct : {25, 60, 100, 150, 200}) {
        samples.append({static_cast<int>(py::round(ftp * pct / 100.0)), 0});
    }
    samples.append({static_cast<int>(py::round(ftp * 0.5)), static_cast<int>(py::round(ftp * 1.1))});
    for (int i = 0; i < 6; ++i) {
        const auto [p0, p1] = samples[i];
        blockLabels_[i]->setText(i < 5 ? QStringLiteral("Zone %1:  %2 Watts").arg(i + 1).arg(p0)
                                       : QStringLiteral("Range:  %1-%2 Watts").arg(p0).arg(p1));
        blockLabels_[i]->setStyleSheet(zones::blockCss(p0, p1, ranges) + QStringLiteral("border-radius:15px; padding-left:14px;"));
        blockLabels_[i]->show();
    }
    blockLabels_[6]->setText(QStringLiteral("Free ride"));
    blockLabels_[6]->setStyleSheet(zones::freeRideCss() + QStringLiteral("border-radius:15px; padding-left:14px;"));
    blockLabels_[7]->hide();
    graph_->update();
}

}  // namespace awo
