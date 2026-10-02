#include "dialogs.h"

#include "config.h"
#include "engine.h"
#include "theme.h"
#include "themeeditor.h"
#include <QUuid>
#include <QSignalBlocker>
#include <QtGlobal>

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTextStream>
#include <QVBoxLayout>
#include <functional>
#include <algorithm>

namespace {

// "90", "1:30", "1:30.5", "" -> seconds; ok=false on garbage.
double parseSeconds(const QString &text, bool *okOut)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (okOut)
            *okOut = true;
        return 0.0;
    }
    double total = 0.0;
    bool ok = true;
    for (const QString &part : t.split(QLatin1Char(':'))) {
        bool partOk = false;
        const double v = part.trimmed().toDouble(&partOk);
        if (!partOk) {
            ok = false;
            break;
        }
        total = total * 60.0 + v;
    }
    if (okOut)
        *okOut = ok;
    return ok ? total : -1.0;
}

QString secondsText(double seconds)
{
    if (seconds <= 0.0)
        return QString();
    const int total = int(seconds);
    return QString::asprintf("%d:%02d", total / 60, total % 60);
}

// formats_in(): (format, count) sorted by count desc, then name.
QVector<QPair<QString, int>> formatsIn(const QVector<Track> &tracks)
{
    // unanalyzed tracks count under "" (shown as "unknown"): the filter
    // compares against the track's format, which is "" for them
    QHash<QString, int> counts;
    for (const Track &track : tracks)
        counts[track.fmt.toLower().trimmed()] += 1;
    QVector<QPair<QString, int>> out;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return out;
}

}   // namespace

