#include "ThemeSelectDialog.hpp"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include "AppLayout.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "FtpColors.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "PreviewRide.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "WorkoutSelectDialog.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace themepicker {

const QStringList kSwatchColors = {QStringLiteral("#28275e"), QStringLiteral("#9fc5d1"), QStringLiteral("#bdd6de"),
                                   QStringLiteral("#1cb5e0"), QStringLiteral("#000000")};
const QString kAdjustedMark = QStringLiteral("  · adjusted");

// Python's str.title(): each word's first letter up, the rest down (a word: letters after anything that isn't one)
QString titleCase(const QString& text) {
    QString out;
    bool previousCased = false;
    for (const QChar c : text) {
        if (c.isLetter()) {
            out += previousCased ? c.toLower() : c.toUpper();
            previousCased = true;
        } else {
            out += c;
            previousCased = false;
        }
    }
    return out;
}

QString displayName(const QString& path) {
    static const QRegularExpression leadingNumber(QStringLiteral("^\\d+[-_ ]*"));
    // (os.path.splitext: only the last extension)
    const QString file = QFileInfo(path).fileName();
    QString name = file.lastIndexOf(u'.') > 0 ? file.left(file.lastIndexOf(u'.')) : file;
    name.remove(leadingNumber);
    name = titleCase(name.replace(u'-', u' ').replace(u'_', u' '));
    return name.isEmpty() ? file : name;
}

}  // namespace themepicker

using namespace themepicker;

namespace {

const QString kPickerExtraStyle = QStringLiteral(R"(
    QListWidget:focus {border: 3px solid #1CB5E0;}
    QLabel#themePreview {font: 900 14pt 'Orbitron'; background-color: transparent;}
)");
constexpr int kListWidth = 440;
constexpr int kSwatchSize[2] = {110, 26};
const QMap<QString, QString> kBaseText = {{QStringLiteral("colors"), QStringLiteral("Base Theme's Colors")},
                                          {QStringLiteral("ftpColors"), QStringLiteral("Base Theme's Gradients")}};
const QString kBaseTip = QStringLiteral("The theme's own, with no colors or gradients file on top of it");
const QStringList kPartOrder = {QStringLiteral("colors"), QStringLiteral("ftpColors")};
constexpr int kPathRole = Qt::UserRole;

bool sameFile(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return a.isEmpty() && b.isEmpty();
    }
    const auto norm = [](const QString& p) { return QDir::cleanPath(QFileInfo(p).absoluteFilePath()).toLower(); };
    return norm(a) == norm(b);
}

QSize swatchSize() {
    const double scale = theme::uiScale();
    return {std::max(1, static_cast<int>(py::round(kSwatchSize[0] * scale))),
            std::max(1, static_cast<int>(py::round(kSwatchSize[1] * scale)))};
}

// an icon of stripes in these colors
QIcon swatch(const QStringList& colors) {
    const QSize size = swatchSize();
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < colors.size(); ++i) {
        const int left = static_cast<int>(py::round(i * size.width() / static_cast<double>(colors.size())));
        const int right = static_cast<int>(py::round((i + 1) * size.width() / static_cast<double>(colors.size())));
        painter.setBrush(QColor(colors[i]));
        painter.drawRect(left, 0, right - left, size.height());
    }
    painter.setPen(QColor(QStringLiteral("#000000")));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(0, 0, size.width() - 1, size.height() - 1);
    painter.end();
    return QIcon(pixmap);
}

// an icon of the zones' fades, each pale to its color
QIcon gradientSwatch(const ftpcolors::Palette& fitted) {
    const QSize size = swatchSize();
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(Qt::NoPen);
    const int count = static_cast<int>(fitted.size());
    for (int i = 0; i < count; ++i) {
        const int left = static_cast<int>(py::round(i * size.width() / static_cast<double>(count)));
        const int right = static_cast<int>(py::round((i + 1) * size.width() / static_cast<double>(count)));
        QLinearGradient gradient(left, 0, right, 0);
        gradient.setColorAt(0, QColor(fitted[i].pale));
        gradient.setColorAt(1, QColor(fitted[i].color));
        painter.setBrush(gradient);
        painter.drawRect(left, 0, right - left, size.height());
    }
    painter.setPen(QColor(QStringLiteral("#000000")));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(0, 0, size.width() - 1, size.height() - 1);
    painter.end();
    return QIcon(pixmap);
}

