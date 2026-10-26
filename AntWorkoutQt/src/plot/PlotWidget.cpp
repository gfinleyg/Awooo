#include "PlotWidget.hpp"

#include <QGraphicsGridLayout>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QResizeEvent>

#include <cmath>

namespace awo::plot {

// ---- LabelItem ----

LabelItem::LabelItem(QGraphicsItem* parent)
    : QGraphicsWidget(parent), item_(new QGraphicsTextItem(this)), size_(QStringLiteral("11pt")) {
    setText(QStringLiteral(" "));
}

void LabelItem::setText(const QString& text, const std::optional<QString>& color, const std::optional<QString>& size) {
    text_ = text;
    if (color) {
        color_ = *color;
    }
    if (size) {
        size_ = *size;
    }
    // (pyqtgraph's foreground grey without a color)
    const QColor shown = color_.isEmpty() ? QColor(150, 150, 150) : QColor(color_);
    QStringList options{QStringLiteral("color: ") + shown.name(QColor::HexArgb)};
    if (!size_.isEmpty()) {
        options.append(QStringLiteral("font-size: ") + size_);
    }
    item_->setHtml(QStringLiteral("<span style='%1'>%2</span>").arg(options.join(QStringLiteral("; ")), text));
    updateMin();
    resizeEvent(nullptr);
    updateGeometry();
}

QRectF LabelItem::itemRect() const {
    return item_->mapRectToParent(item_->boundingRect());
}

void LabelItem::updateMin() {
    const QRectF bounds = itemRect();
    setMinimumWidth(bounds.width());
    setMinimumHeight(bounds.height());
    updateGeometry();
}

QSizeF LabelItem::sizeHint(Qt::SizeHint which, const QSizeF&) const {
    const QRectF bounds = item_->mapRectToParent(item_->boundingRect());
    switch (which) {
        case Qt::MinimumSize:
        case Qt::PreferredSize:
            return bounds.size();
        case Qt::MaximumSize:
            return {-1, -1};
        default:
            return {0, 0};
    }
}

// centered in its space
void LabelItem::resizeEvent(QGraphicsSceneResizeEvent* event) {
    if (event != nullptr) {
        QGraphicsWidget::resizeEvent(event);
    }
    item_->setPos(0, 0);
    QRectF bounds = itemRect();
    bounds.moveCenter(QRectF(QPointF(0, 0), size()).center());
    item_->setPos(bounds.topLeft() - itemRect().topLeft());
}

// ---- PlotItem ----

PlotItem::PlotItem(QGraphicsItem* parent) : QGraphicsWidget(parent), layout_(new QGraphicsGridLayout) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout_->setContentsMargins(1, 1, 1, 1);
    layout_->setHorizontalSpacing(0);
    layout_->setVerticalSpacing(0);
    setLayout(layout_);
    vb_ = new ViewBox(this);
    layout_->addItem(vb_, 2, 1);
    for (const auto orientation : {AxisItem::Top, AxisItem::Bottom, AxisItem::Left, AxisItem::Right}) {
        setAxisItem(orientation, new AxisItem(orientation, this));
    }
    title_ = new LabelItem(this);
    layout_->addItem(title_, 0, 1);
    setTitle(std::nullopt);
    for (int i = 0; i < 4; ++i) {
        layout_->setRowPreferredHeight(i, 0);
        layout_->setRowMinimumHeight(i, 0);
        layout_->setRowSpacing(i, 0);
        layout_->setRowStretchFactor(i, 1);
    }
    for (int i = 0; i < 3; ++i) {
        layout_->setColumnPreferredWidth(i, 0);
        layout_->setColumnMinimumWidth(i, 0);
        layout_->setColumnSpacing(i, 0);
        layout_->setColumnStretchFactor(i, 1);
    }
    layout_->setRowStretchFactor(2, 100);
    layout_->setColumnStretchFactor(1, 100);
}