// ============================================================================
// FilterDialog
// ============================================================================
FilterDialog::FilterDialog(QWidget *parent, const QueueFilter &current,
                           const QVector<Track> &tracks)
    : QDialog(parent), tracks_(tracks), base_(current)
{
    setWindowTitle(tr("Filter the queue"));
    auto *root = new QVBoxLayout(this);

    auto *formatBox = new QGroupBox(tr("Formats"), this);
    auto *grid = new QGridLayout(formatBox);
    const auto formats = formatsIn(tracks);
    int col = 0;
    for (const auto &pair : formats) {
        auto *check = new QCheckBox(QStringLiteral("%1 (%2)")
                                        .arg(pair.first.isEmpty() ? tr("unknown") : pair.first)
                                        .arg(pair.second),
                                    formatBox);
        check->setProperty("format", pair.first);
        // "all formats selected" means no restriction (keeps unknown too)
        check->setChecked(current.formats.isEmpty() || current.formats.contains(pair.first)
                          || (pair.first.isEmpty() && current.formats.contains(QLatin1String("unknown"))));
        connect(check, &QCheckBox::toggled, this, [this] { updatePreview(); });
        grid->addWidget(check, col / 3, col % 3);
        formatChecks_.append(check);
        ++col;
    }
    if (formats.isEmpty())
        grid->addWidget(new QLabel(tr("No analyzed tracks yet - run Analyze first."), formatBox));
    root->addWidget(formatBox);

    auto *range = new QGroupBox(tr("Length (seconds or m:ss)"), this);
    auto *form = new QFormLayout(range);
    minEdit_ = new QLineEdit(secondsText(current.minSeconds), range);
    maxEdit_ = new QLineEdit(secondsText(current.maxSeconds), range);
    minEdit_->setPlaceholderText(tr("no minimum"));
    maxEdit_->setPlaceholderText(tr("no maximum"));
    connect(minEdit_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    connect(maxEdit_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    form->addRow(tr("Shortest"), minEdit_);
    form->addRow(tr("Longest"), maxEdit_);
    root->addWidget(range);

    hideBroken_ = new QCheckBox(tr("Hide modules that cannot be played"), this);
    hideBroken_->setChecked(current.hideBroken);
    connect(hideBroken_, &QCheckBox::toggled, this, [this] { updatePreview(); });
    root->addWidget(hideBroken_);

    countLabel_ = new QLabel(this);
    root->addWidget(countLabel_);

    auto *buttons = new QDialogButtonBox(this);
    auto *clearBtn = buttons->addButton(tr("Clear filter"), QDialogButtonBox::ResetRole);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto *applyBtn = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    applyBtn_ = applyBtn;
    connect(clearBtn, &QPushButton::clicked, this, &FilterDialog::clear);
    connect(applyBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    updatePreview();
    resize(440, 300);
}

QueueFilter FilterDialog::criteria() const
{
    QueueFilter out;
    const bool all = std::all_of(formatChecks_.begin(), formatChecks_.end(),
                                 [](QCheckBox *c) { return c->isChecked(); });
    if (!all) {
        for (QCheckBox *check : formatChecks_) {
            if (check->isChecked()) {
                // not-analyzed tracks are stored as "unknown" (the settings
                // loader drops empty format names)
                const QString f = check->property("format").toString();
                out.formats << (f.isEmpty() ? QStringLiteral("unknown") : f);
            }
        }
    }
    bool ok = true;
    out.minSeconds = std::max(0.0, parseSeconds(minEdit_->text(), &ok));
    out.maxSeconds = std::max(0.0, parseSeconds(maxEdit_->text(), &ok));
    out.hideBroken = hideBroken_->isChecked();
    return out;
}

void FilterDialog::updatePreview()
{
    // nothing checked would be stored as "no restriction" and show everything
    const bool none = !formatChecks_.isEmpty()
                      && std::none_of(formatChecks_.begin(), formatChecks_.end(),
                                      [](QCheckBox *c) { return c->isChecked(); });
    if (applyBtn_)
        applyBtn_->setEnabled(!none);
    if (none) {
        countLabel_->setText(tr("Check at least one format."));
        return;
    }
    const int kept = criteria().count(tracks_);
    QString text = tr("%1 of %2 tracks match").arg(kept).arg(tracks_.size());
    int unknown = 0;
    for (const Track &t : tracks_) {
        if (!t.analyzed)
            ++unknown;
    }
    if (unknown)
        text += tr("  (%1 not analyzed yet)").arg(unknown);
    countLabel_->setText(text);
}

void FilterDialog::clear()
{
    for (QCheckBox *check : formatChecks_)
        check->setChecked(true);
    minEdit_->clear();
    maxEdit_->clear();
    hideBroken_->setChecked(false);
    updatePreview();
}

// ============================================================================
// PlaylistsDialog
// ============================================================================
PlaylistsDialog::PlaylistsDialog(QWidget *parent, PlaylistStore *store,
                                 std::function<QStringList()> queuePaths,
                                 std::function<void(const QString &, bool)> loadAsSource,
                                 std::function<void(const QString &)> saveQueueAs,
                                 std::function<void(const QString &, const QString &)> onRenamed,
                                 std::function<void(const QString &)> onDeleted)
    : QDialog(parent), store_(store), queuePaths_(std::move(queuePaths)),
      loadAsSource_(std::move(loadAsSource)), saveQueueAs_(std::move(saveQueueAs)),
      onRenamed_(std::move(onRenamed)), onDeleted_(std::move(onDeleted))
{
    setWindowTitle(tr("Playlists"));
    auto *root = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Name"), this));
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("new playlist name"));
    top->addWidget(nameEdit_, 1);
    createBtn_ = new QPushButton(tr("Create empty playlist"), this);
    connect(createBtn_, &QPushButton::clicked, this, &PlaylistsDialog::createEmpty);
    connect(nameEdit_, &QLineEdit::returnPressed, this, &PlaylistsDialog::createEmpty);
    top->addWidget(createBtn_);
    root->addLayout(top);

    auto *saveAsBtn = new QPushButton(tr("Save current queue as\u2026"), this);
    connect(saveAsBtn, &QPushButton::clicked, this, &PlaylistsDialog::saveAs);
    root->addWidget(saveAsBtn, 0, Qt::AlignLeft);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Playlists"), tr("Tracks")});
    tree_->setRootIsDecorated(false);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &PlaylistsDialog::updateButtons);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { loadSelected(); });
    root->addWidget(tree_, 1);

    auto *row1 = new QHBoxLayout;
    loadBtn_ = new QPushButton(tr("Load"), this);
    connect(loadBtn_, &QPushButton::clicked, this, &PlaylistsDialog::loadSelected);
    renameBtn_ = new QPushButton(tr("Rename"), this);
    connect(renameBtn_, &QPushButton::clicked, this, &PlaylistsDialog::renameSelected);
    deleteBtn_ = new QPushButton(tr("Delete"), this);
    connect(deleteBtn_, &QPushButton::clicked, this, &PlaylistsDialog::deleteSelected);
    exportBtn_ = new QPushButton(tr("Export M3U\u2026"), this);
    connect(exportBtn_, &QPushButton::clicked, this, &PlaylistsDialog::exportSelected);
    for (QPushButton *b : {loadBtn_, renameBtn_, deleteBtn_, exportBtn_})
        row1->addWidget(b);
    row1->addStretch();
    root->addLayout(row1);

    auto *row2 = new QHBoxLayout;
    addSelBtn_ = new QPushButton(tr("Add selected songs"), this);
    connect(addSelBtn_, &QPushButton::clicked, this, &PlaylistsDialog::addSelectedSongs);
    auto *addFilesBtn = new QPushButton(tr("Add files\u2026"), this);
    connect(addFilesBtn, &QPushButton::clicked, this, &PlaylistsDialog::addFiles);
    auto *importBtn = new QPushButton(tr("Import M3U\u2026"), this);
    connect(importBtn, &QPushButton::clicked, this, &PlaylistsDialog::importM3u);
    row2->addWidget(addSelBtn_);
    row2->addWidget(addFilesBtn);
    row2->addWidget(importBtn);
    row2->addStretch();
    root->addLayout(row2);

    noteLabel_ = new QLabel(this);
    noteLabel_->setWordWrap(true);
    root->addWidget(noteLabel_);

    refresh();
    resize(580, 440);
}