// a colors file's main colors, or a gradients file's zone fades as they look on the current theme
QIcon partSwatch(const QString& part, const QJsonObject& value) {
    if (part == u"colors") {
        QHash<QString, QString> colors;
        for (auto it = value.begin(); it != value.end(); ++it) {
            colors.insert(it.key().toLower(), it.value().toString());
        }
        QStringList shown;
        for (const QString& c : kSwatchColors) {
            shown.append(colors.value(c, c));
        }
        return swatch(shown);
    }
    return gradientSwatch(zones::palette(ftpcolors::filePalette(value), theme::current().colors));
}

// a gradients item's name and tip, marked when its colors were moved to stand out on the current theme
void showAdjusted(QListWidgetItem* item, const QString& name, QString tip, const QJsonObject& value) {
    const QList<int> moved = zones::adjustedZones(ftpcolors::filePalette(value), theme::current().colors);
    item->setText(name + (moved.isEmpty() ? QString() : kAdjustedMark));
    if (!moved.isEmpty()) {
        QStringList numbers;
        for (const int z : moved) {
            numbers.append(QString::number(z + 1));
        }
        tip += QStringLiteral("\nAdjusted for these colors: ") +
               (moved.size() == 1 ? QStringLiteral("zone ") : QStringLiteral("zones ")) + numbers.join(QStringLiteral(", ")) +
               QStringLiteral(" moved to stand out on the background");
    }
    item->setToolTip(tip);
}

// the part a colors/gradients file holds (throws ThemeError for a bad one)
QJsonObject readPart(const QString& path, const QString& part) {
    const QJsonObject data = theme::readFile(path);
    if (!data.value(part).isObject()) {
        throw theme::ThemeError(QStringLiteral("not a %1 file").arg(theme::kParts.value(part)).toStdString());
    }
    return data.value(part).toObject();
}

bool sameSnapshot(const theme::Snapshot& a, const theme::Snapshot& b) {
    return a.data == b.data && a.parts == b.parts && a.file == b.file;
}

}  // namespace

