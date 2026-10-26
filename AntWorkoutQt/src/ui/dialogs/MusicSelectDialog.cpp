#include "MusicSelectDialog.hpp"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#include "AnalysisCache.hpp"
#include "AppLayout.hpp"
#include "CadenceWorkout.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "KeyShortcuts.hpp"
#include "Playlist.hpp"
#include "PyRandom.hpp"
#include "SavedSettings.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"
#include "WorkoutSelectDialog.hpp"

#include <random>

namespace awo {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kTitleRole = Qt::UserRole + 1;

}  // namespace

void AnalysisThread::run() {
    const QHash<QString, music::Analysis> results = music::analyzeSongs(
        paths_, [this](int done, int total) { emit progressed(done, total); },
        [this] { return isInterruptionRequested(); });
    emit analyzed(results);
}

MusicSelectDialog::MusicSelectDialog(QWidget* parent, bool savedSongs) : QDialog(parent) {
    qRegisterMetaType<QHash<QString, music::Analysis>>();
    auto* titleBar =
        framed(this, QStringLiteral("musicSelectWindow"), QStringLiteral("   Musical Cadence: Pick Songs   "), {}, false);
    theme::setSheet(this, pickerStyle(QStringLiteral("musicSelectWindow")));
    resize(1100, 820);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);

    auto* addRow = new QHBoxLayout;
    struct ButtonSpec {
        QString text;
        std::function<void()> slot;
        int key;
        QString label;
    };
    const QList<ButtonSpec> specs = {
        {QStringLiteral("Add Folder"), [this] { addFolder(); }, Qt::Key_D, QStringLiteral("D")},
        {QStringLiteral("Add Songs"), [this] { addSongs(); }, Qt::Key_A, QStringLiteral("A")},
        {QStringLiteral("Add Playlist (.m3u)"), [this] { addPlaylist(); }, Qt::Key_P, QStringLiteral("P")},
        {QStringLiteral("Remove"), [this] { removeSelected(); }, Qt::Key_Delete, QStringLiteral("Del")},
        {QStringLiteral("Clear"), [this] { clearSongs(); }, Qt::Key_C, QStringLiteral("C")},
    };
    for (const ButtonSpec& spec : specs) {
        auto* button = new QPushButton(spec.text);
        connect(button, &QPushButton::clicked, this, spec.slot);
        addRow->addWidget(button);
        keys::add(button, spec.key, spec.label);
    }
    layout->addLayout(addRow);

    list_ = new QListWidget;
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(list_, &QListWidget::itemChanged, this, [this] { updateSummary(); });
    layout->addWidget(list_, 1);

    progress_ = new QProgressBar;
    progress_->hide();
    layout->addWidget(progress_);

    auto* options = new QGridLayout;
    options->addWidget(new QLabel(QStringLiteral("Random ride of")), 0, 0, Qt::AlignRight);
    randomMinutes_ = new QSpinBox;
    randomMinutes_->setRange(kMinRandomMinutes, kMaxRandomMinutes);
    randomMinutes_->setSuffix(QStringLiteral(" min"));
    options->addWidget(randomMinutes_, 0, 1);
    auto* randomButton = new QPushButton(QStringLiteral("Pick Random Songs"));
    connect(randomButton, &QPushButton::clicked, this, &MusicSelectDialog::pickRandom);
    options->addWidget(randomButton, 0, 2);
    auto* allButton = new QPushButton(QStringLiteral("Check All"));
    connect(allButton, &QPushButton::clicked, this, [this] { checkAll(true); });
    options->addWidget(allButton, 0, 3);
    auto* noneButton = new QPushButton(QStringLiteral("Check None"));
    connect(noneButton, &QPushButton::clicked, this, [this] { checkAll(false); });
    options->addWidget(noneButton, 0, 4);
    options->addWidget(new QLabel(QStringLiteral("Preferred cadence")), 1, 0, Qt::AlignRight);
    preferredRpm_ = new QSpinBox;
    preferredRpm_->setRange(static_cast<int>(music::kMinRpm), static_cast<int>(music::kMaxRpm));
    preferredRpm_->setSuffix(QStringLiteral(" RPM"));
    preferredRpm_->setToolTip(QStringLiteral("A song's beat is doubled or halved to the speed nearest this"));
    options->addWidget(preferredRpm_, 1, 1);
    options->addWidget(new QLabel(QStringLiteral("Shortest cadence block")), 1, 2, Qt::AlignRight);
    minSegment_ = new QSpinBox;
    minSegment_->setRange(8, 120);
    minSegment_->setSuffix(QStringLiteral(" s"));
    minSegment_->setToolTip(QStringLiteral("Tempo changes shorter than this are folded into the cadence around them"));
    options->addWidget(minSegment_, 1, 3);
    layout->addLayout(options);

    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setAlignment(Qt::AlignCenter);
    layout->addWidget(summary_);

    auto* buttons = new QHBoxLayout;
    ok_ = new QPushButton(QStringLiteral("Ride These Songs"));
    connect(ok_, &QPushButton::clicked, this, &MusicSelectDialog::accept);
    // (Enter while it's disabled does nothing, rather than press another button)
    ok_->setDefault(true);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &MusicSelectDialog::reject);
    buttons->addWidget(ok_);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    // keys (Ctrl shows them; Esc is the X and Cancel; Space checks the current song with the list focused). Enter
    // rides wherever the focus is, a settings box included
    keys::add(randomButton, Qt::Key_R, QStringLiteral("R"));
    keys::add(allButton, Qt::Key_K, QStringLiteral("K"));
    keys::add(noneButton, Qt::Key_U, QStringLiteral("U"));
    keys::add(ok_, keys::kEnter, {QStringLiteral("Enter")});
    for (QSpinBox* box : {randomMinutes_, preferredRpm_, minSegment_}) {
        keys::Options inBox;
        inBox.field = box;
        keys::add(ok_, keys::kEnter, inBox);
    }
    // the settings: +/- the minutes (5 at a time), ,/. the preferred cadence, [/] the shortest block (keys neither the
    // list nor the boxes keep, so they work whatever's focused; Up/Down stay with the list)
    struct Stepper {
        QSpinBox* box;
        int step;
        QList<int> up, down;
        QString label;
    };
    const QList<Stepper> steppers = {
        {randomMinutes_, 5, {Qt::Key_Plus, Qt::Key_Equal}, {Qt::Key_Minus, Qt::Key_Underscore}, QStringLiteral("+ −")},
        {preferredRpm_, 1, {Qt::Key_Period, Qt::Key_Greater}, {Qt::Key_Comma, Qt::Key_Less}, QStringLiteral(", .")},
        {minSegment_, 1, {Qt::Key_BracketRight, Qt::Key_BraceRight}, {Qt::Key_BracketLeft, Qt::Key_BraceLeft}, QStringLiteral("[ ]")},
    };
    for (const Stepper& stepper : steppers) {
        for (const auto& [keyList, sign] : {std::pair{stepper.up, 1}, std::pair{stepper.down, -1}}) {
            keys::Options options;
            options.label = sign > 0 ? stepper.label : QString();
            options.repeat = true;
            QSpinBox* box = stepper.box;
            const int by = stepper.step * sign;
            options.action = [box, by] { box->stepBy(by); };
            keys::add(box, keyList, options);
        }
    }
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

    // the songs and settings from last time
    const QJsonObject saved = settings::load();
    randomMinutes_->setValue(saved.value(QStringLiteral("musicRandomMinutes")).toInt(60));
    preferredRpm_->setValue(saved.value(QStringLiteral("musicPreferredRpm")).toInt(static_cast<int>(music::kDefaultPreferredRpm)));
    minSegment_->setValue(saved.value(QStringLiteral("musicMinSegment")).toInt(static_cast<int>(music::kDefaultMinSegmentSeconds)));
    QList<std::pair<QString, std::optional<QString>>> songs;
    QSet<QString> checked;
    for (const music::SelectedSong& song : savedSongs ? music::loadSelection() : QList<music::SelectedSong>()) {
        if (QFileInfo(song.path).isFile()) {
            songs.append({song.path, song.title});
        }
        if (song.checked) {
            checked.insert(song.path);
        }
    }
    addToList(songs, checked);
    // scaled for this screen, and kept on it
    layout::fitDialog(this);
}