void PlaylistsDialog::refresh(const QString &selectName)
{
    tree_->clear();
    for (const QString &name : store_->names()) {
        const Playlist *pl = store_->get(name);
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, name);
        item->setText(1, QString::number(pl ? pl->paths.size() : 0));
        item->setData(0, Qt::UserRole, name);
        if (!selectName.isEmpty() && name.compare(selectName, Qt::CaseInsensitive) == 0)
            tree_->setCurrentItem(item);
    }
    updateButtons();
}

QString PlaylistsDialog::selectedName() const
{
    const auto items = tree_->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
}

void PlaylistsDialog::updateButtons()
{
    const QString name = selectedName();
    const bool has = !name.isEmpty();
    const bool fav = PlaylistStore::isFavorites(name);
    for (QPushButton *b : {loadBtn_, renameBtn_, deleteBtn_, exportBtn_, addSelBtn_})
        b->setEnabled(has);
    renameBtn_->setEnabled(has && !fav);
    deleteBtn_->setText(fav ? tr("Clear Favorites") : tr("Delete"));
    if (!has)
        noteLabel_->setText(tr("Pick a playlist to load, rename, or add songs to."));
    else if (fav)
        noteLabel_->setText(tr("Favorites is permanent. Add songs with \u2606 in the player or "
                               "the queue's right-click menu."));
    else {
        const Playlist *pl = store_->get(name);
        noteLabel_->setText(tr("%1 tracks \u00b7 saved order %2")
                                .arg(pl ? pl->paths.size() : 0)
                                .arg(pl && !pl->root.isEmpty()
                                         ? tr("root %1").arg(pl->root)
                                         : tr("has no root folder")));
    }
}

void PlaylistsDialog::createEmpty()
{
    QString err;
    const QString name = PlaylistStore::normalizeName(nameEdit_->text(), &err);
    if (name.isEmpty()) {
        noteLabel_->setText(err.isEmpty() ? tr("Give the playlist a name first.") : err);
        return;
    }
    const bool ok = store_->create(name, {});
    const QString message = store_->error;
    nameEdit_->clear();
    refresh(name);
    if (!ok)
        noteLabel_->setText(message);   // after refresh, which rewrites the note
}

void PlaylistsDialog::addSelectedSongs()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const QStringList paths = queuePaths_();
    if (paths.isEmpty()) {
        noteLabel_->setText(tr("The queue is empty."));
        return;
    }
    const QString message = store_->addPaths(name, paths)
                                ? tr("Added %1 songs to \"%2\".").arg(paths.size()).arg(name)
                                : store_->error;
    refresh(name);
    noteLabel_->setText(message);
}

void PlaylistsDialog::addFiles()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add files to %1").arg(name), QString(),
        tr("Module files (*.mod *.xm *.it *.s3m *.mtm *.669 *.ult *.stm *.far *.gdm *.med);;"
           "All files (*)"));
    if (paths.isEmpty())
        return;
    const QString message = store_->addPaths(name, paths)
                                ? tr("Added %1 files to \"%2\".").arg(paths.size()).arg(name)
                                : store_->error;
    refresh(name);
    noteLabel_->setText(message);
}

