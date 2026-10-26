#include "WorkoutSelectDialog.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <tuple>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AwoParser.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "LongVLabels.hpp"
#include "SavedSettings.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"
#include "ZwoConvert.hpp"

namespace awo {

const QString kWorkoutListFile = QStringLiteral("workoutPickerANT.json");
const QString kWorkoutFolder = QStringLiteral("awoWorkouts");

namespace {

// Awooo workouts, and Zwift workouts (ridden as the .awo they convert to)
bool isWorkoutFile(const QString& name) {
    const QString lower = name.toLower();
    return lower.endsWith(QLatin1String(".awo")) || lower.endsWith(QLatin1String(".zwo"));
}

QString normalized(const QString& path) {
    return QDir::fromNativeSeparators(path);
}

QJsonObject loadList() {
    QFile file(kWorkoutListFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

}  // namespace

QString pickerStyle(const QString& name) {
    return QStringLiteral(R"(
    #%1 {background-color:#9fc5d1;}
    QLabel {font: 900 12pt 'Orbitron';}
    QPushButton {font: 14pt 'Segoe UI Black'; border-radius: 10px; padding: 6px 12px;}
    QListWidget {font: 11pt 'Segoe UI'; color: black; background-color:#bdd6de; border: 2px solid black; border-radius: 6px;}
    QListWidget::item:selected {color: #000001;
        background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);}
    QSpinBox {font: 900 12pt 'Orbitron'; background-color:#bdd6de; min-width: 130px; min-height: 30px;}
    QProgressBar {font: 900 10pt 'Orbitron'; color: black; background-color:#bdd6de; border: 1px solid black; text-align: center;}
    QProgressBar::chunk {background-color:#1CB5E0;}
)")
        .arg(name);
}

WorkoutPreview previewWorkout(const QString& path) {
    const AppState& state = appState();
    AwoOptions options;
    options.ftp = state.ftp;
    const AwoResult result = readWorkoutFile(path, options);
    const QString filename = QFileInfo(path).fileName();
    for (const QString& warning : result.warnings) {
        log::event(QStringLiteral("SETUP"), QStringLiteral("%1: %2").arg(filename, warning));
    }
    WorkoutPreview preview;
    preview.ok = result.ok();
    if (!preview.ok) {
        const AwoError& error = result.errors.front();
        preview.error = error.line ? QStringLiteral("Line %1: %2").arg(error.line).arg(error.reason) : error.reason;
        return preview;
    }
    preview.workout = result.workout;
    preview.title = result.workout.title;
    preview.seconds = result.workout.seconds;
    preview.text = workoutText(result.workout.entries, state.targetUnit());
    return preview;
}

WorkoutPreview cachedPreview(const QString& path) {
    using Key = std::tuple<qint64, qint64, int, QString>;
    static QHash<QString, std::pair<Key, WorkoutPreview>> cache;
    const QFileInfo info(path);
    if (!info.exists()) {
        return previewWorkout(path);
    }
    const AppState& state = appState();
    const Key key{info.size(), info.lastModified().toMSecsSinceEpoch(), state.ftp, state.targetUnit()};
    const auto found = cache.constFind(path);
    if (found != cache.constEnd() && found->first == key) {
        return found->second;
    }
    // (an older read of the same file is replaced)
    const WorkoutPreview preview = previewWorkout(path);
    cache.insert(path, {key, preview});
    return preview;
}

QStringList folderWorkouts(const QString& folder) {
    QStringList names;
    for (const QString& name : QDir(folder).entryList(QDir::Files)) {
        if (isWorkoutFile(name)) {
            names.append(name);
        }
    }
    std::sort(names.begin(), names.end(),
              [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
    QStringList paths;
    for (const QString& name : names) {
        paths.append(normalized(QDir(folder).filePath(name)));
    }
    return paths;
}

WorkoutSelectDialog::WorkoutSelectDialog(QWidget* parent) : QDialog(parent) {
    auto* titleBar = framed(this, QStringLiteral("workoutSelectWindow"), QStringLiteral("   Pick a Workout   "), {}, false);
    theme::setSheet(this, pickerStyle(QStringLiteral("workoutSelectWindow")));
    resize(1300, 860);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);

    auto* addRow = new QHBoxLayout;
    struct ButtonSpec {
        QString text;
        void (WorkoutSelectDialog::*slot)();
        int key;
        QString label;
    };
    const QList<ButtonSpec> specs = {
        {QStringLiteral("Add Folder"), &WorkoutSelectDialog::addFolder, Qt::Key_D, QStringLiteral("D")},
        {QStringLiteral("Add Workouts"), &WorkoutSelectDialog::addFiles, Qt::Key_A, QStringLiteral("A")},
        {QStringLiteral("Remove"), &WorkoutSelectDialog::removeSelected, Qt::Key_Delete, QStringLiteral("Del")},
        {QStringLiteral("Clear"), &WorkoutSelectDialog::clearWorkouts, Qt::Key_C, QStringLiteral("C")},
        {QStringLiteral("Convert to .awo"), &WorkoutSelectDialog::convertSelected, Qt::Key_V, QStringLiteral("V")},
    };
    for (const ButtonSpec& spec : specs) {
        auto* button = new QPushButton(spec.text);
        connect(button, &QPushButton::clicked, this, spec.slot);
        addRow->addWidget(button);
        keys::add(button, spec.key, spec.label);
        // (only for a .zwo file)
        convert_ = button;
    }
    layout->addLayout(addRow);

    auto* middle = new QHBoxLayout;
    list_ = new QListWidget;
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) { showPreview(item); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
    middle->addWidget(list_, 1);
    auto* previewColumn = new QVBoxLayout;
    previewTitle_ = new QLabel;
    previewTitle_->setWordWrap(true);
    previewTitle_->setAlignment(Qt::AlignCenter);
    previewTitle_->setFixedWidth(500);
    previewColumn->addWidget(previewTitle_);
    // the blocks as the setup screen's workout list shows them (colored by power zone)
    preview_ = new LongVLabels(appState().ftpRanges);
    preview_->setFixedWidth(500);
    previewColumn->addWidget(preview_, 1);
    middle->addLayout(previewColumn);
    layout->addLayout(middle, 1);

    auto* options = new QHBoxLayout;
    options->addStretch();
    options->addWidget(new QLabel(QStringLiteral("Random workout of about")));
    randomMinutes_ = new QSpinBox;
    randomMinutes_->setRange(kMinRandomMinutes, kMaxRandomMinutes);
    randomMinutes_->setSuffix(QStringLiteral(" min"));
    options->addWidget(randomMinutes_);
    auto* randomButton = new QPushButton(QStringLiteral("Pick Random Workout"));
    connect(randomButton, &QPushButton::clicked, this, &WorkoutSelectDialog::pickRandom);
    options->addWidget(randomButton);
    options->addStretch();
    layout->addLayout(options);

    summary_ = new QLabel;
    summary_->setAlignment(Qt::AlignCenter);
    layout->addWidget(summary_);

    auto* buttons = new QHBoxLayout;
    ok_ = new QPushButton(QStringLiteral("Ride This Workout"));
    connect(ok_, &QPushButton::clicked, this, &WorkoutSelectDialog::accept);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &WorkoutSelectDialog::reject);
    buttons->addWidget(ok_);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    // keys (Ctrl shows them; Esc is the X and Cancel); Enter in the minutes box picks a random workout
    keys::add(randomButton, Qt::Key_R, QStringLiteral("R"));
    keys::Options inMinutes;
    inMinutes.field = randomMinutes_;
    keys::add(randomButton, keys::kEnter, inMinutes);
    keys::add(ok_, keys::kEnter, {QStringLiteral("Enter")});
    keys::Options up;
    up.label = QStringLiteral("↑ ↓");
    up.place = keys::Place::Inside;
    up.repeat = true;
    up.action = [this] { keys::stepList(list_, -1); };
    keys::add(list_, {Qt::Key_Up}, up);
    keys::Options down;
    down.repeat = true;
    down.action = [this] { keys::stepList(list_, 1); };
    keys::add(list_, {Qt::Key_Down}, down);

    // the list from last time (or the workouts folder), with the workout picked last time (or the loaded one) picked
    randomMinutes_->setValue(settings::load().value(QStringLiteral("workoutRandomMinutes")).toInt(60));
    const QJsonObject saved = loadList();
    QStringList files;
    // (a damaged list's entries that aren't file names are skipped)
    for (const QJsonValue& value : saved.value(QStringLiteral("files")).toArray()) {
        if (value.isString() && QFileInfo(value.toString()).isFile()) {
            files.append(value.toString());
        }
    }
    if (files.isEmpty() && QFileInfo(kWorkoutFolder).isDir()) {
        for (const QString& path : folderWorkouts(kWorkoutFolder)) {
            files.append(normalized(QFileInfo(path).absoluteFilePath()));
        }
    }
    addToList(files);
    const QString loaded = appState().workoutFilename;
    selectPath(loaded.isEmpty() ? saved.value(QStringLiteral("last")).toString() : loaded);
    updateSummary();
    // scaled for this screen, and kept on it
    layout::fitDialog(this);
}

void WorkoutSelectDialog::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Add a Folder of Workouts"), kWorkoutFolder);
    if (!folder.isEmpty()) {
        addToList(folderWorkouts(folder));
    }
}

void WorkoutSelectDialog::addFiles() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add Workouts"), kWorkoutFolder,
        QStringLiteral("Workout Files (*.awo *.zwo);; Awooo Workout Files (*.awo);; Zwift Workout Files (*.zwo);; "
                       "All Files (*)"));
    addToList(paths);
}

void WorkoutSelectDialog::addToList(const QStringList& paths) {
    QStringList listed = this->paths();
    QListWidgetItem* first = nullptr;
    for (QString path : paths) {
        path = normalized(path);
        if (listed.contains(path)) {
            continue;
        }
        listed.append(path);
        if (!previews_.contains(path)) {
            previews_.insert(path, cachedPreview(path));
        }
        auto* item = new QListWidgetItem(itemText(path));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        if (!previews_.value(path).ok) {
            item->setForeground(Qt::darkRed);
        }
        list_->addItem(item);
        if (!first) {
            first = item;
        }
    }
    if (first && !list_->currentItem()) {
        list_->setCurrentItem(first);
    }
    updateSummary();
}

QString WorkoutSelectDialog::itemText(const QString& path) const {
    const WorkoutPreview preview = previews_.value(path);
    const QString name = QFileInfo(path).fileName();
    if (!preview.ok) {
        return QStringLiteral("%1   (can't be used)").arg(name);
    }
    const QString title = preview.title.isEmpty() ? name : QStringLiteral("%1   (%2)").arg(preview.title, name);
    return QStringLiteral("%1   %2").arg(title, minimizedDuration(preview.seconds));
}

// the selected .zwo file written as an .awo file beside it, which is then added and picked
void WorkoutSelectDialog::convertSelected() {
    const QString zwoPath = selectedPath();
    if (!zwoPath.toLower().endsWith(QLatin1String(".zwo"))) {
        return;
    }
    const QString awoPath = normalized(awoPathFor(zwoPath));
    const QString awoName = QFileInfo(awoPath).fileName();
    if (QFileInfo::exists(awoPath) &&
        QMessageBox::question(this, QStringLiteral("Convert to .awo"),
                              QStringLiteral("%1 already exists. Replace it?").arg(awoName)) != QMessageBox::Yes) {
        return;
    }
    const ZwoConversion conversion = writeAwo(zwoPath, awoPath);
    if (!conversion.ok()) {
        QMessageBox::warning(this, QStringLiteral("Convert to .awo"),
                             QStringLiteral("Couldn't convert %1:\n%2").arg(QFileInfo(zwoPath).fileName(), conversion.error));
        return;
    }
    // read again: it may have been listed before, as an older conversion
    previews_.remove(awoPath);
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == awoPath) {
            delete list_->takeItem(i);
            break;
        }
    }
    addToList({awoPath});
    selectPath(awoPath);
    if (!conversion.warnings.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Convert to .awo"),
                                 QStringLiteral("Saved %1\n\n").arg(awoName) + conversion.warnings.join(u'\n'));
    }
}

