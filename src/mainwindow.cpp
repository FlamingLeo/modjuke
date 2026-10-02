#include "mainwindow.h"
#include "widgets.h"

#include "openmptapi.h"
#include "queue.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

constexpr int kTransportWrapWidth = 880;     // below this the position group wraps

}   // namespace

// ============================================================================
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    settings_ = Settings::load();
    setWindowTitle(QStringLiteral("modjuke"));

    queuePanel_ = new QueuePanel(settings_.hiddenQueueColumns, this);
    buildPages();
    buildTransport();
    buildStatusBar();
    bindShortcuts();

    connect(&engine_, &Engine::logMessage, this, &MainWindow::appendLog);
    connect(&engine_, &Engine::finished, &player_, &QueuePlayer::songFinished);
    connect(&engine_, &Engine::loadReady, this, [this] {
        player_.loadDone();
        tick();
    });
    connect(&player_, &QueuePlayer::songStarted, this, [this](const QString &path) {
        queuePanel_->showPlaying(path);
        trackerPage_->songChanged();   // tick requests its data once the load completes
    });
    connect(&player_, &QueuePlayer::stopped, this, [this] {
        queuePanel_->setPlayingPath(QString());
        trackerPage_->clear();
    });
    connect(&player_, &QueuePlayer::reshuffleNeeded, this, [this](const QString &finished) {
        builder_.drawShuffle(finished.isEmpty() ? anchorPath() : QString(), finished);
        rebuildQueue();
    });
    connect(&player_, &QueuePlayer::status, this, &MainWindow::status);
    connect(&player_, &QueuePlayer::logMessage, this, &MainWindow::appendLog);
    connect(&songActions_, &SongActions::playlistsChanged, this, &MainWindow::refreshSourceCombo);
    connect(&songActions_, &SongActions::queueChanged, this, &MainWindow::rebuildQueue);
    connect(&songActions_, &SongActions::status, this, &MainWindow::status);
    connect(&songActions_, &SongActions::playRequested, this, [this](const QString &path) { player_.play(path); });
    connect(&songActions_, &SongActions::ignoreToggled, this, [this](const QString &path) {
        if (ignored_.contains(path))
            changeIgnored({}, {path});
        else
            ignorePath(path);
    });

    connect(&library_, &MusicLibrary::tracksChanged, this, [this] {
        rebuildQueue();
        showTrackCount();
    });
    connect(&library_, &MusicLibrary::analysisFinished, this,
            [this](const QString &error, bool canceled, int analyzed) {
        queuePanel_->showAnalysisIdle();
        if (!error.isEmpty())
            status(tr("Analysis failed: %1").arg(error));
        else if (canceled)
            status(tr("Analysis canceled"));
        else
            status(tr("Analysis complete: %1 files analyzed").arg(analyzed));
    });
    connect(&library_, &MusicLibrary::analysisSuperseded, this, [this] {
        queuePanel_->showAnalysisIdle();
        if (settings_.autoAnalyze)
            analyzeNow();
    });
    connect(&library_, &MusicLibrary::analysisProgress, queuePanel_, &QueuePanel::showAnalysisProgress);
    connect(&builder_, &QueueBuilder::settingsChanged, this, &MainWindow::commitSettings);
    connect(&builder_, &QueueBuilder::warning, this,
            [this](const QString &text) { appendLog(QStringLiteral("warn"), text); });

    uiTimer_ = new QTimer(this);
    uiTimer_->setTimerType(Qt::PreciseTimer);
    connect(uiTimer_, &QTimer::timeout, this, &MainWindow::tick);
    applySettings(nullptr);
    uiTimer_->start();

    sessionTimer_.start();
    stats_.setEnabled(settings_.trackListeningStats, engine_.snapshot());
    builder_.dropMissingSource();
    {
        // files that couldn't be used: say so once the window is up
        QStringList problems;
        if (!settings_.loadWarning.isEmpty())
            problems << settings_.loadWarning;
        if (playlists_.readError())
            problems << playlists_.error;
        for (const QString &problem : std::as_const(problems))
            appendLog(QStringLiteral("error"), problem);
        if (!problems.isEmpty())
            QTimer::singleShot(0, this, [this, problems] {
                QMessageBox::warning(this, tr("modjuke"), problems.join(QStringLiteral("\n\n")));
            });
    }
    refreshSourceCombo();
    builder_.migrateLegacyShuffle();

    const QRect geometry = keeper_.windowGeometry();
    // only where a screen still is (a disconnected monitor would hide it)
    if (geometry.isValid() && QGuiApplication::screenAt(geometry.center()))
        setGeometry(geometry);
    else if (geometry.isValid())
        resize(geometry.size());
    else
        resize(1180, 720);
    setMinimumSize(760, 460);

    if (!settings_.lastDirectory.isEmpty() && QFileInfo(settings_.lastDirectory).isDir())
        openDirectory(settings_.lastDirectory);
    else {
        // A saved playlist is independent of the last library folder. Restore
        // it even on a playlist-only profile or after that folder disappears.
        rebuildQueue();
        status(settings_.queueSource.isEmpty() ? tr("Open a folder to start (Ctrl+O)")
                                              : tr("Source: %1").arg(settings_.queueSource));
    }