void PlaylistsDialog::saveAs()
{
    QString err;
    QString suggested = nameEdit_->text();
    if (suggested.trimmed().isEmpty()) {
        bool ok = false;
        suggested = QInputDialog::getText(this, tr("Save queue as playlist"), tr("Playlist name:"),
                                          QLineEdit::Normal, QStringLiteral("Playlist"), &ok);
        if (!ok)
            return;
    }
    const QString name = PlaylistStore::normalizeName(suggested, &err);
    if (name.isEmpty()) {
        noteLabel_->setText(err.isEmpty() ? tr("Invalid playlist name.") : err);
        return;
    }
    saveQueueAs_(name);
    refresh(name);
}

void PlaylistsDialog::renameSelected()
{
    const QString oldName = selectedName();
    if (oldName.isEmpty())
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Rename playlist"), tr("New name:"),
                                               QLineEdit::Normal, oldName, &ok);
    if (!ok)
        return;
    QString err;
    const QString name = PlaylistStore::normalizeName(text, &err);
    if (name.isEmpty()) {
        noteLabel_->setText(err.isEmpty() ? tr("Invalid playlist name.") : err);
        return;
    }
    if (!store_->rename(oldName, name)) {
        noteLabel_->setText(store_->error);
        return;
    }
    if (onRenamed_)
        onRenamed_(oldName, name);
    refresh(name);
}

void PlaylistsDialog::deleteSelected()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    if (PlaylistStore::isFavorites(name)) {
        if (QMessageBox::question(this, tr("Clear Favorites"),
                tr("Remove all songs from Favorites? Your music files will not be deleted."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        store_->clearFavorites();
        refresh(name);
        return;
    }
    if (QMessageBox::question(this, tr("Delete playlist"),
                              tr("Delete \"%1\"? Songs are only removed from the playlist.")
                                  .arg(name))
        != QMessageBox::Yes)
        return;
    if (!store_->remove(name)) {
        noteLabel_->setText(store_->error);
        return;
    }
    if (onDeleted_)
        onDeleted_(name);
    refresh();
}

void PlaylistsDialog::loadSelected()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    loadAsSource_(name, false);   // Load keeps the order choice (README)
    accept();
}

void PlaylistsDialog::exportSelected()
{
    const QString name = selectedName();
    const Playlist *pl = store_->get(name);
    if (!pl)
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export playlist"),
                                                      name + QStringLiteral(".m3u"),
                                                      tr("M3U playlist (*.m3u)"));
    if (path.isEmpty())
        return;
    const int written = writeM3u(pl->paths, path);
    if (written < 0)
        noteLabel_->setText(tr("Could not write %1").arg(QFileInfo(path).fileName()));
    else
        noteLabel_->setText(tr("Exported %1 of %2 entries to %3")
                                .arg(written).arg(pl->paths.size()).arg(QFileInfo(path).fileName()));
}

void PlaylistsDialog::importM3u()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import M3U"), QString(),
                                                      tr("M3U playlist (*.m3u *.m3u8);;All files (*)"));
    if (path.isEmpty())
        return;
    int skipped = 0;
    const QStringList entries = readM3u(path, &skipped);
    if (entries.isEmpty()) {
        noteLabel_->setText(tr("That M3U had no readable files."));
        return;
    }
    const QString base = QFileInfo(path).completeBaseName();
    QString name = store_->uniqueName(base, QStringLiteral(" (%1)"));
    if (name.isEmpty())
        name = store_->uniqueName(QStringLiteral("Imported"), QStringLiteral(" (%1)"));
    const bool ok = !name.isEmpty() && store_->create(name, entries, QFileInfo(path).absolutePath());
    const QString message = ok ? tr("Imported %1 tracks (%2 missing skipped) as \"%3\"")
                                     .arg(entries.size()).arg(skipped).arg(name)
                               : store_->error;
    refresh(name);
    noteLabel_->setText(message);
    if (ok)
        loadAsSource_(name, true);   // an imported M3U opens in Saved order (README)
}