void PlotItem::setAxisItem(AxisItem::Orientation orientation, AxisItem* axis) {
    static const QPoint kCells[] = {{2, 0}, {2, 2}, {1, 1}, {3, 1}};  // Left, Right, Top, Bottom: (row, column)
    // shown: left and bottom, and any axis put in place of another
    const bool replacing = axes_[orientation] != nullptr;
    if (AxisItem* old = axes_[orientation]) {
        layout_->removeItem(old);
        old->linkToView(nullptr);
        delete old;
    }
    axis->setParentItem(this);
    axis->linkToView(vb_);
    axes_[orientation] = axis;
    layout_->addItem(axis, kCells[orientation].x(), kCells[orientation].y());
    // over images at 0; items to go over the axes go at 1 or more
    axis->setZValue(0.5);
    axis->setFlag(QGraphicsItem::ItemNegativeZStacksBehindParent);
    axis->setShown(orientation == AxisItem::Left || orientation == AxisItem::Bottom || replacing);
    updateGrid();
}

void PlotItem::setLabel(AxisItem::Orientation orientation, const QString& text,
                        const std::optional<AxisItem::Style>& style) {
    axes_[orientation]->setLabel(text, {}, style);
    showAxis(orientation);
}

void PlotItem::showAxis(AxisItem::Orientation orientation, bool show) {
    axes_[orientation]->setShown(show);
}

void PlotItem::setTitle(const std::optional<QString>& title, const std::optional<QString>& color,
                        const std::optional<QString>& size) {
    if (!title) {
        title_->setVisible(false);
        layout_->setRowFixedHeight(0, 0);
        title_->setMaximumHeight(0);
        return;
    }
    title_->setMaximumHeight(30);
    layout_->setRowFixedHeight(0, 30);
    title_->setVisible(true);
    title_->setText(*title, color, size);
}

void PlotItem::showGrid(std::optional<bool> x, std::optional<bool> y, std::optional<double> alpha) {
    if (x) {
        xGrid_ = *x;
    }
    if (y) {
        yGrid_ = *y;
    }
    if (alpha) {
        gridAlpha_ = static_cast<int>(std::clamp(*alpha, 0.0, 1.0) * 255);
    }
    updateGrid();
}

void PlotItem::updateGrid() {
    const std::optional<int> x = xGrid_ ? std::optional(gridAlpha_) : std::nullopt;
    const std::optional<int> y = yGrid_ ? std::optional(gridAlpha_) : std::nullopt;
    for (const auto orientation : {AxisItem::Top, AxisItem::Bottom}) {
        if (axes_[orientation] != nullptr) {
            axes_[orientation]->setGrid(x);
        }
    }
    for (const auto orientation : {AxisItem::Left, AxisItem::Right}) {
        if (axes_[orientation] != nullptr) {
            axes_[orientation]->setGrid(y);
        }
    }
}

// ---- PlotWidget ----

PlotWidget::PlotWidget(QWidget* parent) : QGraphicsView(parent), plotItem_(new PlotItem) {
    setCacheMode(CacheBackground);
    setBackgroundRole(QPalette::NoRole);
    setBackground(QColor(Qt::black));
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(NoAnchor);
    setResizeAnchor(AnchorViewCenter);
    setViewportUpdateMode(MinimalViewportUpdate);
    setMouseTracking(true);
    auto* scene = new QGraphicsScene(this);
    setScene(scene);
    scene->addItem(plotItem_);
    // (the plot laid out at the widget's size from the start, as pyqtgraph does, so a range set before it's shown is
    // padded for that size)
    fitPlot(size());
}

void PlotWidget::setBackground(const std::optional<QColor>& color) {
    setBackgroundBrush(color ? QBrush(*color) : QBrush(Qt::NoBrush));
}

// the plot fills the view, one scene unit a pixel
void PlotWidget::resizeEvent(QResizeEvent* event) {
    fitPlot(event->size());
}

void PlotWidget::fitPlot(const QSize& size) {
    const QRectF range(QPointF(0, 0), QSizeF(size));
    plotItem_->setGeometry(range);
    setSceneRect(range);
    resetTransform();
}

}  // namespace awo::plot