#ifdef MODJUKE_SHOT_DRIVER
    if (qEnvironmentVariableIsSet("MODJUKE_SHOTS"))
        startShotDriver();
#endif
}

void MainWindow::connectQueuePanel()
{
    QueuePanel *panel = queuePanel_;
    connect(panel, &QueuePanel::openFolderClicked, this, &MainWindow::chooseFolder);
    connect(panel, &QueuePanel::rescanClicked, this, &MainWindow::rescan);
    connect(panel, &QueuePanel::analyzeClicked, this, &MainWindow::analyzeNow);
    connect(panel, &QueuePanel::sourceChosen, this, [this](const QString &playlist) {
        builder_.setSource(playlist);
        rebuildQueue();
    });
    connect(panel, &QueuePanel::orderChosen, this, [this](const QString &mode) {
        builder_.setMode(mode);
        rebuildQueue();
        if (builder_.mode() == OrderMode::Shuffle)
            status(tr("Shuffle queue ready - 'Shuffle now' draws a new order (Ctrl+S)"));
    });
    connect(panel, &QueuePanel::shuffleClicked, this, &MainWindow::reshuffle);
    connect(panel, &QueuePanel::playlistsClicked, this, &MainWindow::openPlaylistsDialog);
    connect(panel, &QueuePanel::filterClicked, this, &MainWindow::openFilterDialog);
    connect(panel, &QueuePanel::searchChanged, this, &MainWindow::applyQueueFilters);
    connect(panel, &QueuePanel::addToPlaylistClicked, this, [this] { songActions_.showAddMenu(actionPaths()); });
    connect(panel, &QueuePanel::removeFromPlaylistClicked, this,
            [this] { songActions_.removeFromSource(actionPaths()); });
    connect(panel, &QueuePanel::songActivated, this, [this](const QString &path) { player_.play(path); });
    connect(panel, &QueuePanel::contextMenuRequested, this, [this](const QPoint &pos, const QString &path) {
        songActions_.showContextMenu(pos, path, queuePanel_->selectedPaths());
    });
    connect(panel, &QueuePanel::orderEdited, this, &MainWindow::applyOrderEdited);
    connect(panel, &QueuePanel::columnToggled, this, [this](int column, bool shown) {
        Settings updated = settings_;
        updated.hiddenQueueColumns.removeAll(QueueModel::columnKey(column));
        if (!shown)
            updated.hiddenQueueColumns << QueueModel::columnKey(column);
        const bool saved = keeper_.adopt(updated);
        queuePanel_->applyColumnToggle(column, shown, saved);
        if (!saved)
            QMessageBox::warning(this, tr("Columns"), tr("Could not save column preferences. Check the config folder and qt-ui.json."));
    });
}

void MainWindow::chooseFolder()
{
    const QString start = queuePanel_->folder().isEmpty() ? QDir::homePath() : queuePanel_->folder();
    const QString path = QFileDialog::getExistingDirectory(this, tr("Choose the music folder"), start);
    if (!path.isEmpty())
        openDirectory(path);
}