MusicSelectDialog::~MusicSelectDialog() {
    stopAnalysis();
}

// ---- Adding songs ----

void MusicSelectDialog::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Add a Folder of Songs"));
    if (folder.isEmpty()) {
        return;
    }
    QList<std::pair<QString, std::optional<QString>>> songs;
    for (const QString& path : music::folderSongs(folder)) {
        songs.append({path, std::nullopt});
    }
    addToList(songs);
}

void MusicSelectDialog::addSongs() {
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Add Songs"), QString(),
                                                            QStringLiteral("Songs (*.mp3 *.wav *.flac);; All Files (*)"));
    QList<std::pair<QString, std::optional<QString>>> songs;
    for (const QString& path : paths) {
        if (music::isAudio(path)) {
            songs.append({music::normPath(path), std::nullopt});
        }
    }
    addToList(songs);
}

void MusicSelectDialog::addPlaylist() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Add a Playlist"), QString(),
                                                      QStringLiteral("Playlists (*.m3u *.m3u8);; All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const music::PlaylistSongs found = music::readPlaylist(path);
    missing += found.missing;
    addToList(found.songs);
}

void MusicSelectDialog::addToList(const QList<std::pair<QString, std::optional<QString>>>& songs,
                                  const std::optional<QSet<QString>>& checked) {
    QStringList listed = paths();
    QStringList added;
    {
        const QSignalBlocker blocker(list_);
        for (const auto& [path, title] : songs) {
            if (listed.contains(path)) {
                continue;
            }
            listed.append(path);
            auto* item = new QListWidgetItem;
            item->setData(kPathRole, path);
            item->setData(kTitleRole, title && !title->isEmpty() ? *title : music::songTitle(path));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(!checked || checked->contains(path) ? Qt::Checked : Qt::Unchecked);
            list_->addItem(item);
            labelItem(item);
            added.append(path);
        }
    }
    analyze(added);
    updateSummary();
}