ThemeSelectDialog::ThemeSelectDialog(QWidget* parent, bool preview) : QDialog(parent), wantPreview_(preview) {
    auto* titleBar = framed(this, QStringLiteral("themeSelectWindow"), QStringLiteral("   Pick a Theme   "), {}, false);
    theme::setSheet(this, pickerStyle(QStringLiteral("themeSelectWindow")) + kPickerExtraStyle);
    original_ = theme::snapshot();
    appliedFromTheme();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    auto* addRow = new QHBoxLayout;
    auto* loadButton = new QPushButton(QStringLiteral("Load Theme File"));
    connect(loadButton, &QPushButton::clicked, this, &ThemeSelectDialog::loadFile);
    loadButton->setToolTip(QStringLiteral("A theme, colors or gradients .json from anywhere (e.g. saved from LayoutViewer)"));
    addRow->addWidget(loadButton);
    auto* baseButton = new QPushButton(QStringLiteral("Base Theme"));
    connect(baseButton, &QPushButton::clicked, this, &ThemeSelectDialog::pickBase);
    baseButton->setToolTip(kBaseTip);
    addRow->addWidget(baseButton);
    layout->addLayout(addRow);

    auto* middle = new QHBoxLayout;
    auto* listColumn = new QVBoxLayout;
    for (const auto& [part, heading] : {std::pair{QStringLiteral("colors"), QStringLiteral("Theme")},
                                        std::pair{QStringLiteral("ftpColors"), QStringLiteral("Gradients")}}) {
        auto* label = new QLabel(heading);
        label->setFixedWidth(kListWidth);
        listColumn->addWidget(label);
        auto* partList = new QListWidget;
        partList->setFixedWidth(kListWidth);
        partList->setSelectionMode(QAbstractItemView::SingleSelection);
        partList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        partList->setIconSize(swatchSize());
        connect(partList, &QListWidget::currentItemChanged, this, [this, partList] {
            activeList_ = partList;
            applyTimer_->start();
        });
        connect(partList, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
        listColumn->addWidget(partList, 1);
        lists_.insert(part, partList);
    }
    activeList_ = lists_[QStringLiteral("colors")];
    middle->addLayout(listColumn);
    // the workout screen in the look picked (a picture of it, drawn offscreen)
    previewLabel_ = new QLabel(QStringLiteral("Drawing the workout screen..."));
    previewLabel_->setObjectName(QStringLiteral("themePreview"));
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    middle->addWidget(previewLabel_, 1);
    layout->addLayout(middle, 1);

    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setAlignment(Qt::AlignCenter);
    layout->addWidget(summary_);

    auto* buttons = new QHBoxLayout;
    ok_ = new QPushButton(QStringLiteral("Use This Look"));
    connect(ok_, &QPushButton::clicked, this, &ThemeSelectDialog::accept);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &ThemeSelectDialog::reject);
    buttons->addWidget(ok_);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    applyTimer_ = new QTimer(this);
    applyTimer_->setSingleShot(true);
    applyTimer_->setInterval(kApplyDelayMs);
    connect(applyTimer_, &QTimer::timeout, this, &ThemeSelectDialog::applyChoice);

    // keys (Ctrl shows them; Esc is the X and Cancel): Up/Down move in a list, Left/Right go from one list to the other
    keys::add(loadButton, Qt::Key_L, QStringLiteral("L"));
    keys::add(baseButton, Qt::Key_B, QStringLiteral("B"));
    keys::add(ok_, keys::kEnter, {QStringLiteral("Enter")});
    for (QListWidget* partList : std::as_const(lists_)) {
        keys::Options inList;
        inList.field = partList;
        inList.action = [this] { switchList(); };
        keys::add(partList, {Qt::Key_Left, Qt::Key_Right}, inList);
    }
    keys::Options upDown;
    upDown.label = QStringLiteral("↑ ↓");
    upDown.place = keys::Place::Inside;
    keys::add(lists_[QStringLiteral("colors")], {}, upDown);
    keys::Options leftRight;
    leftRight.label = QStringLiteral("← →");
    leftRight.place = keys::Place::Inside;
    leftRight.action = [this] { switchList(); };
    keys::add(lists_[QStringLiteral("ftpColors")], {Qt::Key_Left, Qt::Key_Right}, leftRight);
    keys::Options up;
    up.repeat = true;
    up.action = [this] { keys::stepList(activeList_, -1); };
    keys::add(lists_[QStringLiteral("colors")], {Qt::Key_Up}, up);
    keys::Options down;
    down.repeat = true;
    down.action = [this] { keys::stepList(activeList_, 1); };
    keys::add(lists_[QStringLiteral("colors")], {Qt::Key_Down}, down);

    // the files to pick from, the ones in use picked
    for (const QString& part : kPartOrder) {
        fillList(part);
    }
    updateSummary();
    // scaled for this screen, then filling it
    layout::fitDialog(this);
    if (QScreen* screen = (parent ? parent : this)->screen()) {
        setGeometry(screen->availableGeometry());
    }
}

ThemeSelectDialog::~ThemeSelectDialog() = default;

void ThemeSelectDialog::appliedFromTheme() {
    const QMap<QString, QString> files = theme::partFiles();
    applied_.clear();
    for (const QString& part : kPartOrder) {
        applied_.insert(part, files.value(part));
    }
}

// ---- The lists ----

void ThemeSelectDialog::fillList(const QString& part) {
    QListWidget* partList = lists_[part];
    const QSignalBlocker blocker(partList);
    partList->clear();
    auto* base = new QListWidgetItem(kBaseText[part]);
    base->setData(kPathRole, QString());
    base->setToolTip(kBaseTip);
    base->setIcon(partSwatch(part, theme::basePart(part)));
    if (part == u"ftpColors") {
        showAdjusted(base, kBaseText[part], kBaseTip, theme::basePart(part));
    }
    partList->addItem(base);
    const QDir folder(theme::partDir(part));
    QStringList paths;
    if (folder.exists()) {
        QStringList names = folder.entryList(QDir::Files);
        std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
        for (const QString& name : names) {
            if (name.toLower().endsWith(QStringLiteral(".json"))) {
                paths.append(folder.filePath(name));
            }
        }
    }
    const QString inUse = applied_.value(part);
    if (!inUse.isEmpty() && std::none_of(paths.begin(), paths.end(), [&](const QString& p) { return sameFile(inUse, p); })) {
        // (loaded from somewhere else)
        paths.append(inUse);
    }
    for (const QString& path : paths) {
        addItem(part, path);
    }
    QListWidgetItem* current = itemFor(part, inUse);
    partList->setCurrentItem(current ? current : base);
}