// ============================================================================
// SettingsDialog
// ============================================================================
SettingsDialog::SettingsDialog(QWidget *parent, const Settings &settings, IgnoreStore *ignored)
    : QDialog(parent), base_(settings), ignoreStore_(ignored)
{
    setWindowTitle(tr("Settings"));
    auto *root = new QVBoxLayout(this);

    auto *playback = new QGroupBox(tr("Playback"), this);
    auto *playForm = new QFormLayout(playback);
    backendCombo_ = new QComboBox(playback);
    backendCombo_->addItem(tr("Default output"), QStringLiteral("auto"));
    backendCombo_->addItem(tr("Silent (no audio device)"), QStringLiteral("null"));
    backendCombo_->setCurrentIndex(base_.backend == QLatin1String("null") ? 1 : 0);
    playForm->addRow(tr("Audio backend"), backendCombo_);
    rateCombo_ = new QComboBox(playback);
    rateCombo_->addItem(tr("Auto (device default)"), 0);
    for (int rate : Settings::sampleRateChoices())
        rateCombo_->addItem(Settings::sampleRateLabel(rate), rate);
    for (int i = 0; i < rateCombo_->count(); ++i)
        if (rateCombo_->itemData(i).toInt() == base_.samplerate)
            rateCombo_->setCurrentIndex(i);
    playForm->addRow(tr("Sample rate"), rateCombo_);
    interpCombo_ = new QComboBox(playback);
    interpCombo_->addItem(tr("No interpolation"), QStringLiteral("off"));
    interpCombo_->addItem(tr("Linear (2-tap)"), QStringLiteral("linear"));
    interpCombo_->addItem(tr("Cubic (4-tap)"), QStringLiteral("cubic"));
    interpCombo_->addItem(tr("Sinc (8-tap)"), QStringLiteral("sinc"));
    for (int i = 0; i < interpCombo_->count(); ++i)
        if (interpCombo_->itemData(i).toString() == base_.interpolation)
            interpCombo_->setCurrentIndex(i);
    playForm->addRow(tr("Interpolation"), interpCombo_);
    bufferSpin_ = new QSpinBox(playback);
    bufferSpin_->setRange(20, 5000);
    bufferSpin_->setSuffix(QStringLiteral(" ms"));
    bufferSpin_->setValue(base_.bufferMs);
    playForm->addRow(tr("Buffer"), bufferSpin_);
    fpsSpin_ = new QSpinBox(playback);
    fpsSpin_->setRange(5, 120);
    fpsSpin_->setValue(base_.uiFps);
    playForm->addRow(tr("UI refresh"), fpsSpin_);
    root->addWidget(playback);

    auto *between = new QGroupBox(tr("Between runs"), this);
    auto *betweenForm = new QFormLayout(between);
    autoAnalyze_ = new QCheckBox(tr("Analyze new files automatically"), between);
    autoAnalyze_->setChecked(base_.autoAnalyze);
    betweenForm->addRow(autoAnalyze_);
    cacheAnalysis_ = new QCheckBox(tr("Keep duration/title details (analysis cache)"), between);
    cacheAnalysis_->setChecked(base_.cacheAnalysis);
    betweenForm->addRow(cacheAnalysis_);
    rememberPos_ = new QCheckBox(tr("Remember the subsong and playing position"), between);
    rememberPos_->setChecked(base_.rememberPosition);
    betweenForm->addRow(rememberPos_);
    trackStats_ = new QCheckBox(tr("Record local listening stats"), between);
    trackStats_->setChecked(base_.trackListeningStats);
    betweenForm->addRow(trackStats_);
    smoothTracker_ = new QCheckBox(tr("Smooth tracker scrolling"), between);
    smoothTracker_->setChecked(base_.smoothTrackerScrolling);
    betweenForm->addRow(smoothTracker_);
    root->addWidget(between);

    auto *appearance = new QGroupBox(tr("Appearance"), this);
    auto *appearanceForm = new QFormLayout(appearance);
    themeCombo_ = new QComboBox(appearance);
    themeCombo_->setObjectName(QStringLiteral("themeCombo"));
    customThemes_ = Palette::cleanCustomThemes(base_.customThemes);
    appearanceForm->addRow(tr("Theme"), themeCombo_);
    auto *themeActions = new QHBoxLayout;
    newTheme_ = new QPushButton(tr("New theme…"), appearance);
    editTheme_ = new QPushButton(tr("Edit…"), appearance);
    deleteTheme_ = new QPushButton(tr("Delete"), appearance);
    newTheme_->setObjectName(QStringLiteral("newTheme"));
    editTheme_->setObjectName(QStringLiteral("editTheme"));
    deleteTheme_->setObjectName(QStringLiteral("deleteTheme"));
    themeActions->addWidget(newTheme_); themeActions->addWidget(editTheme_);
    themeActions->addWidget(deleteTheme_); themeActions->addStretch();
    appearanceForm->addRow(QString(), themeActions);
    connect(newTheme_, &QPushButton::clicked, this, [this] { editTheme(true); });
    connect(editTheme_, &QPushButton::clicked, this, [this] { editTheme(false); });
    connect(deleteTheme_, &QPushButton::clicked, this, &SettingsDialog::deleteTheme);
    connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this] {
        const bool custom = customThemes_.contains(themeCombo_->currentData().toString());
        editTheme_->setEnabled(custom); deleteTheme_->setEnabled(custom);
    });
    refreshThemes(base_.theme);
    titleModeCombo_ = new QComboBox(appearance);
    titleModeCombo_->addItem(tr("Track name"), QStringLiteral("track"));
    titleModeCombo_->addItem(tr("Module title"), QStringLiteral("title"));
    titleModeCombo_->addItem(tr("File name"), QStringLiteral("filename"));
    titleModeCombo_->addItem(tr("Nothing"), QStringLiteral("none"));
    for (int i = 0; i < titleModeCombo_->count(); ++i)
        if (titleModeCombo_->itemData(i).toString() == base_.windowTitleMode)
            titleModeCombo_->setCurrentIndex(i);
    appearanceForm->addRow(tr("Window title"), titleModeCombo_);
    root->addWidget(appearance);

    skipIgnoreConfirmation_ = new QCheckBox(tr("Don't ask again when ignoring songs"), this);
    skipIgnoreConfirmation_->setObjectName(QStringLiteral("skipIgnoreConfirmation"));
    skipIgnoreConfirmation_->setChecked(!base_.confirmIgnore);
    skipIgnoreConfirmation_->setToolTip(tr("Uncheck to show the ignore confirmation again."));
    root->addWidget(skipIgnoreConfirmation_);

    auto *robust = new QHBoxLayout;
    robust->addWidget(new QLabel(tr("Ignored files:"), this));
    ignoredCountLabel_ = new QLabel(this);
    robust->addWidget(ignoredCountLabel_);
    auto *ignoredBtn = new QPushButton(tr("Manage ignored songs\u2026"), this);
    connect(ignoredBtn, &QPushButton::clicked, this, [this] {
        IgnoreDialog dialog(this, ignoreStore_);
        dialog.exec();
        refreshIgnoredCount();
        emit ignoredChanged();
    });
    robust->addWidget(ignoredBtn);
    robust->addStretch();
    root->addLayout(robust);

    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    refreshIgnoredCount();
    resize(480, 560);
}