void MusicSelectDialog::removeSelected() {
    for (QListWidgetItem* item : list_->selectedItems()) {
        delete list_->takeItem(list_->row(item));
    }
    updateSummary();
}

void MusicSelectDialog::clearSongs() {
    list_->clear();
    missing.clear();
    updateSummary();
}

void MusicSelectDialog::checkAll(bool checked) {
    {
        const QSignalBlocker blocker(list_);
        for (QListWidgetItem* item : items()) {
            if (usable(item)) {
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
        }
    }
    updateSummary();
}

// ---- The analysis ----

void MusicSelectDialog::analyze(const QStringList& pathsIn) {
    QStringList todo;
    for (const QString& path : pathsIn) {
        if (!analyses.contains(path)) {
            todo.append(path);
        }
    }
    if (todo.isEmpty()) {
        return;
    }
    if (thread_) {
        pending_ += todo;
        return;
    }
    thread_ = new AnalysisThread(todo);
    connect(thread_, &AnalysisThread::progressed, this, &MusicSelectDialog::analysisProgress);
    connect(thread_, &AnalysisThread::analyzed, this, &MusicSelectDialog::analysisDone);
    progress_->setRange(0, static_cast<int>(todo.size()));
    progress_->setValue(0);
    progress_->show();
    thread_->start();
    updateSummary();
}

void MusicSelectDialog::analysisProgress(int done, int total) {
    progress_->setRange(0, total);
    progress_->setValue(done);
    progress_->setFormat(QStringLiteral("Analyzing songs: %1 / %2").arg(done).arg(total));
}

void MusicSelectDialog::analysisDone(const QHash<QString, music::Analysis>& results) {
    for (auto it = results.cbegin(); it != results.cend(); ++it) {
        analyses.insert(it.key(), it.value());
    }
    thread_->wait();
    thread_->deleteLater();
    thread_ = nullptr;
    progress_->hide();
    {
        const QSignalBlocker blocker(list_);
        for (QListWidgetItem* item : items()) {
            labelItem(item);
        }
    }
    const QStringList pending = std::exchange(pending_, {});
    analyze(pending);
    updateSummary();
}

void MusicSelectDialog::waitForAnalysis() {
    while (thread_) {
        AnalysisThread* thread = thread_;
        thread->wait();
        QCoreApplication::sendPostedEvents(this);
        if (thread_ == thread) {
            // (the result's signal not in yet)
            QCoreApplication::processEvents();
        }
    }
}

void MusicSelectDialog::stopAnalysis() {
    if (thread_) {
        thread_->requestInterruption();
        thread_->wait();
        // (its results' signal never handled: the dialog's going)
        disconnect(thread_, nullptr, this, nullptr);
        thread_->deleteLater();
        thread_ = nullptr;
    }
}

// ---- The list ----

QList<QListWidgetItem*> MusicSelectDialog::items() const {
    QList<QListWidgetItem*> all;
    for (int i = 0; i < list_->count(); ++i) {
        all.append(list_->item(i));
    }
    return all;
}

QStringList MusicSelectDialog::paths() const {
    QStringList all;
    for (QListWidgetItem* item : items()) {
        all.append(item->data(kPathRole).toString());
    }
    return all;
}

bool MusicSelectDialog::usable(QListWidgetItem* item) const {
    const auto found = analyses.find(item->data(kPathRole).toString());
    return found == analyses.end() || !found->error;
}

void MusicSelectDialog::labelItem(QListWidgetItem* item) {
    const QString title = item->data(kTitleRole).toString();
    const auto found = analyses.find(item->data(kPathRole).toString());
    if (found == analyses.end()) {
        item->setText(title + QStringLiteral("   (analyzing...)"));
    } else if (found->error) {
        item->setText(title + QStringLiteral("   (can't be read)"));
        item->setCheckState(Qt::Unchecked);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    } else {
        item->setText(title + QStringLiteral("   ") + minimizedDuration(static_cast<int>(found->duration)));
    }
}

QList<std::pair<QString, QString>> MusicSelectDialog::checkedSongs() const {
    QList<std::pair<QString, QString>> songs;
    for (QListWidgetItem* item : items()) {
        const QString path = item->data(kPathRole).toString();
        const auto found = analyses.find(path);
        if (item->checkState() == Qt::Checked && found != analyses.end() && !found->error) {
            songs.append({path, item->data(kTitleRole).toString()});
        }
    }
    return songs;
}

// Checks a random set of the analyzed songs adding up to about the minutes asked for (spread over as many cadences as
// it can), and moves them to the top of the list in the order they'll play
void MusicSelectDialog::pickRandom() {
    QHash<QString, double> durations;
    for (auto it = analyses.cbegin(); it != analyses.cend(); ++it) {
        if (!it->error) {
            durations.insert(it.key(), it->duration);
        }
    }
    QStringList songs;
    QHash<QString, std::optional<double>> cadences;
    for (const QString& path : paths()) {
        if (durations.contains(path)) {
            songs.append(path);
            cadences.insert(path, music::songCadence(analyses[path], preferredRpm_->value(), minSegment_->value()));
        }
    }
    // (an unseeded random.Random: different every time)
    py::Random rng(std::random_device{}() | (static_cast<std::uint64_t>(std::random_device{}()) << 32));
    const QStringList picked = music::randomSelection(songs, randomMinutes_->value() * 60.0, durations, rng, cadences);
    if (picked.isEmpty()) {
        return;
    }
    {
        const QSignalBlocker blocker(list_);
        QList<QListWidgetItem*> rows;
        while (list_->count()) {
            rows.append(list_->takeItem(0));
        }
        std::stable_sort(rows.begin(), rows.end(), [&](QListWidgetItem* a, QListWidgetItem* b) {
            const qsizetype ia = picked.indexOf(a->data(kPathRole).toString());
            const qsizetype ib = picked.indexOf(b->data(kPathRole).toString());
            return (ia < 0 ? picked.size() : ia) < (ib < 0 ? picked.size() : ib);
        });
        for (QListWidgetItem* item : rows) {
            if (usable(item)) {
                item->setCheckState(picked.contains(item->data(kPathRole).toString()) ? Qt::Checked : Qt::Unchecked);
            }
            list_->addItem(item);
        }
    }
    list_->scrollToTop();
    updateSummary();
}

QString MusicSelectDialog::summary() const {
    return summary_->text();
}

void MusicSelectDialog::updateSummary() {
    const QList<std::pair<QString, QString>> songs = checkedSongs();
    double seconds = 0;
    for (const auto& [path, title] : songs) {
        seconds += analyses[path].duration;
    }
    QStringList parts = {QStringLiteral("Ride: %1 song%2, %3")
                             .arg(songs.size())
                             .arg(songs.size() != 1 ? QStringLiteral("s") : QString())
                             .arg(minimizedDuration(static_cast<int>(seconds)))};
    const auto unreadable = std::count_if(analyses.cbegin(), analyses.cend(), [](const music::Analysis& a) { return a.error.has_value(); });
    if (unreadable) {
        parts.append(QStringLiteral("%1 can't be read").arg(unreadable));
    }
    if (!missing.isEmpty()) {
        parts.append(QStringLiteral("%1 playlist entr%2 not found")
                         .arg(missing.size())
                         .arg(missing.size() != 1 ? QStringLiteral("ies") : QStringLiteral("y")));
    }
    if (thread_) {
        parts.append(QStringLiteral("still analyzing"));
    }
    summary_->setText(parts.join(QStringLiteral("   ·   ")));
    ok_->setEnabled(!songs.isEmpty() && !thread_);
}

// ---- Done ----

void MusicSelectDialog::accept() {
    if (checkedSongs().isEmpty()) {
        return;
    }
    save();
    QDialog::accept();
}

void MusicSelectDialog::reject() {
    stopAnalysis();
    save();
    QDialog::reject();
}

void MusicSelectDialog::save() {
    QList<music::SelectedSong> songs;
    for (QListWidgetItem* item : items()) {
        songs.append({item->data(kPathRole).toString(), item->data(kTitleRole).toString(), item->checkState() == Qt::Checked});
    }
    music::saveSelection(songs);
    settings::update({{QStringLiteral("musicPreferredRpm"), preferredRpm_->value()},
                      {QStringLiteral("musicMinSegment"), minSegment_->value()},
                      {QStringLiteral("musicRandomMinutes"), randomMinutes_->value()}});
}

MusicSelection MusicSelectDialog::selection() const {
    MusicSelection result;
    for (const auto& [path, title] : checkedSongs()) {
        result.songs.append({path, title});
    }
    result.analyses = analyses;
    result.preferred = preferredRpm_->value();
    result.minSegment = minSegment_->value();
    return result;
}

}  // namespace awo