QListWidgetItem* ThemeSelectDialog::addItem(const QString& part, const QString& path) {
    auto* item = new QListWidgetItem(displayName(path));
    item->setData(kPathRole, path);
    item->setToolTip(path);
    try {
        const QJsonObject value = readPart(path, part);
        item->setIcon(partSwatch(part, value));
        if (part == u"ftpColors") {
            showAdjusted(item, displayName(path), path, value);
        }
    } catch (const std::exception& e) {
        item->setForeground(Qt::darkRed);
        item->setToolTip(QStringLiteral("%1\nCan't be used: %2").arg(path, QString::fromUtf8(e.what())));
    }
    lists_[part]->addItem(item);
    return item;
}

// the gradients' swatches and marks again, for the colors just picked (they're fitted to its background)
void ThemeSelectDialog::refreshGradientSwatches() {
    QListWidget* partList = lists_[QStringLiteral("ftpColors")];
    for (int row = 0; row < partList->count(); ++row) {
        QListWidgetItem* item = partList->item(row);
        const QString path = item->data(kPathRole).toString();
        QJsonObject value;
        try {
            value = path.isEmpty() ? theme::basePart(QStringLiteral("ftpColors")) : readPart(path, QStringLiteral("ftpColors"));
        } catch (const std::exception&) {
            continue;
        }
        item->setIcon(partSwatch(QStringLiteral("ftpColors"), value));
        showAdjusted(item, path.isEmpty() ? kBaseText[QStringLiteral("ftpColors")] : displayName(path),
                     path.isEmpty() ? kBaseTip : path, value);
    }
}

QListWidgetItem* ThemeSelectDialog::itemFor(const QString& part, const QString& path) const {
    QListWidget* partList = lists_[part];
    for (int row = 0; row < partList->count(); ++row) {
        if (sameFile(partList->item(row)->data(kPathRole).toString(), path)) {
            return partList->item(row);
        }
    }
    return nullptr;
}

// the file's row picked (without trying it: it's applied already)
void ThemeSelectDialog::select(const QString& part, const QString& path) {
    QListWidget* partList = lists_[part];
    QListWidgetItem* item = itemFor(part, path);
    if (!item && !path.isEmpty()) {
        item = addItem(part, path);
    }
    {
        const QSignalBlocker blocker(partList);
        partList->setCurrentItem(item);
    }
    if (item) {
        partList->scrollToItem(item);
    }
}

void ThemeSelectDialog::switchList() {
    QListWidget* other = activeList_ == lists_[QStringLiteral("colors")] ? lists_[QStringLiteral("ftpColors")]
                                                                          : lists_[QStringLiteral("colors")];
    activeList_ = other;
    other->setFocus();
    if (other->currentItem()) {
        other->scrollToItem(other->currentItem());
    }
}

// ---- Trying a choice on ----

void ThemeSelectDialog::applyChoice() {
    bool changed = false;
    for (const QString& part : kPartOrder) {
        QListWidgetItem* item = lists_[part]->currentItem();
        if (!item) {
            continue;
        }
        const QString path = item->data(kPathRole).toString();
        if (sameFile(path, applied_.value(part))) {
            continue;
        }
        QString kind;
        try {
            if (path.isEmpty()) {
                theme::dropPart(part);
                kind = part;
            } else {
                kind = theme::loadFile(path);
            }
        } catch (const std::exception& e) {
            log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load %1 %2: %3")
                                                    .arg(theme::kParts.value(part), path, QString::fromUtf8(e.what())));
            item->setForeground(Qt::darkRed);
            item->setToolTip(QStringLiteral("%1\nCan't be used: %2").arg(path, QString::fromUtf8(e.what())));
            continue;
        }
        changed = true;
        appliedFromTheme();
        // (a whole theme in the colors or gradients folder: the new base theme)
        if (kind == u"theme") {
            for (const QString& p : kPartOrder) {
                fillList(p);
            }
            break;
        }
        if (part == u"colors") {
            refreshGradientSwatches();
        }
    }
    if (changed) {
        redrawPreview();
        updateSummary();
    }
}

void ThemeSelectDialog::pickBase() {
    for (QListWidget* partList : std::as_const(lists_)) {
        partList->setCurrentRow(0);
    }
    applyTimer_->stop();
    applyChoice();
}