void SettingsDialog::accept()
{
    // Failed writes leave the dialog and its theme drafts available for retry.
    if (!saveHandler_ || saveHandler_()) QDialog::accept();
}

void SettingsDialog::refreshThemes(const QString &selected)
{
    const QSignalBlocker block(themeCombo_);
    themeCombo_->clear();
    for (const QString &key : Palette::builtinThemes())
        themeCombo_->addItem(Palette::themeLabel(key), key);
    QStringList ids = customThemes_.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return customThemes_.value(a).toObject().value("name").toString().toCaseFolded()
             < customThemes_.value(b).toObject().value("name").toString().toCaseFolded();
    });
    for (const QString &id : ids)
        themeCombo_->addItem(tr("%1 (custom)").arg(customThemes_.value(id).toObject().value("name").toString()), id);
    themeCombo_->setCurrentIndex(std::max(0, themeCombo_->findData(Palette::normalizeTheme(selected, customThemes_))));
    const bool custom = customThemes_.contains(themeCombo_->currentData().toString());
    editTheme_->setEnabled(custom); deleteTheme_->setEnabled(custom);
    newTheme_->setEnabled(customThemes_.size() < 100);
    newTheme_->setToolTip(customThemes_.size() >= 100 ? tr("Delete a custom theme before creating another (limit: 100).")
                                                    : tr("Copy the selected palette into a new custom theme."));
}

void SettingsDialog::editTheme(bool create)
{
    const QString selected = themeCombo_->currentData().toString();
    if ((!create && !customThemes_.contains(selected)) || (create && customThemes_.size() >= 100)) return;
    const QString id = create ? QStringLiteral("custom:") + QUuid::createUuid().toString(QUuid::Id128) : selected;
    QString name = customThemes_.value(selected).toObject().value("name").toString();
    if (create) {
        QSet<QString> used;
        for (auto it=customThemes_.constBegin(); it!=customThemes_.constEnd(); ++it)
            used.insert(it.value().toObject().value("name").toString().toCaseFolded());
        name = tr("My theme");
        for (int n=2; used.contains(name.toCaseFolded()); ++n) name=tr("My theme %1").arg(n);
    }
    ThemeEditorDialog editor(this, id, name, Palette::resolve(selected, customThemes_), customThemes_);
    if (editor.exec() != QDialog::Accepted) return;
    QJsonObject proposed = customThemes_; proposed.insert(id, editor.definition());
    const auto cleaned = Palette::cleanCustomThemes(proposed);
    if (cleaned.size()!=proposed.size() || !cleaned.contains(id)) return;
    customThemes_ = cleaned; refreshThemes(id);
}