void WorkoutSelectDialog::removeSelected() {
    if (QListWidgetItem* item = list_->currentItem()) {
        delete list_->takeItem(list_->row(item));
    }
    updateSummary();
}

void WorkoutSelectDialog::clearWorkouts() {
    list_->clear();
    updateSummary();
}

QStringList WorkoutSelectDialog::paths() const {
    QStringList paths;
    for (int i = 0; i < list_->count(); ++i) {
        paths.append(list_->item(i)->data(Qt::UserRole).toString());
    }
    return paths;
}

void WorkoutSelectDialog::selectPath(const QString& path) {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == path) {
            list_->setCurrentRow(i);
            list_->scrollToItem(list_->item(i), QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

QString WorkoutSelectDialog::selectedPath() const {
    QListWidgetItem* item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString WorkoutSelectDialog::previewTitle() const {
    return previewTitle_->text();
}

void WorkoutSelectDialog::showPreview(QListWidgetItem* item) {
    const QString path = item ? item->data(Qt::UserRole).toString() : QString();
    if (!item || !previews_.contains(path)) {
        layout::setText(previewTitle_, QString());
        preview_->setFullText(QString());
    } else if (const WorkoutPreview preview = previews_.value(path); preview.ok) {
        const QString name = preview.title.isEmpty() ? QFileInfo(path).fileName() : preview.title;
        layout::setText(previewTitle_, QStringLiteral("%1\n%2").arg(name, minimizedDuration(preview.seconds)));
        preview_->setFullText(preview.text);
    } else {
        layout::setText(previewTitle_, QStringLiteral("Can't be used:\n%1").arg(preview.error));
        preview_->setFullText(QString());
    }
    updateSummary();
}

// a random workout from the list that's about as long as asked (within kRandomTolerance), else the closest
void WorkoutSelectDialog::pickRandom() {
    const double target = randomMinutes_->value() * 60.0;
    QStringList usable;
    for (const QString& path : paths()) {
        if (previews_.value(path).ok) {
            usable.append(path);
        }
    }
    if (usable.isEmpty()) {
        return;
    }
    QStringList near;
    for (const QString& path : usable) {
        if (std::abs(previews_.value(path).seconds - target) <= kRandomTolerance * target) {
            near.append(path);
        }
    }
    if (!near.isEmpty()) {
        QStringList choices;
        for (const QString& path : near) {
            if (path != selectedPath()) {
                choices.append(path);
            }
        }
        if (choices.isEmpty()) {
            choices = near;
        }
        selectPath(choices.at(QRandomGenerator::global()->bounded(static_cast<int>(choices.size()))));
        return;
    }
    const auto closest = std::min_element(usable.begin(), usable.end(), [&](const QString& a, const QString& b) {
        return std::abs(previews_.value(a).seconds - target) < std::abs(previews_.value(b).seconds - target);
    });
    selectPath(*closest);
}

QString WorkoutSelectDialog::summary() const {
    return summary_->text();
}

void WorkoutSelectDialog::updateSummary() {
    const QStringList paths = this->paths();
    const auto unusable = std::count_if(paths.begin(), paths.end(), [this](const QString& p) { return !previews_.value(p).ok; });
    QString text = QStringLiteral("%1 workout%2").arg(paths.size()).arg(paths.size() != 1 ? QStringLiteral("s") : QString());
    if (unusable) {
        text += QStringLiteral("   ·   %1 can't be used").arg(unusable);
    }
    layout::setText(summary_, text);
    const QString path = selectedPath();
    const bool ok = !path.isEmpty() && previews_.value(path).ok;
    ok_->setEnabled(ok);
    convert_->setEnabled(ok && path.toLower().endsWith(QLatin1String(".zwo")));
}

void WorkoutSelectDialog::accept() {
    if (!ok_->isEnabled()) {
        return;
    }
    save();
    QDialog::accept();
}

void WorkoutSelectDialog::reject() {
    save();
    QDialog::reject();
}

void WorkoutSelectDialog::save() {
    QSaveFile file(kWorkoutListFile);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonArray files;
        for (const QString& path : paths()) {
            files.append(path);
        }
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("files"), files}, {QStringLiteral("last"), selectedPath()}})
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
    settings::update({{QStringLiteral("workoutRandomMinutes"), randomMinutes_->value()}});
}

}  // namespace awo
