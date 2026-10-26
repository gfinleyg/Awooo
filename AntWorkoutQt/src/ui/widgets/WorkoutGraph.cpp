#include "WorkoutGraph.hpp"

#include <QGraphicsGridLayout>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QRegularExpression>
#include <QTime>

#include <cmath>

#include "PyCompat.hpp"
#include "Theme.hpp"
#include "Units.hpp"

namespace awo {

using plot::AxisItem;

namespace {

constexpr AxisItem::Orientation kAxes[] = {AxisItem::Left, AxisItem::Right, AxisItem::Bottom};

// a style option set in place (or added at the end)
void setStyleValue(AxisItem::Style& style, const QString& key, const QString& value) {
    for (auto& entry : style) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    style.append({key, value});
}

std::optional<QString> styleValue(const AxisItem::Style& style, const QString& key) {
    for (const auto& [name, value] : style) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

QString hourMinuteFromSecond(double seconds) {
    const QTime time = QTime(0, 0).addSecs(static_cast<int>(std::floor(seconds)));
    return seconds > 3590 ? time.toString(QStringLiteral("h'h 'm'm'")) : time.toString(QStringLiteral("m'm"));
}

QString hourMinuteSecondFromSecond(double seconds) {
    const QTime time = QTime(0, 0).addSecs(static_cast<int>(std::floor(seconds)));
    return seconds > 3590 ? time.toString(QStringLiteral("h'h 'm'm 'ss's'"))
                          : time.toString(QStringLiteral("m'm 'ss's'"));
}

QStringList TimeAxisItem::tickStrings(const QList<double>& values, double, double spacing) const {
    QStringList strings;
    for (const double value : values) {
        // ticks closer than a minute apart (a zoomed in synopsis) need the seconds too
        strings.append(spacing < 60 ? hourMinuteSecondFromSecond(std::floor(value))
                                    : hourMinuteFromSecond(std::floor(value)));
    }
    return strings;
}

WorkoutGraph::WorkoutGraph(QWidget* parent) : PlotWidget(parent) {
    axisStyles_ = {{QStringLiteral("color"), QStringLiteral("#000000")}, {QStringLiteral("font-size"), QStringLiteral("18px")}};
    plot::PlotItem* plot = plotScreen();
    setBackground(QColor(QStringLiteral("#bdd6de")));
    setXRange(0, 20);
    plot->setTitle(QStringLiteral("Workout Stats"), QStringLiteral("#000000"), QStringLiteral("20pt"));
    plot->setLabel(AxisItem::Left, QStringLiteral("Power (Watts)"), axisStyles_);
    axis(AxisItem::Left)->setTickSpacing(100, 20);
    axis(AxisItem::Left)->setTickPen(plot::mkPen(QColor(QStringLiteral("#1CB5E0"))));
    setSpeedLabel();
    axis(AxisItem::Right)->setTickSpacing(50, 50);
    axis(AxisItem::Right)->setTickPen(plot::mkPen(QColor(QStringLiteral("#dadbde"))));
    plot->setAxisItem(AxisItem::Bottom, new TimeAxisItem(AxisItem::Bottom));
    axis(AxisItem::Bottom)->setTickSpacing(60, 60);
    plot->setLabel(AxisItem::Bottom, QStringLiteral("Time (minutes)"), axisStyles_);
    plot->showGrid(true, true);

    // the power, and the heart rate / cadence / speed, views over the plot's
    powerVBox_ = new plot::ViewBox;
    scene()->addItem(powerVBox_);
    axis(AxisItem::Left)->linkToView(powerVBox_);
    powerVBox_->setXLink(vb());
    powerVBox_->setYRange(0, 400);
    hrCadenceVBox_ = new plot::ViewBox;
    scene()->addItem(hrCadenceVBox_);
    axis(AxisItem::Right)->linkToView(hrCadenceVBox_);
    hrCadenceVBox_->setXLink(vb());
    hrCadenceVBox_->setYRange(0, 220);

    connect(&Units::current(), &Units::changed, this, &WorkoutGraph::setSpeedLabel);
    connect(&theme::notifier(), &theme::Notifier::changed, this, &WorkoutGraph::applyTheme);
}

void WorkoutGraph::setSpeedLabel() {
    plotScreen()->setLabel(
        AxisItem::Right,
        QStringLiteral("Heart Rate(BPM) / Cadence(RPM) / Speed(%1)").arg(Units::current().speedUnit()), axisStyles_);
}

void WorkoutGraph::updateViews() {
    const QRectF geometry = vb()->sceneBoundingRect();
    for (plot::ViewBox* view : {powerVBox_, hrCadenceVBox_}) {
        view->setGeometry(geometry);
        // (the link again, now the two are the same shape)
        view->setXLink(vb());
    }
}

// AppTheme.theme_graph: the title and axes ("Workout Stats", "Power (Watts)", the time ticks, ...) in the Text font
// row's font, size and weight, and the theme's colors
void WorkoutGraph::applyTheme() {
    const theme::Theme& look = theme::current();
    const theme::FontSettings settings = look.fonts.value(theme::kRoleMain);
    const double scale = theme::widgetScale(this);
    const double factor = settings.scale / 100. * scale;
    plot::LabelItem* title = plotScreen()->titleLabel();
    if (!titleLook_) {
        titleLook_ = TitleLook{title->text(), title->fontSize().isEmpty() ? QStringLiteral("20pt") : title->fontSize(),
                               title->color()};
    }
    // a graph with a title has its axis labels at the title's size too, unless it sets its own (axisTitlePoints);
    // the tick numbers keep their own size
    std::optional<double> titlePoints;
    if (!titleLook_->text.isEmpty() && title->isVisible()) {
        QString size = titleLook_->size;
        while (size.endsWith(u'p') || size.endsWith(u't')) {
            size.chop(1);
        }
        titlePoints = size.toDouble() * factor;
        title->item()->setFont(look.mapFont(QFont(QStringLiteral("Orbitron")), scale));
        title->setText(titleLook_->text, titleLook_->color.isEmpty() ? QString() : look.mapHex(titleLook_->color),
                       py::fixed(*titlePoints, 1) + QStringLiteral("pt"));
        // the title's row as tall as the title (pyqtgraph pins it at 30px, which cuts off a bigger one)
        const int height = static_cast<int>(std::ceil(title->itemRect().height()));
        title->setMaximumHeight(height);
        plotScreen()->gridLayout()->setRowFixedHeight(0, height);
    }
    const std::optional<double> labelPoints =
        axisTitlePoints ? std::optional(*axisTitlePoints * factor) : titlePoints;
    // the tick numbers: pyqtgraph's own grey unless the theme changes black, then that
    const auto black = look.colors.constFind(QStringLiteral("#000000"));
    for (const AxisItem::Orientation orientation : kAxes) {
        AxisItem* axis = this->axis(orientation);
        std::optional<AxisLook>& saved = axisLooks_[orientation];
        if (!saved) {
            saved = AxisLook{axis->tickFont().value_or(font()), axis->labelText(), axis->labelUnits(),
                             axis->labelStyle(), axis->textPen(), axis->pen(), axis->ownTickPen()};
        }
        axis->setTextPen(black != look.colors.constEnd() ? QPen(QColor(*black)) : saved->textPen);
        axis->setPen(theme::themedPen(saved->linePen));
        if (saved->tickPen) {
            axis->setTickPen(theme::themedPen(*saved->tickPen));
        }
        AxisItem::Style labelStyle = saved->labelStyle;
        if (const auto color = styleValue(labelStyle, QStringLiteral("color"))) {
            setStyleValue(labelStyle, QStringLiteral("color"), look.mapHex(*color));
        }
        QFont font(saved->tickFont);
        font.setFamily(settings.family);
        if (saved->tickFont.pointSizeF() > 0) {
            font.setPointSizeF(saved->tickFont.pointSizeF() * factor);
        }
        if (settings.weight != QLatin1String("Original")) {
            font.setWeight(static_cast<QFont::Weight>(settings.weight.toInt()));
        }
        axis->setTickFont(font);
        if (saved->labelText.isEmpty()) {
            continue;
        }
        AxisItem::Style style = labelStyle;
        setStyleValue(style, QStringLiteral("font-family"), settings.family);
        static const QRegularExpression pxSize(QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)px)"));
        const QRegularExpressionMatch match =
            pxSize.match(styleValue(labelStyle, QStringLiteral("font-size")).value_or(QString()));
        if (labelPoints) {
            setStyleValue(style, QStringLiteral("font-size"), py::fixed(*labelPoints, 1) + QStringLiteral("pt"));
        } else if (match.hasMatch()) {
            setStyleValue(style, QStringLiteral("font-size"),
                          py::fixed(match.captured(1).toDouble() * factor, 1) + QStringLiteral("px"));
        }
        if (settings.weight != QLatin1String("Original")) {
            setStyleValue(style, QStringLiteral("font-weight"), settings.weight);
        }
        QFont labelFont(font);
        if (labelPoints) {
            labelFont.setPointSizeF(*labelPoints);
        }
        axis->label()->setFont(labelFont);
        axis->setLabel(saved->labelText, saved->labelUnits, style);
    }
}

}  // namespace awo