void SettingsDialog::deleteTheme()
{
    const QString id = themeCombo_->currentData().toString();
    if (!customThemes_.contains(id)) return;
    QMessageBox question(QMessageBox::Question, tr("Delete theme"),
        tr("Delete “%1”? This takes effect only when you save Settings.")
            .arg(customThemes_.value(id).toObject().value("name").toString()),
        QMessageBox::Yes | QMessageBox::No, this);
    question.setTextFormat(Qt::PlainText); question.setDefaultButton(QMessageBox::No);
    if (question.exec()!=QMessageBox::Yes) return;
    customThemes_.remove(id); refreshThemes(QStringLiteral("dark"));
}

void SettingsDialog::refreshIgnoredCount()
{
    ignoredCountLabel_->setText(ignoreStore_ ? QString::number(ignoreStore_->count())
                                             : QStringLiteral("0"));
}

void SettingsDialog::setConfirmIgnore(bool confirm)
{
    skipIgnoreConfirmation_->setChecked(!confirm);
}

void SettingsDialog::collect(Settings &out) const
{
    out.confirmIgnore = !skipIgnoreConfirmation_->isChecked();
    out.backend = backendCombo_->currentData().toString();
    out.samplerate = rateCombo_->currentData().toInt();
    out.interpolation = interpCombo_->currentData().toString();
    out.bufferMs = bufferSpin_->value();
    out.uiFps = fpsSpin_->value();
    out.autoAnalyze = autoAnalyze_->isChecked();
    out.cacheAnalysis = cacheAnalysis_->isChecked();
    out.rememberPosition = rememberPos_->isChecked();
    out.trackListeningStats = trackStats_->isChecked();
    out.smoothTrackerScrolling = smoothTracker_->isChecked();
    out.theme = themeCombo_->currentData().toString();
    out.customThemes = customThemes_;
    out.windowTitleMode = titleModeCombo_->currentData().toString();
}

// ============================================================================
// SongInfoDialog
// ============================================================================
SongInfoDialog::SongInfoDialog(QWidget *parent, const ModuleInfo &info) : QDialog(parent)
{
    setWindowTitle(tr("Song info - %1").arg(info.title.isEmpty() ? info.path : info.title));
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("<h3>%1</h3>").arg(info.title.toHtmlEscaped()), this));
    QString summary = tr("%1, %2 samples, %3 instruments")
                          .arg(info.formatLong.isEmpty() ? info.format : info.formatLong)
                          .arg(info.samples).arg(info.instruments);
    if (!info.tracker.isEmpty())
        summary += tr(" \u00b7 %1").arg(info.tracker);
    root->addWidget(new QLabel(summary, this));

    auto *tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Samples"), tr("Instruments")});
    tree->setRootIsDecorated(false);
    const int rows = std::max(info.sampleNames.size(), info.instrumentNames.size());
    for (int i = 0; i < rows; ++i) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, i < info.sampleNames.size()
                             ? QStringLiteral("%1. %2").arg(i + 1)
                                   .arg(info.sampleNames.at(i).trimmed())
                             : QString());
        item->setText(1, i < info.instrumentNames.size()
                             ? QStringLiteral("%1. %2").arg(i + 1)
                                   .arg(info.instrumentNames.at(i).trimmed())
                             : QString());
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(tree, 1);

    if (!info.message.isEmpty()) {
        auto *message = new QPlainTextEdit(info.message, this);
        message->setReadOnly(true);
        message->setMaximumHeight(140);
        root->addWidget(message);
    }
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    resize(640, 460);
}