void MainWindow::buildPages()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);


    // tab strip: [Player][Tracker] ......... [i][Listening stats][Settings]
    auto *tabBarRow = new QWidget(central);
    tabBarRow->setObjectName(QStringLiteral("barPanel"));
    auto *tabLayout = new QHBoxLayout(tabBarRow);
    tabLayout->setContentsMargins(10, 4, 10, 0);
    tabBar_ = new QTabBar(tabBarRow);
    tabBar_->addTab(tr("Player"));
    tabBar_->addTab(tr("Tracker"));
    tabBar_->setExpanding(false);
    tabBar_->setDrawBase(false);
    auto *aboutButton = new QPushButton(QStringLiteral("\u24D8"), tabBarRow);   // ⓘ
    aboutButton->setToolTip(tr("About modjuke"));
    aboutButton->setAccessibleName(tr("About modjuke"));
    aboutButton->setObjectName(QStringLiteral("aboutButton"));
    aboutButton->setText(QString());
    aboutButton->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    aboutButton->setIconSize(QSize(14, 14));
    auto *statsButton = new QPushButton(tr("Listening stats"), tabBarRow);
    statsButton->setObjectName(QStringLiteral("statsButton"));
    auto *settingsButton = new QPushButton(tr("Settings"), tabBarRow);
    tabLayout->addWidget(tabBar_, 0, Qt::AlignLeft);
    tabLayout->addStretch(1);
    tabLayout->addWidget(aboutButton);
    tabLayout->addWidget(statsButton);
    tabLayout->addWidget(settingsButton);
    root->addWidget(tabBarRow);
    root->addWidget(queuePanel_->folderRow());
    root->addWidget(queuePanel_->controlsRow());

    pages_ = new QStackedWidget(central);
    root->addWidget(pages_, 1);
    setCentralWidget(central);

    buildPlayerPage();
    trackerPage_ = new TrackerPage(engine_, pages_);
    pages_->addWidget(playerPage_);
    pages_->addWidget(trackerPage_);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        pages_->setCurrentIndex(index);
        showTrackerTab(index == 1);
    });
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::openAboutDialog);
    connect(statsButton, &QPushButton::clicked, this, &MainWindow::openStatsDialog);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);
    connectQueuePanel();
}

void MainWindow::buildPlayerPage()
{
    playerPage_ = new QWidget(pages_);
    auto *outer = new QVBoxLayout(playerPage_);
    outer->setContentsMargins(10, 4, 10, 4);

    splitter_ = new QSplitter(Qt::Horizontal, playerPage_);
    outer->addWidget(splitter_, 1);

    splitter_->addWidget(queuePanel_);

    infoPanel_ = new InfoPanel(settings_.playAllSubsongs, splitter_);
    connect(infoPanel_, &InfoPanel::favoriteClicked, this, &MainWindow::addCurrentFavorite);
    connect(infoPanel_, &InfoPanel::ignoreClicked, this, &MainWindow::ignoreCurrent);
    connect(infoPanel_, &InfoPanel::songInfoClicked, this, &MainWindow::openSongInfoDialog);
    connect(infoPanel_, &InfoPanel::revealClicked, this, &MainWindow::revealCurrent);
    connect(infoPanel_, &InfoPanel::playAllSubsongsToggled, this, [this](bool on) {
        settings_.playAllSubsongs = on;
        engine_.setPlayAllSubsongs(on);
        commitSettings();
    });
    connect(infoPanel_, &InfoPanel::subsongChosen, this, [this](int index) {
        engine_.setSubsong(index);
        trackerPage_->songChanged();
    });
    splitter_->addWidget(infoPanel_);
    splitter_->setChildrenCollapsible(false);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({600, 400});
}

void MainWindow::buildTransport()
{
    transport_ = new TransportBar(settings_.loopTrack, settings_.loopQueue, settings_.muted,
                                  settings_.volume);
    setContextMenuPolicy(Qt::PreventContextMenu);
    // (transport_ is docked together with the status row in buildStatusBar)
    connect(transport_, &TransportBar::previousClicked, this, &MainWindow::playPrevious);
    connect(transport_, &TransportBar::playClicked, this, &MainWindow::togglePlay);
    connect(transport_, &TransportBar::nextClicked, this, &MainWindow::playNext);
    connect(transport_, &TransportBar::stopClicked, &player_, &QueuePlayer::stop);
    connect(transport_, &TransportBar::loopToggled, this, [this](bool on) {
        settings_.loopTrack = on;
        engine_.setLoopTrack(on);
        commitSettings();
    });
    connect(transport_, &TransportBar::repeatToggled, this, [this](bool on) {
        settings_.loopQueue = on;
        commitSettings();
    });
    connect(transport_, &TransportBar::muteToggled, this, [this](bool on) {
        settings_.muted = on;
        engine_.setMuted(on);
        commitSettings();
    });
    connect(transport_, &TransportBar::volumeChanged, this, [this](int value) {
        settings_.volume = value;
        engine_.setVolume(value);
    });
    connect(transport_, &TransportBar::volumeCommitted, this, &MainWindow::commitSettings);
    connect(transport_, &TransportBar::seekRequested, this,
            [this](double fraction) { engine_.seekFraction(fraction); });
}