void ThemeSelectDialog::loadFile() {
    const QString folder = QDir(theme::themesDir()).exists() ? theme::themesDir() : QDir::currentPath();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Theme, Colors or Gradients"), folder,
                                                      QStringLiteral("Theme files (*.json)"));
    if (!path.isEmpty()) {
        loadPath(path);
    }
}

void ThemeSelectDialog::loadPath(const QString& path) {
    applyTimer_->stop();
    // (anything still waiting first, so the file goes on top of it)
    applyChoice();
    QString kind;
    try {
        kind = theme::loadFile(path);
    } catch (const std::exception& e) {
        log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load theme %1: %2").arg(path, QString::fromUtf8(e.what())));
        summary_->setText(QStringLiteral("Couldn't load %1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what())));
        return;
    }
    appliedFromTheme();
    if (kind == u"theme") {
        // a new base theme: its own colors and gradients, the swatches for them redrawn
        for (const QString& part : kPartOrder) {
            fillList(part);
        }
    } else {
        select(kind, path);
        if (kind == u"colors") {
            refreshGradientSwatches();
        }
    }
    redrawPreview();
    updateSummary();
}

QString ThemeSelectDialog::summary() const {
    return summary_->text();
}

void ThemeSelectDialog::updateSummary() {
    const QString themeFile = theme::themeFile();
    QStringList parts = {themeFile.isEmpty() ? QStringLiteral("Base theme: the app's default")
                                             : QStringLiteral("Base theme: ") + displayName(themeFile)};
    for (const auto& [part, what] : {std::pair{QStringLiteral("colors"), QStringLiteral("Colors")},
                                     std::pair{QStringLiteral("ftpColors"), QStringLiteral("Gradients")}}) {
        const QString path = applied_.value(part);
        parts.append(what + QStringLiteral(": ") + (path.isEmpty() ? QStringLiteral("the base theme's") : displayName(path)));
    }
    summary_->setText(parts.join(QStringLiteral("   ·   ")));
}

// ---- The preview ----

void ThemeSelectDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    activeList_->setFocus();
    if (!preview_ && wantPreview_) {
        // once the window's up (riding the preview workout takes a moment)
        QTimer::singleShot(0, this, &ThemeSelectDialog::buildPreview);
    }
}

void ThemeSelectDialog::buildPreview() {
    if (preview_ || !isVisible()) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const QScreen* screen = this->screen();
        preview_ = std::make_unique<RidePreview>(screen ? screen->availableGeometry().size() : QSize(1920, 1040));
    } catch (const std::exception& e) {
        // (the picker still works without it)
        log::event(QStringLiteral("ERROR"), QStringLiteral("Theme picker: couldn't build the workout screen preview: %1")
                                                .arg(QString::fromUtf8(e.what())));
        previewLabel_->setText(QStringLiteral("The workout screen preview couldn't be drawn"));
        QApplication::restoreOverrideCursor();
        return;
    }
    QApplication::restoreOverrideCursor();
    redrawPreview();
}

void ThemeSelectDialog::redrawPreview() {
    if (!preview_) {
        return;
    }
    previewPicture_ = preview_->picture();
    showPicture();
}

void ThemeSelectDialog::showPicture() {
    if (previewPicture_.isNull()) {
        return;
    }
    const double dpr = devicePixelRatioF();
    QPixmap picture = previewPicture_.scaled(previewLabel_->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    picture.setDevicePixelRatio(dpr);
    previewLabel_->setPixmap(picture);
}

void ThemeSelectDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    showPicture();
}

// ---- Done ----

void ThemeSelectDialog::accept() {
    if (applyTimer_->isActive()) {
        applyTimer_->stop();
        applyChoice();
    }
    QJsonObject parts;
    const QMap<QString, QString> files = theme::partFiles();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        parts.insert(it.key(), it.value());
    }
    settings::update({{QStringLiteral("themeFile"), theme::themeFile().isEmpty() ? QJsonValue() : QJsonValue(theme::themeFile())},
                      {QStringLiteral("themePartFiles"), parts}});
    log::event(QStringLiteral("SETUP"), QStringLiteral("Theme picked: ") + summary_->text());
    closePreview();
    QDialog::accept();
}

void ThemeSelectDialog::reject() {
    applyTimer_->stop();
    if (!sameSnapshot(theme::snapshot(), original_)) {
        theme::restore(original_);
    }
    closePreview();
    QDialog::reject();
}

void ThemeSelectDialog::closePreview() {
    preview_.reset();
}

}  // namespace awo