// ============================================================================
// StatsDialog
// ============================================================================
StatsDialog::StatsDialog(QWidget *parent, StatsStore *store) : QDialog(parent), store_(store)
{
    setWindowTitle(tr("Listening stats"));
    auto *root = new QVBoxLayout(this);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Module"), tr("Plays"), tr("Time"), tr("Last played")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    root->addWidget(tree_, 1);
    totalLabel_ = new QLabel(this);
    root->addWidget(totalLabel_);
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);
    auto *buttons = new QHBoxLayout;
    auto *clearBtn = new QPushButton(tr("Clear stats"), this);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clear stats"), tr("Forget all listening history?"))
            == QMessageBox::Yes) {
            store_->resetAll();
            store_->save();
            refresh();
            if (store_->error.isEmpty())
                emit statsReset();
        }
    });
    buttons->addWidget(clearBtn);
    buttons->addStretch();
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);
    refresh();
    resize(660, 420);
}

void StatsDialog::refresh()
{
    tree_->clear();
    for (const ModuleStats &row : store_->mostPlayed(500)) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, row.title.isEmpty() ? QFileInfo(row.path).fileName() : row.title);
        item->setToolTip(0, row.path);
        item->setText(1, QString::number(row.plays));
        item->setText(2, formatTime(row.seconds));
        item->setText(3, row.lastPlayed > 0
                             ? QDateTime::fromSecsSinceEpoch(qint64(row.lastPlayed))
                                   .toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                             : QStringLiteral("-"));
    }
    totalLabel_->setText(tr("%1 modules, %2 total listening time")
                             .arg(store_->count()).arg(formatTime(store_->totalSeconds())));
    errorLabel_->setText(store_->error);
    errorLabel_->setVisible(!store_->error.isEmpty());
}

// ============================================================================
// IgnoreDialog
// ============================================================================
IgnoreDialog::IgnoreDialog(QWidget *parent, IgnoreStore *store) : QDialog(parent), store_(store)
{
    setWindowTitle(tr("Ignored songs"));
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Ignored files are skipped by scans, playlists, and imports."),
                               this));
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    root->addWidget(tree_, 1);
    auto *buttons = new QHBoxLayout;
    auto *removeBtn = new QPushButton(tr("Remove selected"), this);
    connect(removeBtn, &QPushButton::clicked, this, &IgnoreDialog::removeSelected);
    buttons->addWidget(removeBtn);
    auto *clearBtn = new QPushButton(tr("Unignore everything"), this);
    connect(clearBtn, &QPushButton::clicked, this, &IgnoreDialog::clearAll);
    buttons->addWidget(clearBtn);
    buttons->addStretch();
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);
    countLabel_ = new QLabel(this);
    root->addWidget(countLabel_);
    refresh();
    resize(560, 380);
}

void IgnoreDialog::refresh()
{
    tree_->clear();
    for (const QString &path : store_->paths()) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, path);
        item->setData(0, Qt::UserRole, path);
    }
    countLabel_->setText(tr("%1 files ignored").arg(store_->count()));
}

void IgnoreDialog::removeSelected()
{
    QStringList paths;
    for (QTreeWidgetItem *item : tree_->selectedItems())
        paths << item->data(0, Qt::UserRole).toString();
    if (!paths.isEmpty()) {
        store_->change({}, paths);
        refresh();
    }
}

void IgnoreDialog::clearAll()
{
    store_->change({}, store_->paths());
    refresh();
}

// ============================================================================
// AboutDialog
// ============================================================================
AboutDialog::AboutDialog(QWidget *parent, const QString &libopenmptVersion, bool libLoaded)
    : QDialog(parent)
{
    setWindowTitle(tr("About modjuke"));
    auto *root = new QVBoxLayout(this);
    auto *aboutLabel = new QLabel(tr("Made by FlamingLeo, 2026.<br/>"
                                     "Plays MOD, XM, IT, S3M and friends "
                                     "through libopenmpt.<br/>"
                                     "Project page: <a href=\"https://github.com/FlamingLeo/modjuke\">"
                                     "github.com/FlamingLeo/modjuke</a>"),
                                  this);
    aboutLabel->setTextFormat(Qt::RichText);
    aboutLabel->setOpenExternalLinks(true);
    aboutLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    aboutLabel->setWordWrap(true);
    root->addWidget(aboutLabel);
    const QString qtVersion = QString::fromLatin1(qVersion());
    auto *libLabel = new QLabel(
        libLoaded ? tr("libopenmpt %1<br/>Qt %2").arg(libopenmptVersion, qtVersion)
                  : tr("libopenmpt was not found - install libopenmpt0<br/>"
                       "or set MODJUKE_LIBOPENMPT.<br/>Qt %1").arg(qtVersion),
        this);
    libLabel->setTextFormat(Qt::RichText);
    libLabel->setWordWrap(true);
    root->addWidget(libLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    resize(430, 190);
}