void MainWindow::buildStatusBar()
{
    // Keep status and transport inside the central layout. A QDockWidget would
    // add a vertical splitter handle above these rows, letting the bottom area
    // be resized independently from the player.
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("barPanel"));
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 2, 10, 2);
    statusLabel_ = new ElidedLabel(bar);
    statusLabel_->setObjectName(QStringLiteral("dimLabel"));
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long messages are shortened
    statusLabel_->setFullText(tr("Ready"));
    healthLabel_ = new QLabel(bar);
    healthLabel_->setObjectName(QStringLiteral("dimLabel"));
    resetButton_ = new QPushButton(tr("Reset layout"), bar);
    resetButton_->setObjectName(QStringLiteral("miniButton"));
    row->addWidget(statusLabel_, 1);
    row->addWidget(healthLabel_);
    row->addWidget(resetButton_);
    connect(resetButton_, &QPushButton::clicked, this, &MainWindow::resetLayout);
    healthLabel_->setToolTip(tr("Layout reset: Ctrl+0"));

    auto *bottom = new QWidget(centralWidget());
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    bl->addWidget(bar);
    bl->addWidget(transport_);
    centralWidget()->layout()->addWidget(bottom);
}

void MainWindow::bindShortcuts()
{
    // Plain keys (Space, letters, arrows, +/-/0) belong to a text field while
    // one is edited; Ctrl combinations, F-keys and Escape work everywhere.
    auto key = [this](const QKeySequence &sequence, std::function<void()> action) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        const QKeyCombination combo = sequence[0];
        const Qt::Key k = combo.key();
        const bool textKey = combo.keyboardModifiers() == Qt::NoModifier
                                 ? !(k >= Qt::Key_F1 && k <= Qt::Key_F35) && k != Qt::Key_Escape
                                 : (k == Qt::Key_Left || k == Qt::Key_Right);   // word moves
        connect(shortcut, &QShortcut::activated, this, [this, textKey, action = std::move(action)] {
            if (textKey && typing())
                return;
            action();
        });
    };
    key(Qt::Key_Space, [this] { togglePlay(); });
    key(Qt::Key_Return, [this] { player_.playSelected(queuePanel_->selectedPath()); });
    key(Qt::Key_PageUp, [this] { playPrevious(); });
    key(Qt::Key_PageDown, [this] { playNext(); });
    key(Qt::Key_Left, [this] { engine_.seekRelative(-5); });
    key(Qt::Key_Right, [this] { engine_.seekRelative(5); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { engine_.seekRelative(-30); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { engine_.seekRelative(30); });
    key(Qt::Key_L, [this] { transport_->toggleLoop(); });
    key(Qt::Key_R, [this] { transport_->toggleRepeat(); });
    key(Qt::Key_M, [this] { transport_->toggleMute(); });
    key(Qt::Key_Plus, [this] { transport_->stepVolume(+5); });
    key(Qt::Key_Equal, [this] { transport_->stepVolume(+5); });
    key(Qt::Key_Minus, [this] { transport_->stepVolume(-5); });
    key(Qt::Key_0, [this] { transport_->setVolume(0); });
    key(Qt::Key_F1, [this] { openAboutDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_O), [this] { chooseFolder(); });
    key(Qt::Key_F5, [this] { rescan(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_F), [this] { queuePanel_->focusSearch(); });
    key(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] { openFilterDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_S), [this] { reshuffle(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_P), [this] { openPlaylistsDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_I), [this] { openSongInfoDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { openStatsDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_R), [this] { revealCurrent(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { resetLayout(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Up), [this] { moveSelectedRow(-1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Down), [this] { moveSelectedRow(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_T), [this] {
        tabBar_->setCurrentIndex(tabBar_->currentIndex() == 0 ? 1 : 0);
    });
    key(Qt::Key_Escape, [this] { queuePanel_->clearSearch(); });
}

bool MainWindow::typing() const
{
    // read-only fields (folder path, log) take no text: shortcuts stay on
    QWidget *focus = QApplication::focusWidget();
    if (auto *line = qobject_cast<QLineEdit *>(focus))
        return !line->isReadOnly();
    if (auto *text = qobject_cast<QPlainTextEdit *>(focus))
        return !text->isReadOnly();
    return qobject_cast<QSpinBox *>(focus) != nullptr;
}

// ============================================================================
// library / queue
// ============================================================================
void MainWindow::openDirectory(const QString &path)
{
    const QString root = QFileInfo(path).absoluteFilePath();
    queuePanel_->setFolder(root);
    settings_.lastDirectory = root;
    settings_.lastPickerDir = root;
    commitSettings();
    scanFolder(root);
}

void MainWindow::rescan()
{
    scanFolder(library_.root());
}

void MainWindow::scanFolder(const QString &root)
{
    if (root.isEmpty()) {
        status(tr("Choose a folder first (Open folder...)"));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    status(tr("Scanning %1...").arg(root));
    const QStringList errors = library_.rescan(root);
    QApplication::restoreOverrideCursor();
    showTrackCount();
    if (!errors.isEmpty()) {
        status(tr("Scan finished with %1 problems").arg(errors.size()));
        for (int i = 0; i < std::min(20, int(errors.size())); ++i)   // which ones (log)
            appendLog(QStringLiteral("warn"), errors.at(i));
    } else {
        status(tr("Scanned %1 tracks in %2").arg(library_.tracks().size()).arg(root));
    }
    rebuildQueue();
    if (library_.pendingCount() > 0 && settings_.autoAnalyze && !library_.analyzing())
        analyzeNow();
}

void MainWindow::showTrackCount()
{
    queuePanel_->setTrackCount(library_.tracks().size(), library_.dirCount());
}

void MainWindow::analyzeNow()
{
    if (library_.analyzing()) {
        library_.cancelAnalysis();
        status(tr("Canceling analysis..."));
        return;
    }
    const int pending = library_.pendingCount();
    if (!library_.analyze(settings_.cacheAnalysis)) {
        status(tr("Everything is already analyzed"));
        return;
    }
    queuePanel_->showAnalysisProgress(0, pending);
    status(tr("Analyzing %1 files in the background").arg(pending));
}

void MainWindow::rebuildQueue()
{
    queue_.setOrdered(builder_.ordered(anchorPath()), builder_.sourceKey());
    applyQueueFilters();
}

void MainWindow::applyQueueFilters()
{
    const QueueFilter filter = builder_.filter();
    queue_.narrow(queuePanel_->searchText(), filter, currentPath());
    queuePanel_->showQueue(queue_.tracks(), builder_.mode() == OrderMode::ByDirectory, currentPath(),
                           builder_.reorderable());
    queuePanel_->setFilterText(filter.active() ? filter.describe() : QString());

    refreshQueueControls();
    const QStringList context{library_.root(), settings_.queueSource, settings_.queueMode};
    if (context != queueViewContext_) {
        queueViewContext_ = context;
        queuePanel_->revealPath(currentPath());
    }
}

void MainWindow::refreshQueueControls()
{
    queuePanel_->setSourceKind(!settings_.queueSource.isEmpty(), settings_.queueMode);
    queuePanel_->setShuffleEnabled(!queue_.isEmpty());
}

void MainWindow::refreshSourceCombo()
{
    queuePanel_->setSources(playlists_, settings_.queueSource);
}

void MainWindow::reshuffle()
{
    // the "Shuffle now" button: draw a brand new order, current track first
    builder_.reshuffle(anchorPath());
    rebuildQueue();
    queuePanel_->revealPath(currentPath());
    QString tail;
    if (!queue_.isEmpty())
        tail = tr(" - starting from %1").arg(queue_.at(0).displayTitle());
    status(tr("New shuffle order drawn (Ctrl+S)%1").arg(tail));
}

void MainWindow::applyOrderEdited(const QStringList &newOrder)
{
    if (!builder_.reorderable())
        return;
    const bool saved = builder_.saveOrder(queue_.paths(), newOrder);
    rebuildQueue();
    status(saved ? tr("Saved the new order to \"%1\"").arg(settings_.queueSource) : playlists_.error);
}

void MainWindow::moveSelectedRow(int delta)
{
    if (settings_.queueSource.isEmpty() || builder_.mode() != OrderMode::Saved)
        return;
    const QString path = queuePanel_->currentRowPath();
    if (path.isEmpty())
        return;
    QStringList order = queue_.paths();
    const int from = order.indexOf(path);
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    applyOrderEdited(order);
    queuePanel_->setCurrentPath(path);
}

// ============================================================================
// playback
// ============================================================================
QString MainWindow::currentPath() const
{
    return engine_.snapshot().path;
}

QString MainWindow::anchorPath() const
{
    return currentPath().isEmpty() ? queuePanel_->selectedPath() : currentPath();
}

void MainWindow::togglePlay()
{
    player_.togglePlay(queuePanel_->selectedPath());
}

void MainWindow::playPrevious()
{
    player_.previous(queuePanel_->selectedPath());
}

void MainWindow::playNext()
{
    player_.next(queuePanel_->selectedPath());
}

void MainWindow::addCurrentFavorite()
{
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    const bool on = songActions_.toggleFavorite(path);
    infoPanel_->setFavorite(on);
    status(on ? tr("Added to Favorites") : tr("Removed from Favorites"));
}

bool MainWindow::confirmIgnorePath(const QString &path)
{
    if (!settings_.confirmIgnore)
        return true;
    QMessageBox question(QMessageBox::Question, tr("Ignore this file"),
        tr("Ignore %1? It will be skipped everywhere unless you un-ignore it in Settings. "
           "The music file and saved playlist memberships will be kept.")
            .arg(QFileInfo(path).fileName()),
        QMessageBox::Yes | QMessageBox::No, this);
    question.setTextFormat(Qt::PlainText);
    question.setDefaultButton(QMessageBox::No);
    auto *skip = new QCheckBox(tr("Don't ask again when ignoring songs"), &question);
    skip->setObjectName(QStringLiteral("skipIgnoreConfirmation"));
    question.setCheckBox(skip);
    if (question.exec() != QMessageBox::Yes)
        return false; // cancellation never changes the preference or playback
    if (skip->isChecked()) {
        Settings updated = settings_;
        updated.confirmIgnore = false;
        if (!keeper_.adopt(updated)) {
            QMessageBox::warning(this, tr("Ignore confirmation"),
                tr("Could not save this preference. You may be asked again next time."));
        }
        if (settingsDialog_)
            settingsDialog_->setConfirmIgnore(settings_.confirmIgnore);
    }
    return true;
}

void MainWindow::ignoreCurrent()
{
    const auto snap = engine_.snapshot();
    if (!snap.path.isEmpty() && (snap.loaded || snap.loading))
        ignorePath(snap.path);
}

void MainWindow::ignorePath(const QString &path)
{
    if (path.isEmpty() || ignored_.contains(path) || !confirmIgnorePath(path))
        return;
    changeIgnored({path});
}

bool MainWindow::changeIgnored(const QStringList &add, const QStringList &remove)
{
    const EngineSnapshot snap = engine_.snapshot();
    if (!ignored_.change(add, remove)) {
        if (!ignored_.error.isEmpty())
            QMessageBox::warning(this, tr("Ignored songs"), ignored_.error);
        return false; // failed writes must not interrupt playback
    }
    const bool skip = !snap.path.isEmpty() && ignored_.contains(snap.path);
    QueuePlayer::Successor next;
    if (skip)
        next = player_.stopIgnored(snap);   // chosen in the queue before the rebuild
    rebuildQueue();
    if (skip)
        player_.playSuccessor(next);
    else
        status(tr("Ignore list updated - music files and saved memberships kept"));
    if (settingsDialog_)
        settingsDialog_->refreshIgnoredCount();
    return true;
}

void MainWindow::revealCurrent()
{
    const QString path = anchorPath();
    if (!path.isEmpty())
        songActions_.reveal(path);
}

QStringList MainWindow::actionPaths() const
{
    // the selected songs, or else the playing one
    QStringList paths = queuePanel_->selectedPaths();
    if (paths.isEmpty() && !currentPath().isEmpty())
        paths << currentPath();
    return paths;
}

// ============================================================================
// dialogs
// ============================================================================
void MainWindow::openFilterDialog()
{
    FilterDialog dialog(this, builder_.filter(), builder_.sourceTracks());   // the queue's source
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QueueFilter filter = dialog.criteria();
    builder_.setFilter(filter);
    rebuildQueue();
    status(filter.active() ? tr("Filter: %1").arg(filter.describe()) : tr("Filter cleared"));
}

void MainWindow::openPlaylistsDialog()
{
    PlaylistsDialog dialog(this, &playlists_, [this] {
        QStringList paths = queuePanel_->selectedPaths();
        return paths.isEmpty() ? queue_.paths() : paths;
    });
    connect(&dialog, &PlaylistsDialog::loadRequested, this, [this](const QString &name, bool savedOrder) {
        // Load keeps the order choice; a new or imported list opens in
        // Saved order (README)
        builder_.setSource(name, savedOrder);
        refreshSourceCombo();
        rebuildQueue();
        status(tr("Source: %1").arg(name));
    });
    connect(&dialog, &PlaylistsDialog::saveQueueRequested, this,
            [this](const QString &name) { songActions_.saveQueueAs(name, queue_.paths()); });
    connect(&dialog, &PlaylistsDialog::renamed, this, [this](const QString &oldName, const QString &newName) {
        builder_.playlistRenamed(oldName, newName);
        refreshSourceCombo();
        rebuildQueue();
    });
    connect(&dialog, &PlaylistsDialog::removed, this, [this](const QString &name) {
        if (builder_.playlistRemoved(name))
            status(tr("The selected playlist is gone - Library selected (Ctrl+P)"));
        refreshSourceCombo();
        rebuildQueue();
    });
    dialog.exec();
    refreshSourceCombo();
    rebuildQueue();
}

void MainWindow::openSettingsDialog()
{
    // Exactly one settings window can be open; Settings clicks raise it.
    if (settingsDialog_) {
        settingsDialog_->setVisible(true);
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }
    auto *dialog = new SettingsDialog(this, settings_, &ignored_,
                                      [this](const QStringList &paths) { return changeIgnored({}, paths); });
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    settingsDialog_ = dialog;
    dialog->setSaveHandler([this, dialog] { return applySettingsFromDialog(dialog); });
    connect(dialog, &QDialog::rejected, dialog, &QWidget::close);
    dialog->show();
}

bool MainWindow::applySettingsFromDialog(SettingsDialog *dialog)
{
    Settings updated = settings_;
    dialog->collect(updated);
    const Settings previous = settings_;
    if (!keeper_.adopt(updated)) {
        QMessageBox::warning(this, tr("Settings"),
                             tr("Could not save settings. Check the config folder and qt-ui.json."));
        return false;
    }
    stats_.setEnabled(settings_.trackListeningStats, engine_.snapshot());
    applySettings(&previous);
    status(tr("Settings saved"));
    return true;
}

void MainWindow::openAboutDialog()
{
    OpenMPTLib *lib = OpenMPTLib::instance();
    AboutDialog dialog(this, lib ? lib->versionString() : QString(), lib != nullptr);
    dialog.exec();
}

void MainWindow::openStatsDialog()
{
    stats_.flush(engine_.snapshot());
    stats_.save();
    StatsDialog dialog(this, &stats_.store());
    connect(&dialog, &StatsDialog::statsReset, this,
            [this] { stats_.afterReset(engine_.snapshot()); });
    dialog.exec();
}

void MainWindow::openSongInfoDialog()
{
    const EngineSnapshot snap = engine_.snapshot();
    if (!snap.loaded) {
        status(tr("Nothing playing"));
        return;
    }
    SongInfoDialog dialog(this, snap.info);
    dialog.exec();
}

void MainWindow::showTrackerTab(bool show)
{
    // Folder/queue controls belong to Player, not the shared tab strip.
    // Hiding the complete rows also returns their layout space to Tracker.
    queuePanel_->setRowsVisible(!show);
    if (show)
        trackerPage_->sync(engine_.snapshot());
}

// ============================================================================
// tick + panels
// ============================================================================
void MainWindow::tick()
{
    const EngineSnapshot snap = engine_.snapshot();
    trackerPage_->refresh(snap);
    transport_->refresh(snap, queue_.indexOf(snap.path), queue_.size());
    updateLoadStatus(snap);
    infoPanel_->refresh(snap, {queue_.isEmpty(), ignored_.contains(snap.path),
                               !snap.path.isEmpty() && playlists_.isFavorite(snap.path),
                               engine_.outputDescription()});
    updateWindowTitle(snap);

    stats_.update(snap);

    if (sessionTimer_.elapsed() >= 15000)
        saveSession();

    healthLabel_->setToolTip(engine_.outputDescription());

    transport_->setCompact(width() < kTransportWrapWidth);
}

void MainWindow::updateLoadStatus(const EngineSnapshot &snap)
{
    // replace the load message once the load is done (it used to be found by
    // its text, which a translation would break)
    if (snap.loading) {
        status(tr("Loading %1...").arg(QFileInfo(snap.path).fileName()));
        statusShowsLoading_ = true;
    } else if (!snap.loadError.isEmpty()) {
        status(snap.loadError);
    } else if (snap.loaded && statusShowsLoading_) {
        status(tr("Playing %1").arg(QFileInfo(snap.path).fileName()));
    }
}

void MainWindow::updateWindowTitle(const EngineSnapshot &snap)
{
    QString title = QStringLiteral("modjuke");
    if (settings_.windowTitleMode != QLatin1String("none") && !snap.path.isEmpty()) {
        QString name;
        if (settings_.windowTitleMode == QLatin1String("filename"))
            name = QFileInfo(snap.path).fileName();
        else if (settings_.windowTitleMode == QLatin1String("title") && !snap.info.title.isEmpty())
            name = snap.info.title;
        else
            name = snap.info.title.isEmpty() ? QFileInfo(snap.path).fileName() : snap.info.title;
        title = QStringLiteral("%1 - modjuke").arg(name);
    }
    if (windowTitle() != title)
        setWindowTitle(title);
}

void MainWindow::saveSession(bool force)
{
    if (!settings_.rememberPosition && !force)
        return;
    keeper_.rememberPlayback(engine_.snapshot());
    sessionTimer_.restart();
    commitSettings();
}

void MainWindow::commitSettings()
{
    keeper_.save();
}

// ============================================================================
// misc
// ============================================================================
void MainWindow::applyTheme()
{
    palette_ = Palette::resolve(settings_.theme, settings_.customThemes);
    qApp->setStyleSheet(palette_.toStyleSheet());
    if (auto *about = findChild<QPushButton *>(QStringLiteral("aboutButton"))) {
        if (auto *stats = findChild<QPushButton *>(QStringLiteral("statsButton"))) {
            about->setFont(stats->font());
            about->setFixedHeight(stats->sizeHint().height());
        }
    }
    transport_->applyPalette(&palette_);
    infoPanel_->applyPalette(&palette_);
    trackerPage_->applyPalette(&palette_);
    queuePanel_->applyPalette(palette_);
}

void MainWindow::resetLayout()
{
    queuePanel_->resetLayout();
    if (splitter_)
        splitter_->setSizes({600, 400});
    status(tr("Layout reset - column widths, the pane split and scrolling are back to "
              "how the window opened (Ctrl+0)"));
}

void MainWindow::appendLog(const QString &level, const QString &text)
{
    if (infoPanel_)
        infoPanel_->appendLog(level, text);
}

void MainWindow::status(const QString &text)
{
    statusLabel_->setFullText(text);
    statusShowsLoading_ = false;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stats_.flush(engine_.snapshot());
    stats_.save();   // writes only when something changed
    saveSession(true);
    keeper_.setWindowGeometry(geometry());
    engine_.stop();
    keeper_.save();
    QMainWindow::closeEvent(event);
}

void MainWindow::applySettings(const Settings *previous)
{
    // Pushes settings_ to the engine and to what shows them; `previous` is
    // what was applied before (nullptr at startup: everything).
    const bool all = previous == nullptr;
    if (all || previous->backend != settings_.backend || previous->samplerate != settings_.samplerate
        || previous->bufferMs != settings_.bufferMs || previous->interpolation != settings_.interpolation)
        engine_.applySettings(settings_);   // also the mixer; rebuilds a running output
    else if (previous->volume != settings_.volume)
        engine_.setVolume(settings_.volume);
    uiTimer_->setInterval(std::max(1000 / std::clamp(settings_.uiFps, 5, 120), 8));
    trackerPage_->setSmoothScrolling(settings_.smoothTrackerScrolling);
    healthLabel_->setText(tr("buffer %1 ms").arg(settings_.bufferMs));
    if (all || previous->theme != settings_.theme || previous->customThemes != settings_.customThemes)
        applyTheme();
}

void MainWindow::applySessionOptions(SessionOptions options)
{
    const Settings &before = keeper_.beginSession(options);
    if (options.volume)
        transport_->setVolume(settings_.volume);   // the slider showed the saved volume
    applySettings(&before);   // the constructor applied the saved ones
    if (options.speed)
        engine_.setTempoFactor(*options.speed);
}

void MainWindow::startupPlay(const QString &track, bool autoplay)
{
    QTimer::singleShot(0, this, [this, track, autoplay] {
        if (!track.isEmpty()) {
            const QString path = QFileInfo::exists(track)
                ? QFileInfo(track).absoluteFilePath()
                : track;
            player_.play(path);
            return;
        }
        if (autoplay && !queue_.isEmpty()) {
            player_.playQueued(0, +1);   // the app's choice: skip on if broken
            return;
        }
        if (settings_.rememberPosition && !settings_.lastPath.isEmpty()
            && QFile::exists(settings_.lastPath))
            player_.play(settings_.lastPath, settings_.lastPosition, true, settings_.subsong);
    });
}
