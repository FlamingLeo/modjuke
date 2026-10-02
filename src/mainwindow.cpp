#include "mainwindow.h"
#include "casefold.h"

#include "openmptapi.h"
#include "stores.h"
#include "reveal.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRandomGenerator>
#include <QCheckBox>
#include <QClipboard>
#include <QSet>
#include <QShortcut>
#include <functional>
#include <cmath>
#include <QComboBox>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

quint32 freshShuffleSeed(qint64 previous)
{
    quint32 seed;
    do { seed = QRandomGenerator::global()->bounded(1u, 2000000000u); }
    while (seed == quint32(previous));
    return seed;
}

constexpr int kTransportWrapWidth = 880;     // below this the position group wraps

const QStringList &defaultExtensions()
{
    static const QStringList exts = {
        QStringLiteral("669"), QStringLiteral("amf"), QStringLiteral("ams"),
        QStringLiteral("dbm"), QStringLiteral("dmf"), QStringLiteral("dsm"),
        QStringLiteral("dtm"), QStringLiteral("far"), QStringLiteral("gdm"),
        QStringLiteral("it"), QStringLiteral("j2b"), QStringLiteral("med"),
        QStringLiteral("mdl"), QStringLiteral("mod"), QStringLiteral("mptm"),
        QStringLiteral("mt2"), QStringLiteral("mtm"), QStringLiteral("noiser"),
        QStringLiteral("okta"), QStringLiteral("pt3"), QStringLiteral("s3m"),
        QStringLiteral("sfx"), QStringLiteral("stm"), QStringLiteral("stx"),
        QStringLiteral("ult"), QStringLiteral("wow"), QStringLiteral("xm"),
    };
    return exts;
}

QStringList scanExtensions(OpenMPTLib *lib)
{
    if (lib) {
        const QStringList supported = lib->supportedExtensions();
        if (!supported.isEmpty())
            return supported;
    }
    return defaultExtensions();
}

QRect parseGeometry(const QString &text)
{
    // Tk geometry: "+X" is a position (X may be negative: "+-1920" on a
    // monitor left of the primary one, which the old pattern rejected);
    // "-X" counts from the far edge (approximated as before)
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)x(\\d+)([+-])(-?\\d+)([+-])(-?\\d+)$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return QRect();
    int x = m.captured(4).toInt(), y = m.captured(6).toInt();
    if (m.captured(3) == QLatin1String("-"))
        x = 100 - x;
    if (m.captured(5) == QLatin1String("-"))
        y = 100 - y;
    return QRect(x, y, m.captured(1).toInt(), m.captured(2).toInt());
}

}   // namespace

// ============================================================================
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    settings_ = Settings::load();
    setWindowTitle(QStringLiteral("modjuke"));

    buildToolbar();
    buildQueueBar();
    buildPages();
    buildTransport();
    buildStatusBar();
    bindShortcuts();

    engine_.applySettings(settings_);
    engine_.setVolume(settings_.volume);
    engine_.setMuted(settings_.muted);
    engine_.setLoopTrack(settings_.loopTrack);
    connect(&engine_, &Engine::logMessage, this, &MainWindow::appendLog);
    connect(&engine_, &Engine::finished, this, &MainWindow::onFinished);
    connect(&engine_, &Engine::loadReady, this, [this] {
        handleLoadResult();
        tick();
    });
    connect(&engine_, &Engine::songDataReady, this, [this](const SongDataReply &reply) {
        if (reply.token != songToken_ || reply.orders.isEmpty())
            return;
        tracker_->setFamily(engine_.snapshot().info.format.toLower());
        tracker_->setSong(reply.path, reply.orders, reply.rows, reply.channels, reply.numPatterns);
        const auto snap = engine_.snapshot();
        tracker_->setPlaying(snap.order, snap.row, snap.playing, snap.paused);
    });
    connect(tracker_, &TrackerView::patternNeeded, this, [this](int pattern) {
        engine_.requestPattern(songToken_, pattern, engine_.snapshot().channels);
    });
    connect(&engine_, &Engine::patternDataReady, this, [this](const PatternDataReply &reply) {
        if (reply.token != songToken_)
            return;
        tracker_->setPatternData(reply);
    });

    connect(&analyzer_, &Analyzer::resultsReady, this, [this](const AnalysisResults &results) {
        if (analysisRestartPending_)
            return; // a rescan superseded this job
        for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
            auto trackIt = trackByPath_.find(it.key());
            if (trackIt == trackByPath_.end())
                continue;
            Track &track = trackIt.value();
            const CachedModule &entry = it.value();
            track.analyzed = true;
            track.duration = entry.duration;
            track.fmt = entry.fmt;
            track.channels = entry.channels;
            track.subsongs = entry.subsongs;
            track.title = entry.title;
            track.broken = entry.broken;
        }
        // Both playlist lookup and library/filter views must see the same metadata.
        for (Track &track : libraryTracks_)
            track = trackByPath_.value(track.path, track);
        analyzedTotal_ = results.size();
        rebuildQueue();
        countLabel_->setText(QStringLiteral("%1 tracks, %2 folders")
                                 .arg(libraryTracks_.size()).arg(libraryDirs_));
    });
    connect(&analyzer_, &Analyzer::finished, this, [this](const QString &error, bool canceled) {
        analyzeButton_->setText(tr("Analyze"));
        if (analysisRestartPending_) {
            analysisRestartPending_ = false;
            if (settings_.autoAnalyze)
                analyzeNow();
            return;
        }
        if (!error.isEmpty())
            status(tr("Analysis failed: %1").arg(error));
        else if (canceled)
            status(tr("Analysis canceled"));
        else
            status(tr("Analysis complete: %1 files analyzed").arg(analyzedTotal_));
    });
    connect(&analyzer_, &Analyzer::progress, this, [this](int done, int total) {
        if (!analysisRestartPending_)
            analyzeButton_->setText(tr("Analyzing %1/%2").arg(done).arg(total));
    });

    uiTimer_ = new QTimer(this);
    uiTimer_->setTimerType(Qt::PreciseTimer);
    connect(uiTimer_, &QTimer::timeout, this, &MainWindow::tick);
    uiTimer_->start(std::max(1000 / std::clamp(settings_.uiFps, 5, 120), 8));

    sessionTimer_.start();
    statsTimer_.start();

    applyTheme();
    {
        // same startup normalization as ui.py __init__: a restored playlist
        // source that vanished falls back to the library (its dangling shuffle
        // order and legacy list are dropped), and "playlist" ordering without a
        // playlist resets to "by directory"
        bool changed = false;
        // (an unreadable playlists file isn't "the playlist is gone")
        if (!settings_.queueSource.isEmpty() && !playlists_.get(settings_.queueSource)
            && !playlists_.readError()) {
            settings_.queueSource.clear();
            settings_.activePlaylist.clear();
            settings_.shufflePaths.clear();
            changed = true;
        }
        if (settings_.queueMode == QLatin1String("playlist") && settings_.queueSource.isEmpty()) {
            settings_.queueMode = QStringLiteral("by directory");
            changed = true;
        }
        if (changed)
            commitSettings();
    }
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
    migrateShuffleOrder();

    const QRect geometry = parseGeometry(settings_.windowGeometry);
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

    if (qEnvironmentVariableIsSet("MODJUKE_SHOTS"))
        startShotDriver();
}

void MainWindow::buildToolbar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 6, 10, 2);
    openButton_ = new QPushButton(tr("Open folder\u2026"), bar);
    openButton_->setObjectName(QStringLiteral("accentButton"));
    rescanButton_ = new QPushButton(tr("Rescan"), bar);
    analyzeButton_ = new QPushButton(tr("Analyze"), bar);
    dirField_ = new QLineEdit(bar);
    dirField_->setReadOnly(true);
    dirField_->setPlaceholderText(tr("no library folder"));
    countLabel_ = new QLabel(bar);
    countLabel_->setObjectName(QStringLiteral("dimLabel"));
    layout->addWidget(openButton_);
    layout->addWidget(rescanButton_);
    layout->addWidget(analyzeButton_);
    layout->addWidget(dirField_, 1);
    layout->addWidget(countLabel_);
    toolbarRow_ = bar;
    toolbarRow_->setObjectName(QStringLiteral("libraryToolbar"));

    connect(openButton_, &QPushButton::clicked, this, [this] {
        const QString start = dirField_->text().isEmpty() ? QDir::homePath() : dirField_->text();
        const QString path = QFileDialog::getExistingDirectory(this, tr("Choose the music folder"),
                                                               start);
        if (!path.isEmpty())
            openDirectory(path);
    });
    connect(rescanButton_, &QPushButton::clicked, this, &MainWindow::rescan);
    connect(analyzeButton_, &QPushButton::clicked, this, &MainWindow::analyzeNow);
}

void MainWindow::buildQueueBar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 2, 10, 4);
    auto *sourceLabel = new QLabel(tr("Source:"), bar);
    sourceCombo_ = new QComboBox(bar);
    sourceCombo_->setMinimumWidth(140);
    auto *orderLabel = new QLabel(tr("Order:"), bar);
    orderCombo_ = new QComboBox(bar);
    orderCombo_->setMinimumWidth(110);
    shuffleBtn_ = new QPushButton(tr("Shuffle now"), bar);
    playlistsBtn_ = new QPushButton(tr("Playlists"), bar);
    filterBtn_ = new QPushButton(tr("Filter"), bar);
    auto *searchLabel = new QLabel(tr("Search:"), bar);
    searchEdit_ = new QLineEdit(bar);
    searchEdit_->setPlaceholderText(tr("title or file name"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMaximumWidth(220);
    filterLabel_ = new QLabel(bar);
    filterLabel_->setObjectName(QStringLiteral("dimLabel"));
    // Spare width belongs to the trailing stretch, not to the text labels.
    for (QWidget *widget : std::initializer_list<QWidget *>{static_cast<QWidget *>(sourceLabel), orderLabel, searchLabel,
                            static_cast<QWidget *>(sourceCombo_), orderCombo_})
        widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    // A long filter description must not widen the window's minimum size
    // (that clipped the transport instead): it's cut, the tooltip has it all.
    filterLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    filterLabel_->setMinimumWidth(40);
    layout->setSpacing(6);
    layout->addWidget(sourceLabel);
    layout->addWidget(sourceCombo_);
    layout->addWidget(orderLabel);
    layout->addWidget(orderCombo_);
    layout->addWidget(shuffleBtn_);
    layout->addWidget(playlistsBtn_);
    layout->addWidget(filterBtn_);
    layout->addWidget(searchLabel);
    layout->addWidget(searchEdit_, 1);
    layout->addWidget(filterLabel_);
    layout->addStretch(1);

    connect(sourceCombo_, &QComboBox::activated, this, [this](int) {
        settings_.queueSource = sourceCombo_->currentData().toString();
        settings_.activePlaylist = settings_.queueSource;
        // Saved order exists only for playlists (README: switching to the
        // library changes it to By directory)
        if (settings_.queueSource.isEmpty() && settings_.queueMode == QLatin1String("playlist"))
            settings_.queueMode = QStringLiteral("by directory");
        commitSettings();
        refreshQueueControls();
        rebuildQueue();
    });
    connect(orderCombo_, &QComboBox::activated, this, [this](int) {
        // Changing the ordering never redraws a saved shuffle.
        settings_.queueMode = orderCombo_->currentData().toString();
        commitSettings();
        rebuildQueue();
        if (orderModeFromName(settings_.queueMode) == OrderMode::Shuffle)
            status(tr("Shuffle queue ready - 'Shuffle now' draws a new order (Ctrl+S)"));
    });
    connect(shuffleBtn_, &QPushButton::clicked, this, &MainWindow::reshuffle);
    connect(playlistsBtn_, &QPushButton::clicked, this, &MainWindow::openPlaylistsDialog);
    connect(filterBtn_, &QPushButton::clicked, this, &MainWindow::openFilterDialog);
    connect(searchEdit_, &QLineEdit::textChanged, this, [this] { applyQueueFilters(); });
    searchEdit_->installEventFilter(this);
    queueBar_ = bar;
    queueBar_->setObjectName(QStringLiteral("queueControls"));
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
    if (toolbarRow_)
        root->addWidget(toolbarRow_);
    if (queueBar_)
        root->addWidget(queueBar_);

    pages_ = new QStackedWidget(central);
    root->addWidget(pages_, 1);
    setCentralWidget(central);

    buildPlayerPage();
    buildTrackerPage();
    pages_->addWidget(playerPage_);
    pages_->addWidget(trackerPage_);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        pages_->setCurrentIndex(index);
        showTrackerTab(index == 1);
    });
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::openAboutDialog);
    connect(statsButton, &QPushButton::clicked, this, &MainWindow::openStatsDialog);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);

    // queue table wiring
    connect(queueView_, &QueueTableView::activatedPath, this,
            [this](const QString &path) { playPath(path); });
    connect(queueView_, &QueueTableView::contextMenuFor, this,
            [this](const QPoint &globalPos, const QString &path) {
                // Directory headers are structural rows, not songs. Do not
                // offer song actions (especially Add to playlist) for them.
                if (path.isEmpty())
                    return;
                QMenu menu(this);
                menu.addAction(tr("Play now"), [this, path] { playPath(path); });
                menu.addSeparator();
                menu.addAction(ignored_.contains(path) ? tr("Un-ignore this file")
                                                        : tr("Ignore this file"),
                               [this, path] {
                                   if (ignored_.contains(path))
                                       changeIgnored({}, {path});
                                   else
                                       ignorePath(path);
                               });
                menu.addAction(tr("Toggle favorite"), [this, path] {
                    playlists_.toggleFavorite(path);
                    refreshSourceCombo();
                    if (PlaylistStore::isFavorites(settings_.queueSource))
                        rebuildQueue();
                });
                QMenu *addTo = menu.addMenu(tr("Add to playlist"));
                const QStringList selected = queueSelectedPaths();
                for (const QString &name : playlists_.names()) {
                    if (PlaylistStore::isFavorites(name))
                        continue;
                    addTo->addAction(name, [this, name, selected, path] {
                        QStringList paths = selected.isEmpty() ? QStringList{path} : selected;
                        playlists_.addPaths(name, paths);
                        refreshSourceCombo();
                    });
                }
                addTo->addAction(tr("New playlist\u2026"), [this, selected, path] {
                    bool ok = false;
                    const QString base = QInputDialog::getText(this, tr("New playlist"),
                                                                tr("Playlist name:"),
                                                                QLineEdit::Normal,
                                                                QStringLiteral("Playlist"), &ok);
                    if (!ok)
                        return;
                    QString err;
                    const QString name = playlists_.uniqueName(base, QStringLiteral(" %1"), &err);
                    if (name.isEmpty()) {
                        status(err);
                        return;
                    }
                    QStringList paths = selected.isEmpty() ? QStringList{path} : selected;
                    if (!playlists_.create(name, paths, libraryRoot_)) {
                        status(playlists_.error);
                        return;
                    }
                    refreshSourceCombo();
                    status(tr("Created \"%1\" with %2 songs").arg(name).arg(paths.size()));
                });
                menu.addAction(tr("Copy path"), [path] {
                    QApplication::clipboard()->setText(path);
                });
                menu.addSeparator();
                menu.addAction(tr("Show in folder"), [this, path] {
                    revealFile(path, this, [this](bool, bool, const QString &message) { status(message); });
                });
                menu.exec(globalPos);
            });
    connect(queueModel_, &QueueModel::orderEdited, this, &MainWindow::applyOrderEdited);
}

void MainWindow::buildPlayerPage()
{
    playerPage_ = new QWidget(pages_);
    auto *outer = new QVBoxLayout(playerPage_);
    outer->setContentsMargins(10, 4, 10, 4);

    splitter_ = new QSplitter(Qt::Horizontal, playerPage_);
    outer->addWidget(splitter_, 1);

    auto *left = new QWidget(splitter_);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(5);
    auto *actions = new QHBoxLayout;
    addToPlaylistBtn_ = new QPushButton(tr("Add to playlist\u2026"), left);
    removeFromPlaylistBtn_ = new QPushButton(tr("Remove from playlist"), left);
    actions->addWidget(addToPlaylistBtn_);
    actions->addWidget(removeFromPlaylistBtn_);
    actions->addStretch();
    leftLayout->addLayout(actions);

    queueModel_ = new QueueModel(this);
    queueView_ = new QueueTableView(left);
    queueView_->setModel(queueModel_);
    queueView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    queueView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    queueView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queueView_->setShowGrid(false);
    queueView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    queueView_->setWordWrap(false);
    queueView_->verticalHeader()->setVisible(false);
    auto *header = queueView_->horizontalHeader();
    queueView_->resetColumnWidths();
    for (int c = QueueModel::Folder; c < QueueModel::COLUMN_COUNT; ++c)
        queueView_->setColumnHidden(c, settings_.hiddenQueueColumns.contains(QueueModel::columnKey(c)));

    auto *columnsMenu = new QMenu(tr("Columns"), queueView_);
    columnsMenu->setObjectName(QStringLiteral("queueColumnsMenu"));
    columnsMenu->setToolTipsVisible(true);
    // Module is the queue's identity column and is always visible. Do not
    // include it in this menu: an unavailable option is clearer when omitted
    // than when it looks like a disabled toggle.
    for (int c = QueueModel::Folder; c < QueueModel::COLUMN_COUNT; ++c) {
        auto *action = columnsMenu->addAction(QueueModel::columnName(c));
        action->setObjectName(QStringLiteral("queueColumn_") + QueueModel::columnKey(c));
        action->setCheckable(true);
        action->setChecked(!queueView_->isColumnHidden(c));
        connect(action, &QAction::toggled, this, [this, action, c](bool shown) {
            const QStringList previous = settings_.hiddenQueueColumns;
            const QString key = QueueModel::columnKey(c);
            settings_.hiddenQueueColumns.removeAll(key);
            if (!shown) settings_.hiddenQueueColumns << key;
            if (!saveSettings(settings_)) {
                settings_.hiddenQueueColumns = previous;
                const QSignalBlocker blocker(action);
                action->setChecked(!queueView_->isColumnHidden(c));
                QMessageBox::warning(this, tr("Columns"), tr("Could not save column preferences. Check the config folder and qt-ui.json."));
                return;
            }
            queueView_->setColumnHidden(c, !shown);
            queueView_->ensureHeaderWidths();
        });
    }
    auto *columnsButton = new QToolButton(left);
    columnsButton->setText(tr("Columns"));
    columnsButton->setObjectName(QStringLiteral("queueColumnsButton"));
    columnsButton->setToolTip(tr("Choose the columns shown in the queue."));
    columnsButton->setPopupMode(QToolButton::InstantPopup);
    columnsButton->setMenu(columnsMenu);
    actions->addWidget(columnsButton);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this, [header, columnsMenu](const QPoint &pos) {
        columnsMenu->popup(header->mapToGlobal(pos));
    });
    header->setHighlightSections(false);
    leftLayout->addWidget(queueView_, 1);
    splitter_->addWidget(left);

    auto *rightPanel = new QWidget(splitter_);
    rightPanel->setObjectName(QStringLiteral("infoPanel"));
    auto *panelLayout = new QVBoxLayout(rightPanel);
    panelLayout->setContentsMargins(12, 12, 12, 10);
    buildInfoPanel(rightPanel);
    splitter_->addWidget(rightPanel);
    splitter_->setChildrenCollapsible(false);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({600, 400});

    connect(addToPlaylistBtn_, &QPushButton::clicked, this, &MainWindow::addToPlaylistMenu);
    connect(removeFromPlaylistBtn_, &QPushButton::clicked, this,
            &MainWindow::removeFromPlaylist);
}

void MainWindow::buildInfoPanel(QWidget *parent)
{
    auto *layout = qobject_cast<QVBoxLayout *>(parent->layout());

    titleLabel_ = new QLabel(tr("Nothing playing"), parent);
    titleLabel_->setObjectName(QStringLiteral("bigTitle"));
    titleLabel_->setTextFormat(Qt::PlainText);
    titleLabel_->setWordWrap(true);
    layout->addWidget(titleLabel_);
    subtitleLabel_ = new QLabel(tr("Double-click a module to play it"), parent);
    subtitleLabel_->setObjectName(QStringLiteral("dimLabel"));
    subtitleLabel_->setTextFormat(Qt::PlainText);
    subtitleLabel_->setWordWrap(true);
    layout->addWidget(subtitleLabel_);

    auto *actions = new QHBoxLayout;
    favoriteButton_ = new QPushButton(QStringLiteral("\u2606"), parent);   // ☆
    favoriteButton_->setCheckable(true);
    favoriteButton_->setToolTip(tr("Toggle favorite"));
    favoriteButton_->setEnabled(false);
    ignoreButton_ = new QPushButton(tr("Ignore"), parent);
    ignoreButton_->setEnabled(false);
    songInfoButton_ = new QPushButton(tr("Song info"), parent);
    songInfoButton_->setEnabled(false);
    revealButton_ = new QPushButton(tr("Show in folder"), parent);
    revealButton_->setEnabled(false);
    for (QPushButton *b : {favoriteButton_, ignoreButton_, songInfoButton_, revealButton_})
        actions->addWidget(b);
    actions->addStretch();
    layout->addLayout(actions);
    connect(favoriteButton_, &QPushButton::clicked, this, &MainWindow::addCurrentFavorite);
    connect(ignoreButton_, &QPushButton::clicked, this, &MainWindow::ignoreCurrent);
    connect(songInfoButton_, &QPushButton::clicked, this, &MainWindow::openSongInfoDialog);
    connect(revealButton_, &QPushButton::clicked, this, &MainWindow::revealCurrent);

    auto *grid = new QGridLayout;
    grid->setVerticalSpacing(1);
    const QList<QPair<QString, QString>> rows = {
        {QStringLiteral("format"), tr("Format")},      {QStringLiteral("tracker"), tr("Tracker")},
        {QStringLiteral("artist"), tr("Artist")},       {QStringLiteral("size"), tr("Module")},
        {QStringLiteral("length"), tr("Length")},       {QStringLiteral("position"), tr("Position")},
        {QStringLiteral("sequencer"), tr("Sequencer")}, {QStringLiteral("resample"), tr("Resampling")},
        {QStringLiteral("output"), tr("Output")},
    };
    int r = 0;
    for (const auto &pair : rows) {
        auto *name = new QLabel(pair.second, parent);
        name->setObjectName(QStringLiteral("dimLabel"));
        grid->addWidget(name, r, 0, Qt::AlignLeft);
        auto *value = new QLabel(QStringLiteral("-"), parent);
        value->setTextFormat(Qt::PlainText);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long artist is cut, not widening the panel
        grid->addWidget(value, r, 1, Qt::AlignLeft);
        infoLabels_.insert(pair.first, value);
        ++r;
    }
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    auto *channelsLabel = new QLabel(tr("Channels"), parent);
    channelsLabel->setObjectName(QStringLiteral("dimLabel"));
    layout->addWidget(channelsLabel);
    vu_ = new VuMeter(parent);
    layout->addWidget(vu_);

    auto *subs = new QHBoxLayout;
    auto *subLabel = new QLabel(tr("Subsong"), parent);
    subLabel->setObjectName(QStringLiteral("dimLabel"));
    subsongSpin_ = new QSpinBox(parent);
    subsongSpin_->setObjectName(QStringLiteral("subsongSpin"));
    subsongSpin_->setToolTip(tr("Choose a subsong (1 is the first). This restarts it and turns off Play all subsongs; pause is preserved."));
    subsongSpin_->setRange(1, 1);
    subsongSpin_->setKeyboardTracking(false);
    subsongSpin_->setEnabled(false);
    subsongCountLabel_ = new QLabel(tr("(single song)"), parent);
    subsongCountLabel_->setObjectName(QStringLiteral("dimLabel"));
    loopLabel_ = new QLabel(parent);
    loopLabel_->setObjectName(QStringLiteral("dimLabel"));
    subs->addWidget(subLabel);
    subs->addWidget(subsongSpin_);
    subs->addWidget(subsongCountLabel_);
    allSubsongsCheck_ = new QCheckBox(tr("Play all subsongs"), parent);
    allSubsongsCheck_->setObjectName(QStringLiteral("allSubsongsCheck"));
    allSubsongsCheck_->setChecked(settings_.playAllSubsongs);
    allSubsongsCheck_->setToolTip(tr("Play every subsong in order, then advance to the next file if auto-advance is enabled. Enabling starts at the first subsong, preserving pause. Loop repeats the whole sequence. Time and seeking refer to the current subsong."));
    subs->addWidget(allSubsongsCheck_);
    subs->addStretch();
    subs->addWidget(loopLabel_);
    layout->addLayout(subs);
    subsongNameLabel_ = new QLabel(parent);
    subsongNameLabel_->setTextFormat(Qt::PlainText);
    subsongNameLabel_->setWordWrap(true);
    subsongNameLabel_->hide();
    layout->addWidget(subsongNameLabel_);
    connect(allSubsongsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.playAllSubsongs = on;
        engine_.setPlayAllSubsongs(on);
        commitSettings();
    });
    connect(subsongSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        allSubsongsCheck_->setChecked(false);
        engine_.setSubsong(value - 1);
        ++songToken_;
        songTokenKey_.clear();
        tracker_->clearSong();
    });

    auto *logLabel = new QLabel(tr("Log"), parent);
    logLabel->setObjectName(QStringLiteral("dimLabel"));
    layout->addWidget(logLabel);
    logView_ = new QPlainTextEdit(parent);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(200);
    logView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(logView_, 1);
}

void MainWindow::buildTrackerPage()
{
    trackerPage_ = new QWidget(pages_);
    auto *layout = new QVBoxLayout(trackerPage_);
    layout->setContentsMargins(10, 4, 10, 6);
    auto *header = new QHBoxLayout;
    trackerHeader_ = new QLabel(tr("no module"), trackerPage_);
    trackerHeader_->setObjectName(QStringLiteral("headLabel"));
    followingButton_ = new StableLabelButton({tr("Follow"), tr("Following")}, trackerPage_);
    followingButton_->setText(tr("Following"));
    followingButton_->setCheckable(true);
    followingButton_->setChecked(true);
    followingButton_->setObjectName(QStringLiteral("miniButton"));
    auto *prevPat = new QPushButton(QStringLiteral("\u25C0"), trackerPage_);
    auto *nextPat = new QPushButton(QStringLiteral("\u25B6"), trackerPage_);
    for (QPushButton *b : {prevPat, nextPat})
        b->setObjectName(QStringLiteral("miniButton"));
    header->addWidget(trackerHeader_, 1);
    header->addWidget(prevPat);
    header->addWidget(nextPat);
    header->addWidget(followingButton_);
    layout->addLayout(header);

    tracker_ = new TrackerView(trackerPage_);
    tracker_->setSmoothScrolling(settings_.smoothTrackerScrolling);
    layout->addWidget(tracker_, 1);

    connect(followingButton_, &QPushButton::toggled, this, [this](bool on) {
        tracker_->setFollowing(on);
    });
    connect(tracker_, &TrackerView::followingChanged, this, [this](bool following) {
        const QSignalBlocker blocker(followingButton_);
        followingButton_->setChecked(following);
        followingButton_->setText(following ? tr("Following") : tr("Follow"));
    });
    connect(prevPat, &QPushButton::clicked, this, [this] {
        tracker_->jumpPattern(TrackerView::PrevPattern);
    });
    connect(nextPat, &QPushButton::clicked, this, [this] {
        tracker_->jumpPattern(TrackerView::NextPattern);
    });
    connect(tracker_, &TrackerView::rowClicked, this, [this](int order, int row) {
        engine_.seekOrderRow(order, row);
        const EngineSnapshot snap = engine_.snapshot();
        if (!snap.playing && !snap.paused)
            engine_.play();
        else
            engine_.play();
    });
}

void MainWindow::buildTransport()
{
    transportBar_ = new QWidget();
    transportBar_->setObjectName(QStringLiteral("barPanel"));
    auto *grid = new QGridLayout(transportBar_);
    grid->setContentsMargins(10, 6, 10, 8);
    grid->setHorizontalSpacing(14);

    auto *buttons = new QWidget(transportBar_);
    buttons->setObjectName(QStringLiteral("transportControls"));
    auto *bl = new QHBoxLayout(buttons);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(3);
    prevButton_ = new QPushButton(QStringLiteral("\u25C0\u25C0"), buttons);
    playButton_ = new QPushButton(QStringLiteral("\u25B6"), buttons);
    nextButton_ = new QPushButton(QStringLiteral("\u25B6\u25B6"), buttons);
    stopButton_ = new QPushButton(QStringLiteral("\u25A0"), buttons);
    for (QPushButton *b : {prevButton_, playButton_, nextButton_, stopButton_}) {
        b->setObjectName(QStringLiteral("transportButton"));
        b->setMinimumWidth(44);
        bl->addWidget(b);
    }
    loopCheck_ = new QCheckBox(tr("Loop"), buttons);
    loopCheck_->setChecked(settings_.loopTrack);
    repeatCheck_ = new QCheckBox(tr("Repeat queue"), buttons);
    repeatCheck_->setToolTip(tr("Repeat queue, draw new order in shuffle (R)"));
    repeatCheck_->setChecked(settings_.loopQueue);
    bl->addSpacing(8);
    bl->addWidget(loopCheck_);
    bl->addWidget(repeatCheck_);

    auto *volume = new QWidget(transportBar_);
    volume->setObjectName(QStringLiteral("volumeGroup"));
    auto *vl = new QHBoxLayout(volume);
    vl->setContentsMargins(0, 0, 0, 0);
    muteButton_ = new StableLabelButton({tr("Mute"), tr("Unmute")}, volume);
    muteButton_->setText(settings_.muted ? tr("Unmute") : tr("Mute"));
    muteButton_->setObjectName(QStringLiteral("muteButton"));
    muteButton_->setCheckable(true);
    muteButton_->setChecked(settings_.muted);
    volumeSlider_ = new JumpSlider(Qt::Horizontal, volume);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setPageStep(5);
    volumeSlider_->setValue(std::clamp(settings_.volume, 0, 100));
    volumeSlider_->setFixedWidth(120);
    volumeLabel_ = new QLabel(QStringLiteral("%1%").arg(settings_.volume), volume);
    volumeLabel_->setObjectName(QStringLiteral("dimLabel"));
    volumeLabel_->setMinimumWidth(38);
    vl->addWidget(muteButton_);
    vl->addWidget(volumeSlider_);
    vl->addWidget(volumeLabel_);

    auto *pos = new QWidget(transportBar_);
    pos->setObjectName(QStringLiteral("posGroup"));
    auto *pl = new QHBoxLayout(pos);
    pl->setContentsMargins(0, 0, 0, 0);
    timeLabel_ = new QLabel(QStringLiteral("0:00"), pos);
    timeLabel_->setObjectName(QStringLiteral("timeLabel"));
    durationLabel_ = new QLabel(QStringLiteral("0:00"), pos);
    durationLabel_->setObjectName(QStringLiteral("timeLabel"));
    queuePosLabel_ = new QLabel(QStringLiteral("0/0"), pos);
    queuePosLabel_->setObjectName(QStringLiteral("dimLabel"));
    seekSlider_ = new JumpSlider(Qt::Horizontal, pos);
    seekSlider_->setRange(0, 1000);
    seekSlider_->setMinimumWidth(90);
    pl->addWidget(timeLabel_);
    pl->addWidget(seekSlider_, 1);
    pl->addWidget(durationLabel_);
    pl->addWidget(queuePosLabel_);

    grid->addWidget(buttons, 0, 0);
    grid->addWidget(volume, 0, 1);
    grid->addWidget(pos, 0, 2);
    grid->setColumnStretch(2, 1);

    setContextMenuPolicy(Qt::PreventContextMenu);
    // (transportBar_ is docked together with the status row in buildStatusBar)

    connect(prevButton_, &QPushButton::clicked, this, &MainWindow::playPrevious);
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::togglePlay);
    connect(nextButton_, &QPushButton::clicked, this, &MainWindow::playNext);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    connect(loopCheck_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.loopTrack = on;
        engine_.setLoopTrack(on);
        commitSettings();
    });
    connect(repeatCheck_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.loopQueue = on;
        commitSettings();
    });
    connect(muteButton_, &QPushButton::toggled, this, [this](bool on) {
        settings_.muted = on;
        muteButton_->setText(on ? tr("Unmute") : tr("Mute"));
        engine_.setMuted(on);
        commitSettings();
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        settings_.volume = value;
        engine_.setVolume(value);
        volumeLabel_->setText(QStringLiteral("%1%").arg(value));
    });
    connect(volumeSlider_, &QSlider::sliderReleased, this, &MainWindow::commitSettings);
    connect(seekSlider_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seekSlider_, &QSlider::sliderMoved, this, [this](int value) {
        const EngineSnapshot snap = engine_.snapshot();
        if (snap.durationValid)
            timeLabel_->setText(formatTime(snap.duration * value / 1000.0));
    });
    connect(seekSlider_, &JumpSlider::positionRequested, this, [this](int value) {
        engine_.seekFraction(value / 1000.0);
        const EngineSnapshot snap = engine_.snapshot();
        if (snap.durationValid)
            timeLabel_->setText(formatTime(snap.duration * value / 1000.0));
    });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
    });
    seekSlider_->installEventFilter(this);   // right click -> jump to start
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
    statusLabel_ = new QLabel(tr("Ready"), bar);
    statusLabel_->setObjectName(QStringLiteral("dimLabel"));
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long messages are cut
    healthLabel_ = new QLabel(bar);
    healthLabel_->setObjectName(QStringLiteral("dimLabel"));
    resetButton_ = new QPushButton(tr("Reset layout"), bar);
    resetButton_->setObjectName(QStringLiteral("miniButton"));
    row->addWidget(statusLabel_, 1);
    row->addWidget(healthLabel_);
    row->addWidget(resetButton_);
    connect(resetButton_, &QPushButton::clicked, this, &MainWindow::resetLayout);
    filterBtn_->setToolTip(tr("Filter formats, length, broken files (Ctrl+Shift+F)"));
    playlistsBtn_->setToolTip(tr("Save the queue as a playlist (Ctrl+P)"));
    healthLabel_->setToolTip(tr("Layout reset: Ctrl+0"));

    auto *bottom = new QWidget(centralWidget());
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    bl->addWidget(bar);
    bl->addWidget(transportBar_);
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
    key(Qt::Key_Return, [this] { playSelected(); });
    key(Qt::Key_PageUp, [this] { playPrevious(); });
    key(Qt::Key_PageDown, [this] { playNext(); });
    key(Qt::Key_Left, [this] { engine_.seekRelative(-5); });
    key(Qt::Key_Right, [this] { engine_.seekRelative(5); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { engine_.seekRelative(-30); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { engine_.seekRelative(30); });
    key(Qt::Key_L, [this] { loopCheck_->toggle(); });
    key(Qt::Key_R, [this] { repeatCheck_->toggle(); });
    key(Qt::Key_M, [this] { muteButton_->toggle(); });
    key(Qt::Key_Plus, [this] { volumeSlider_->setValue(volumeSlider_->value() + 5); });
    key(Qt::Key_Equal, [this] { volumeSlider_->setValue(volumeSlider_->value() + 5); });
    key(Qt::Key_Minus, [this] { volumeSlider_->setValue(volumeSlider_->value() - 5); });
    key(Qt::Key_0, [this] { volumeSlider_->setValue(0); });
    key(Qt::Key_F1, [this] { openAboutDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_O), [this] { openButton_->click(); });
    key(Qt::Key_F5, [this] { rescan(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_F), [this] { searchEdit_->setFocus(); });
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
    key(Qt::Key_Escape, [this] {
        if (!searchEdit_->text().isEmpty())
            searchEdit_->clear();
    });
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
    libraryRoot_ = QFileInfo(path).absoluteFilePath();
    dirField_->setText(libraryRoot_);
    dirField_->setToolTip(libraryRoot_);
    settings_.lastDirectory = libraryRoot_;
    settings_.lastPickerDir = libraryRoot_;
    commitSettings();
    rescan();
}

void MainWindow::rescan()
{
    if (libraryRoot_.isEmpty()) {
        status(tr("Choose a folder first (Open folder...)"));
        return;
    }
    if (analyzer_.isRunning()) {
        analysisRestartPending_ = true;
        analyzer_.cancel();
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    status(tr("Scanning %1...").arg(libraryRoot_));
    OpenMPTLib *lib = OpenMPTLib::instance();
    ScanResult result = scanLibrary(libraryRoot_, scanExtensions(lib));
    libraryTracks_ = std::move(result.tracks);
    libraryDirs_ = result.dirs;
    trackByPath_.clear();
    for (const Track &track : libraryTracks_)
        trackByPath_.insert(track.path, track);
    // apply the cache once
    {
        AnalysisCache cache;
        if (cache.load() >= 0) {
            for (auto it = trackByPath_.begin(); it != trackByPath_.end(); ++it) {
                const QFileInfo info(it.key());
                CachedModule entry;
                if (cache.apply(QFileInfo::exists(it.key())
                                    ? info.absoluteFilePath()
                                    : it.key(),
                                info.size(), fileMTime(it.key()),
                                &entry)) {
                    Track &track = it.value();
                    track.analyzed = true;
                    track.duration = entry.duration;
                    track.fmt = entry.fmt;
                    track.channels = entry.channels;
                    track.subsongs = entry.subsongs;
                    track.title = entry.title;
                    track.broken = entry.broken;
                }
            }
        }
    }
    for (Track &track : libraryTracks_)
        track = trackByPath_.value(track.path, track);
    QApplication::restoreOverrideCursor();
    countLabel_->setText(QStringLiteral("%1 tracks, %2 folders")
                            .arg(libraryTracks_.size()).arg(libraryDirs_));
    if (!result.errors.isEmpty()) {
        status(tr("Scan finished with %1 problems").arg(result.errors.size()));
        for (int i = 0; i < std::min(20, int(result.errors.size())); ++i)   // which ones (log)
            appendLog(QStringLiteral("warn"), result.errors.at(i));
    }
    else
        status(tr("Scanned %1 tracks in %2").arg(libraryTracks_.size()).arg(libraryRoot_));
    rebuildQueue();
    if (!libraryTracks_.isEmpty()) {
        bool anyUnanalyzed = false;
        for (const Track &track : libraryTracks_)
            anyUnanalyzed |= !track.analyzed;
        if (anyUnanalyzed && settings_.autoAnalyze && !analyzer_.isRunning())
            analyzeNow();
    }
}

void MainWindow::analyzeNow()
{
    if (analyzer_.isRunning()) {
        analyzer_.cancel();
        status(tr("Canceling analysis..."));
        return;
    }
    QStringList pending;
    for (const Track &track : libraryTracks_) {
        if (!track.analyzed)
            pending << track.path;
    }
    if (pending.isEmpty()) {
        status(tr("Everything is already analyzed"));
        return;
    }
    analyzeButton_->setText(tr("Analyzing 0/%1").arg(pending.size()));
    analyzer_.start(pending, settings_.cacheAnalysis);
    status(tr("Analyzing %1 files in the background").arg(pending.size()));
}

QueueFilter MainWindow::filterFromSettings() const
{
    QueueFilter filter;
    filter.formats = settings_.filterFormats;
    filter.minSeconds = settings_.filterMin;
    filter.maxSeconds = settings_.filterMax;
    filter.hideBroken = settings_.filterHideBroken;
    return filter;
}

void MainWindow::syncFilterFromSettings()
{
    rebuildQueue();
}

QStringList MainWindow::queueSelectedPaths() const
{
    QStringList paths;
    const QModelIndexList rows = queueView_->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const QString path = queueModel_->pathAt(index);
        if (!path.isEmpty())
            paths << path;
    }
    paths.removeDuplicates();
    return paths;
}

void MainWindow::rebuildQueue()
{
    const bool isPlaylistSource = !settings_.queueSource.isEmpty();
    const Playlist *playlist = isPlaylistSource ? playlists_.get(settings_.queueSource) : nullptr;

    QVector<Track> base = sourceTracks();
    OrderMode mode = orderModeFromName(settings_.queueMode);
    const bool savedMode = mode == OrderMode::Saved && playlist;
    QVector<Track> ordered;
    if (mode == OrderMode::Shuffle) {
        // restore the drawn order of this source; a plan is only freshly drawn
        // for a source that never had one
        ensureShufflePlan(base);
        mergeNewIntoShufflePlan(base);
        ordered = applyPathOrder(base, shufflePaths_);
    } else if (savedMode) {
        ordered = applyPathOrder(base, playlist->paths);
    } else if (mode == OrderMode::Saved) {
        ordered = base;          // "playlist" order without a playlist: leave as scanned
    } else {
        ordered = orderTracks(base, mode, quint32(settings_.shuffleSeed));
    }

    orderedSource_ = std::move(ordered);
    applyQueueFilters();
}

void MainWindow::applyQueueFilters()
{
    const bool savedMode = orderModeFromName(settings_.queueMode) == OrderMode::Saved
                          && !settings_.queueSource.isEmpty() && playlists_.get(settings_.queueSource);
    const OrderMode mode = orderModeFromName(settings_.queueMode);
    QVector<Track> ordered = orderedSource_;
    const QString needle = searchEdit_ ? searchEdit_->text() : QString();
    if (!needle.trimmed().isEmpty())
        ordered = searchFilter(ordered, needle);
    QueueFilter filter = filterFromSettings();
    queueTracks_ = filter.select(ordered);

    queueModel_->setRows(queueTracks_, mode == OrderMode::ByDirectory);
    queueIndex_.clear();
    queueIndex_.reserve(queueTracks_.size());
    for (int i = 0; i < queueTracks_.size(); ++i)
        queueIndex_.insert(queueTracks_[i].path, i);
    if (queueIndex_.contains(currentPath()))
        playingQueueIndex_ = queueIndex_.value(currentPath());
    QSet<QString> missing;
    for (const Track &track : queueTracks_) {
        if (!QFile::exists(track.path))
            missing.insert(track.path);
    }
    queueModel_->setMissingPaths(missing);
    queueModel_->setPlayingPath(currentPath());
    queueView_->setReorderEnabled(savedMode);
    filterLabel_->setText(filter.active() ? filter.describe() : QString());
    filterLabel_->setToolTip(filterLabel_->text());

    refreshQueueControls();
    const QStringList context{libraryRoot_, settings_.queueSource, settings_.queueMode};
    if (context != queueViewContext_) {
        queueViewContext_ = context;
        revealPlayingInQueue();
    }
}

void MainWindow::revealPlayingInQueue()
{
    const auto index = queueModel_->indexOfPath(currentPath());
    if (index.isValid()) {
        queueView_->setCurrentIndex(index);
        queueView_->scrollTo(index, QAbstractItemView::PositionAtTop);
    }
}

void MainWindow::refreshQueueControls()
{
    const bool isPlaylistSource = !settings_.queueSource.isEmpty();
    // Rebuild the order combo: "Saved order" only for playlists.
    const QString wanted = (isPlaylistSource || settings_.queueMode != QLatin1String("playlist"))
                               ? settings_.queueMode
                               : QStringLiteral("by directory");
    const QSignalBlocker blocker(orderCombo_);
    orderCombo_->clear();
    orderCombo_->addItem(tr("Alphabetical"), QStringLiteral("alphabetical"));
    orderCombo_->addItem(tr("By directory"), QStringLiteral("by directory"));
    orderCombo_->addItem(tr("Shuffle"), QStringLiteral("shuffle"));
    if (isPlaylistSource)
        orderCombo_->addItem(tr("Saved order"), QStringLiteral("playlist"));
    for (int i = 0; i < orderCombo_->count(); ++i)
        if (orderCombo_->itemData(i).toString() == wanted)
            orderCombo_->setCurrentIndex(i);
    if (removeFromPlaylistBtn_)
        removeFromPlaylistBtn_->setEnabled(isPlaylistSource);
    shuffleBtn_->setEnabled(!queueTracks_.isEmpty());
}

void MainWindow::refreshSourceCombo()
{
    const QSignalBlocker blocker(sourceCombo_);
    sourceCombo_->clear();
    sourceCombo_->addItem(tr("Library"), QString());
    for (const QString &name : playlists_.names()) {
        if (PlaylistStore::isFavorites(name))
            sourceCombo_->addItem(tr("Favorites (%1)").arg(playlists_.get(name)->paths.size()),
                                  name);
        else
            sourceCombo_->addItem(name, name);
    }
    for (int i = 0; i < sourceCombo_->count(); ++i)
        if (sourceCombo_->itemData(i).toString() == settings_.queueSource)
            sourceCombo_->setCurrentIndex(i);
}

void MainWindow::reshuffle()
{
    // the "Shuffle now" button: draw a brand new order, current track first
    settings_.queueMode = QStringLiteral("shuffle");
    drawShufflePlan(freshShuffleSeed(settings_.shuffleSeed), currentPath().isEmpty() ? selectedPath() : currentPath(), QString());
    rebuildQueue();
    revealPlayingInQueue();
    QString tail;
    if (!queueTracks_.isEmpty())
        tail = tr(" - starting from %1").arg(queueTracks_.first().displayTitle());
    status(tr("New shuffle order drawn (Ctrl+S)%1").arg(tail));
}

// ---------------------------------------------------------------------------
// shuffle plans (port of ui.py _ensure/_draw/_remember/_migrate_shuffle_*)
// ---------------------------------------------------------------------------
QVector<Track> MainWindow::sourceTracks() const
{
    QVector<Track> base;
    const Playlist *playlist = settings_.queueSource.isEmpty()
        ? nullptr
        : playlists_.get(settings_.queueSource);
    if (playlist) {
        for (const QString &path : playlist->paths) {
            if (ignored_.contains(path))
                continue;
            auto it = trackByPath_.constFind(path);
            if (it != trackByPath_.constEnd()) {
                base.append(*it);
                continue;
            }
            Track track;                                // external playlist entry
            track.path = path;
            const QFileInfo info(path);
            track.name = info.fileName();
            track.relDir =
                info.absolutePath() == libraryRoot_ ? QString() : info.absolutePath();
            track.ext = info.suffix().toLower();
            track.fmt = track.ext;
            track.size = info.size();
            if (!info.exists())
                track.broken = QStringLiteral("missing");
            base.append(track);
        }
        return base;
    }
    base.reserve(libraryTracks_.size());
    for (const Track &track : libraryTracks_) {
        if (!ignored_.contains(track.path))
            base.append(track);
    }
    return base;
}

QString MainWindow::shuffleSourceKey() const
{
    return settings_.queueSource.isEmpty() ? ShuffleStore::libraryKey(libraryRoot_)
                                           : ShuffleStore::playlistKey(settings_.queueSource);
}

void MainWindow::drawShufflePlan(quint32 newSeed, const QString &firstPath,
                                 const QString &avoidFirst)
{
    quint32 seed = newSeed;
    if (!seed) {
        seed = quint32(settings_.shuffleSeed)
            ? quint32(settings_.shuffleSeed)
            : quint32(QRandomGenerator::global()->bounded(1, int(2e9)));
    }
    settings_.shuffleSeed = seed;
    QString anchor = firstPath;
    if (anchor.isEmpty() && avoidFirst.isEmpty())
        anchor = currentPath().isEmpty() ? selectedPath() : currentPath();
    const QVector<Track> plan = orderTracks(sourceTracks(), OrderMode::Shuffle, seed, anchor,
                                            avoidFirst);
    shuffleKey_ = shuffleSourceKey();
    shufflePaths_.clear();
    for (const Track &track : plan)
        shufflePaths_ << track.path;
    if (!shuffles_.put(shuffleKey_, shufflePaths_) && !shuffles_.error.isEmpty())
        appendLog(QStringLiteral("warn"), shuffles_.error);
    commitSettings();          // the seed travels with the plan
}

void MainWindow::ensureShufflePlan(const QVector<Track> &source)
{
    const QString key = shuffleSourceKey();
    if (shuffleKey_ == key && !shufflePaths_.isEmpty())
        return;
    shuffleKey_ = key;
    shufflePaths_ = shuffles_.get(key);
    if (shufflePaths_.isEmpty() && !source.isEmpty())
        drawShufflePlan();     // never drawn for this source: draw (and store) one
}

void MainWindow::mergeNewIntoShufflePlan(const QVector<Track> &source)
{
    // Songs added to the source after the order was drawn (a rescan, new
    // favorites) would otherwise all be appended in alphabetical order.
    if (shufflePaths_.isEmpty())
        return;
    const QSet<QString> planned(shufflePaths_.cbegin(), shufflePaths_.cend());
    QStringList fresh;
    for (const Track &track : source) {
        if (!planned.contains(track.path))
            fresh << track.path;
    }
    if (fresh.isEmpty())
        return;
    if (fresh.size() > shufflePaths_.size()) {
        drawShufflePlan();   // mostly new: a fresh order fits better
        return;
    }
    // each new song gets a random slot; one merge pass keeps it O(n)
    auto *rng = QRandomGenerator::global();
    QVector<QPair<int, QString>> places;
    places.reserve(fresh.size());
    for (const QString &path : std::as_const(fresh))
        places.append({int(rng->bounded(shufflePaths_.size() + 1)), path});
    std::sort(places.begin(), places.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QStringList merged;
    merged.reserve(shufflePaths_.size() + places.size());
    int s = 0;
    for (int i = 0; i <= shufflePaths_.size(); ++i) {
        while (s < places.size() && places[s].first == i)
            merged << places[s++].second;
        if (i < shufflePaths_.size())
            merged << shufflePaths_[i];
    }
    shufflePaths_ = merged;
    if (!shuffles_.put(shuffleKey_, shufflePaths_) && !shuffles_.error.isEmpty())
        appendLog(QStringLiteral("warn"), shuffles_.error);
}

void MainWindow::migrateShuffleOrder()
{
    const QStringList legacy = settings_.shufflePaths;
    if (legacy.isEmpty())
        return;
    const QString key =
        settings_.queueSource.isEmpty() ? ShuffleStore::libraryKey(settings_.lastDirectory)
                                        : ShuffleStore::playlistKey(settings_.queueSource);
    if (!shuffles_.contains(key) && !shuffles_.put(key, legacy))
        return;   // keep the old copy until it can be saved
    settings_.shufflePaths.clear();
    commitSettings();
}

void MainWindow::onPlaylistRenamed(const QString &oldName, const QString &newName)
{
    shuffles_.rename(ShuffleStore::playlistKey(oldName), ShuffleStore::playlistKey(newName));
    if (caseFold(settings_.queueSource) == caseFold(oldName)) {
        settings_.queueSource = newName;
        settings_.activePlaylist = newName;
        commitSettings();
        shuffleKey_.clear();                 // reload the order under the new name
    }
    refreshSourceCombo();
    rebuildQueue();
}

void MainWindow::onPlaylistRemoved(const QString &name)
{
    shuffles_.remove(ShuffleStore::playlistKey(name));
    if (caseFold(settings_.queueSource) == caseFold(name)) {
        settings_.queueSource.clear();
        settings_.activePlaylist.clear();
        if (settings_.queueMode == QLatin1String("playlist"))
            settings_.queueMode = QStringLiteral("by directory");
        commitSettings();
        status(tr("The selected playlist is gone - Library selected (Ctrl+P)"));
    }
    refreshSourceCombo();
    rebuildQueue();
}

void MainWindow::applyOrderEdited(const QStringList &newOrder)
{
    if (settings_.queueSource.isEmpty() || orderModeFromName(settings_.queueMode) != OrderMode::Saved)
        return;
    const Playlist *playlist = playlists_.get(settings_.queueSource);
    if (!playlist)
        return;
    // Only the visible entries were reordered: they fill their own slots in
    // the saved list, and entries hidden by search/filter/ignore stay where
    // they are (they used to move to the end).
    QSet<QString> queued;
    for (const Track &track : queueTracks_)
        queued.insert(track.path);
    QStringList result;
    result.reserve(playlist->paths.size());
    int next = 0;
    for (const QString &path : playlist->paths) {
        if (queued.contains(path) && next < newOrder.size())
            result << newOrder[next++];
        else
            result << path;
    }
    while (next < newOrder.size())
        result << newOrder[next++];
    playlists_.replace(settings_.queueSource, result);
    rebuildQueue();
    status(tr("Saved the new order to \"%1\"").arg(settings_.queueSource));
}

void MainWindow::moveSelectedRow(int delta)
{
    if (settings_.queueSource.isEmpty() || orderModeFromName(settings_.queueMode) != OrderMode::Saved)
        return;
    const QModelIndex current = queueView_->currentIndex();
    const QString path = queueModel_->pathAt(current);
    if (path.isEmpty())
        return;
    QStringList order;
    for (const Track &track : queueTracks_)
        order << track.path;
    const int from = order.indexOf(path);
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    applyOrderEdited(order);
    queueView_->setCurrentIndex(queueModel_->indexOfPath(path));
}

// ============================================================================
// playback
// ============================================================================
QString MainWindow::currentPath() const
{
    const EngineSnapshot snap = engine_.snapshot();
    return !snap.path.isEmpty() ? snap.path : QString();
}

QString MainWindow::selectedPath() const
{
    const QModelIndex index = queueView_->currentIndex().isValid()
        ? queueView_->currentIndex()
        : queueView_->selectionModel()->currentIndex();
    const QString path = queueModel_->pathAt(index);
    if (!path.isEmpty())
        return path;
    const QStringList selected = queueSelectedPaths();
    return selected.isEmpty() ? QString() : selected.first();
}

int MainWindow::queueIndexOf(const QString &path) const
{
    // all queued tracks, also those inside a collapsed folder (the model's
    // row lookup only knows visible rows)
    return queueIndex_.value(path, -1);
}

int MainWindow::nextQueueIndexAfter(const QString &path) const
{
    // The song isn't in the visible queue (search, filter, unfavorited,
    // removed): continue after its place in the unfiltered order, or after
    // its last known queue position.
    const int here = queueIndexOf(path);
    if (here >= 0)
        return here + 1;
    for (int k = 0; k < orderedSource_.size(); ++k) {
        if (orderedSource_[k].path != path)
            continue;
        for (int j = k + 1; j < orderedSource_.size(); ++j) {
            const int q = queueIndexOf(orderedSource_[j].path);
            if (q >= 0)
                return q;
        }
        return queueTracks_.size();   // nothing visible after it: end of the queue
    }
    if (playingQueueIndex_ >= 0)
        return std::min(playingQueueIndex_, int(queueTracks_.size()));   // its successor moved up
    return -1;
}

void MainWindow::playPath(const QString &path, double position, bool paused, int subsong, bool preserveBufferedTail)
{
    if (path.isEmpty())
        return;
    if (ignored_.contains(path)) {
        status(tr("This song is ignored - restore it in Settings → Ignored songs"));
        return;
    }
    const int index = queueIndexOf(path);
    // A manual track change can happen before the next UI tick (including a
    // restart of the same path), so settle the previous interval now rather
    // than attributing it to the new playback generation.
    if (settings_.trackListeningStats) {
        flushStats();
        statsTimer_.restart();
    }
    engine_.playPath(path, position, paused, subsong, preserveBufferedTail);
    // Count a play only after the asynchronous load succeeds. Keeping the
    // generation lets a quick A→B change discard A rather than recording a
    // failed or superseded load, while a paused session is counted on resume.
    const EngineSnapshot requested = engine_.snapshot();
    skipDirection_ = pendingSkipDirection_;
    pendingSkipDirection_ = 0;
    skipGeneration_ = requested.songGeneration;
    if (skipDirection_ == 0)
        skipRun_ = 0;   // a song the user picked starts a new count
    if (settings_.trackListeningStats) {
        statsPendingPath_ = path;
        statsPendingGeneration_ = requested.songGeneration;
    } else {
        statsPendingPath_.clear();
        statsPendingGeneration_ = 0;
    }
    queueModel_->setPlayingPath(path);
    playingQueueIndex_ = index;
    // Show the row, but don't replace the user's selection: actions like
    // "Remove from playlist" act on it (a track change used to select the
    // newly playing row and remove that one instead).
    const QModelIndex row = queueModel_->indexOfPath(path);
    if (row.isValid()) {
        if (!queueView_->selectionModel()->hasSelection())
            queueView_->setCurrentIndex(row);
        queueView_->scrollTo(row);
    }
    ++songToken_;
    songTokenKey_.clear();
    tracker_->clearSong(); // tick requests metadata after the actual load completes
    if (index >= 0)
        status(tr("Playing %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::playSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty()) {
        status(tr("Nothing selected"));
        return;
    }
    playPath(path);
}

void MainWindow::togglePlay()
{
    const EngineSnapshot snap = engine_.snapshot();
    if (snap.playing || snap.paused) {
        engine_.togglePause();
        return;
    }
    if (!snap.path.isEmpty() && (snap.ended || snap.failed)) {
        playPath(snap.path);
        return;
    }
    QString path = selectedPath();
    if (path.isEmpty() && !queueTracks_.isEmpty()) {
        path = queueTracks_.first().path;
        pendingSkipDirection_ = 1;   // the queue's first song, not a pick
    }
    if (!path.isEmpty())
        playPath(path);
    else
        status(tr("Nothing to play"));
}

void MainWindow::playPrevious()
{
    const EngineSnapshot snap = engine_.snapshot();
    if (snap.playing && !snap.paused && snap.position > 3.0) {
        engine_.seek(0.0);
        return;
    }
    if (queueTracks_.isEmpty())
        return;
    int index = queueIndexOf(currentPath());
    if (index < 0)
        index = queueIndexOf(selectedPath());
    if (index == 0 && !settings_.loopQueue) {
        // like Next at the end: no wrap without Repeat queue
        if (!snap.path.isEmpty())
            engine_.seek(0.0);
        status(tr("Start of the queue"));
        return;
    }
    index = (index <= 0) ? queueTracks_.size() - 1 : index - 1;
    pendingSkipDirection_ = -1;
    playPath(queueTracks_[index].path);
}

void MainWindow::playNext()
{
    if (queueTracks_.isEmpty())
        return;
    int next = currentPath().isEmpty() ? -1 : nextQueueIndexAfter(currentPath());
    if (next < 0) {
        const int selected = queueIndexOf(selectedPath());
        next = selected + 1;   // nothing known: after the selection, or the first
    }
    if (next < queueTracks_.size()) {
        pendingSkipDirection_ = 1;
        playPath(queueTracks_[next].path);
        return;
    }
    if (!settings_.loopQueue) {
        status(tr("End of the queue - enable 'Repeat queue' (R) to start over"));
        return;
    }
    restartQueue();
}

void MainWindow::restartQueue()
{
    restartQueueImpl(false);
}

void MainWindow::restartQueueImpl(bool preserveBufferedTail)
{
    // Repeat the queue; in shuffle mode draw a new order that does not open
    // with the track that just finished.
    QString finished;
    const int index = queueIndexOf(currentPath());
    if (index >= 0)
        finished = queueTracks_[index].path;
    if (orderModeFromName(settings_.queueMode) == OrderMode::Shuffle && queueTracks_.size() > 1) {
        drawShufflePlan(freshShuffleSeed(settings_.shuffleSeed), QString(), finished);
        rebuildQueue();
        status(tr("Queue finished - new shuffle order (Ctrl+S, R), starting with %1")
                   .arg(queueTracks_.isEmpty() ? QStringLiteral("?")
                                               : queueTracks_.first().displayTitle()));
    } else {
        status(tr("Queue finished - starting over (R to repeat)"));
    }
    if (!queueTracks_.isEmpty()) {
        // the new order avoids the finished song only before search/filters
        // narrow it: don't open with it again when it is first anyway
        const int start = queueTracks_.size() > 1 && queueTracks_.first().path == finished ? 1 : 0;
        pendingSkipDirection_ = 1;
        playPath(queueTracks_[start].path, 0.0, false, 0, preserveBufferedTail);
    }
}

void MainWindow::onFinished()
{
    flushStats();
    const EngineSnapshot snap = engine_.snapshot();
    if (!settings_.autoAdvance || queueTracks_.isEmpty())
        return;
    const int next = nextQueueIndexAfter(snap.path);
    if (next < 0)
        return;
    if (next < queueTracks_.size()) {
        pendingSkipDirection_ = 1;
        playPath(queueTracks_[next].path, 0.0, false, 0, true);
        return;
    }
    if (settings_.loopQueue) {
        restartQueueImpl(true);
        return;
    }
    status(tr("Queue finished - enable 'Repeat queue' (R) to start over "
              "(shuffle then draws new order, Ctrl+S)"));
}

void MainWindow::handleLoadResult()
{
    // A file that can't be loaded (deleted after the scan, damaged) used to
    // stop the queue. When the app chose the song itself (auto-advance,
    // Next/Previous, Play on an unselected queue) it skips on in the same
    // direction; a song the user picked directly keeps the error on screen.
    const EngineSnapshot snap = engine_.snapshot();
    if (skipGeneration_ == 0 || snap.songGeneration != skipGeneration_ || snap.loading)
        return;   // not the load we asked for, or still loading
    skipGeneration_ = 0;
    if (!snap.failed) {
        skipRun_ = 0;
        return;
    }
    if (skipDirection_ == 0 || queueTracks_.isEmpty()) {
        skipRun_ = 0;
        return;
    }
    appendLog(QStringLiteral("warn"), tr("Skipped %1: %2").arg(QFileInfo(snap.path).fileName(), snap.loadError));
    // at most one pass over the queue: a queue of only broken files stops
    if (++skipRun_ >= queueTracks_.size()) {
        skipRun_ = 0;
        status(tr("No song in the queue could be loaded"));
        return;
    }
    int next;
    if (skipDirection_ > 0) {
        next = nextQueueIndexAfter(snap.path);
        if (next < 0)
            return;
        if (next >= queueTracks_.size()) {
            if (!settings_.loopQueue) {
                skipRun_ = 0;
                status(tr("Queue finished - %1 could not be loaded").arg(QFileInfo(snap.path).fileName()));
                return;
            }
            next = 0;
        }
    } else {
        const int here = queueIndexOf(snap.path);
        if (here < 0)
            return;
        if (here == 0 && !settings_.loopQueue) {
            skipRun_ = 0;
            status(tr("Start of the queue - %1 could not be loaded").arg(QFileInfo(snap.path).fileName()));
            return;
        }
        next = here == 0 ? queueTracks_.size() - 1 : here - 1;
    }
    pendingSkipDirection_ = skipDirection_;
    playPath(queueTracks_[next].path);
    status(tr("Skipped %1 (could not be loaded)").arg(QFileInfo(snap.path).fileName()));
}

void MainWindow::stopPlayback()
{
    flushStats();
    engine_.stop();
    queueModel_->setPlayingPath(QString());
    tracker_->clearSong();
    status(tr("Stopped"));
}

void MainWindow::toggleMute()
{
    muteButton_->toggle();
}

void MainWindow::addCurrentFavorite()
{
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    const bool on = playlists_.toggleFavorite(path);
    favoriteButton_->setText(on ? QStringLiteral("\u2605") : QStringLiteral("\u2606"));
    favoriteButton_->setChecked(on);
    refreshSourceCombo();
    if (PlaylistStore::isFavorites(settings_.queueSource))
        rebuildQueue();
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
        settings_.confirmIgnore = false;
        if (!saveSettings(settings_)) {
            settings_.confirmIgnore = true;
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
    // Mirror PlayerApp.change_ignored: select from the pre-change queue, not
    // the rebuilt queue (where the old index no longer identifies this song).
    const QVector<Track> oldQueue = queueTracks_;
    const EngineSnapshot snap = engine_.snapshot();
    if (!ignored_.change(add, remove)) {
        if (!ignored_.error.isEmpty())
            QMessageBox::warning(this, tr("Ignored songs"), ignored_.error);
        return false; // failed writes must not interrupt playback
    }
    const bool skip = !snap.path.isEmpty() && ignored_.contains(snap.path);
    QString nextPath;
    bool repeatShuffle = false;
    if (skip) {
        int index = -1;
        for (int i = 0; i < oldQueue.size(); ++i)
            if (oldQueue[i].path == snap.path) { index = i; break; }
        auto consider = [&](int begin, int end) {
            for (int i = begin; i < end && nextPath.isEmpty(); ++i)
                if (!ignored_.contains(oldQueue[i].path))
                    nextPath = oldQueue[i].path;
        };
        consider(index + 1, oldQueue.size());
        if (settings_.loopQueue && nextPath.isEmpty()) {
            repeatShuffle = orderModeFromName(settings_.queueMode) == OrderMode::Shuffle;
            consider(0, index + 1);
        }
        flushStats();
        engine_.pause();
        stopPlayback();
    }
    rebuildQueue();
    if (skip) {
        if (repeatShuffle && !queueTracks_.isEmpty()) {
            // The ignored song has already been excluded. Do not use a first
            // path captured from the previous shuffle and never include it again.
            drawShufflePlan(freshShuffleSeed(settings_.shuffleSeed), QString(), snap.path);
            rebuildQueue();
            nextPath = queueTracks_.isEmpty() ? QString() : queueTracks_.first().path;
        }
        if (!nextPath.isEmpty() && queueIndexOf(nextPath) >= 0) {
            const bool paused = snap.paused || (!snap.playing && !snap.loading);
            playPath(nextPath, 0.0, paused);
        } else {
            status(tr("Song ignored - no next eligible song in this queue "
                      "(Settings → Ignored songs to restore)"));
        }
    } else {
        status(tr("Ignore list updated - music files and saved memberships kept"));
    }
    if (settingsDialog_)
        settingsDialog_->refreshIgnoredCount();
    return true;
}

void MainWindow::revealCurrent()
{
    const QString path = currentPath().isEmpty() ? selectedPath() : currentPath();
    if (path.isEmpty())
        return;
    revealFile(path, this, [this](bool, bool, const QString &message) { status(message); });
}

void MainWindow::addToPlaylistMenu()
{
    QStringList paths = queueSelectedPaths();
    if (paths.isEmpty()) {
        const QString path = currentPath();
        if (!path.isEmpty())
            paths << path;
    }
    if (paths.isEmpty()) {
        status(tr("Select songs in the queue first"));
        return;
    }
    QMenu menu(this);
    for (const QString &name : playlists_.names()) {
        menu.addAction(name, [this, name, paths] {
            playlists_.addPaths(name, paths);
            refreshSourceCombo();
            status(tr("Added %1 songs to \"%2\"").arg(paths.size()).arg(name));
        });
    }
    menu.addSeparator();
    menu.addAction(tr("New playlist from selection\u2026"), [this, paths] {
        bool ok = false;
        const QString base = QInputDialog::getText(this, tr("New playlist"), tr("Playlist name:"),
                                                   QLineEdit::Normal, QStringLiteral("Playlist"),
                                                   &ok);
        if (!ok)
            return;
        QString err;
        const QString name = playlists_.uniqueName(base, QStringLiteral(" %1"), &err);
        if (name.isEmpty()) {
            status(err.isEmpty() ? tr("Invalid playlist name") : err);
            return;
        }
        if (!playlists_.create(name, paths, libraryRoot_)) {
            status(playlists_.error);
            return;
        }
        refreshSourceCombo();
        status(tr("Created \"%1\" with %2 songs").arg(name).arg(paths.size()));
    });
    menu.exec(QCursor::pos());
}

void MainWindow::removeFromPlaylist()
{
    if (settings_.queueSource.isEmpty())
        return;
    QStringList paths = queueSelectedPaths();
    if (paths.isEmpty()) {
        const QString path = currentPath();
        if (!path.isEmpty())
            paths << path;
    }
    if (paths.isEmpty()) {
        status(tr("Select songs to remove"));
        return;
    }
    playlists_.removePaths(settings_.queueSource, QSet<QString>(paths.begin(), paths.end()));
    rebuildQueue();
    status(tr("Removed %1 songs from \"%2\"").arg(paths.size()).arg(settings_.queueSource));
}

// ============================================================================
// dialogs
// ============================================================================
void MainWindow::openFilterDialog()
{
    FilterDialog dialog(this, filterFromSettings(), sourceTracks());   // the queue's source
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QueueFilter filter = dialog.criteria();
    settings_.filterFormats = filter.formats;
    settings_.filterMin = filter.minSeconds;
    settings_.filterMax = filter.maxSeconds;
    settings_.filterHideBroken = filter.hideBroken;
    commitSettings();
    rebuildQueue();
    status(filter.active() ? tr("Filter: %1").arg(filter.describe()) : tr("Filter cleared"));
}

void MainWindow::openPlaylistsDialog()
{
    PlaylistsDialog dialog(
        this, &playlists_, [this] {
            QStringList paths = queueSelectedPaths();
            if (paths.isEmpty()) {
                for (const Track &track : queueTracks_)
                    paths << track.path;
            }
            return paths;
        },
        [this](const QString &name, bool savedOrder) {
            settings_.queueSource = name;
            settings_.activePlaylist = name;
            // Load keeps the order choice; a new or imported list opens in
            // Saved order (README)
            if (savedOrder)
                settings_.queueMode = QStringLiteral("playlist");
            commitSettings();
            refreshSourceCombo();
            rebuildQueue();
            status(tr("Source: %1").arg(name));
        },
        [this](const QString &name) {
            QStringList paths;
            for (const Track &track : queueTracks_)
                paths << track.path;
            const bool exists = playlists_.has(name);
            if (exists && QMessageBox::question(
                              this, tr("Save queue as playlist"),
                              tr("Replace the songs of \"%1\" with the current queue (%2 songs)?")
                                  .arg(name).arg(paths.size()))
                              != QMessageBox::Yes)
                return;
            const bool ok = exists ? playlists_.replace(name, paths)
                                   : playlists_.create(name, paths, libraryRoot_);
            if (!ok) {
                status(playlists_.error);
                return;
            }
            if (!exists) {   // a new playlist opens in Saved order (README)
                settings_.queueSource = name;
                settings_.activePlaylist = name;
                settings_.queueMode = QStringLiteral("playlist");
                commitSettings();
            }
            refreshSourceCombo();
            rebuildQueue();
            status(tr("Saved the queue as \"%1\" (%2 songs)").arg(name).arg(paths.size()));
        },
        [this](const QString &oldName, const QString &newName) { onPlaylistRenamed(oldName, newName); },
        [this](const QString &name) { onPlaylistRemoved(name); });
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
    auto *dialog = new SettingsDialog(this, settings_, &ignored_);
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    settingsDialog_ = dialog;
    dialog->setSaveHandler([this, dialog] { return applySettingsFromDialog(dialog); });
    connect(dialog, &SettingsDialog::ignoredChanged, this, [this] { rebuildQueue(); });
    connect(dialog, &QDialog::rejected, dialog, &QWidget::close);
    dialog->show();
}

bool MainWindow::applySettingsFromDialog(SettingsDialog *dialog)
{
    Settings updated = settings_;
    dialog->collect(updated);
    const bool audioChanged = updated.backend != settings_.backend
        || updated.samplerate != settings_.samplerate
        || updated.bufferMs != settings_.bufferMs
        || updated.interpolation != settings_.interpolation;
    if (!saveSettings(updated)) {
        QMessageBox::warning(this, tr("Settings"),
                             tr("Could not save settings. Check the config folder and qt-ui.json."));
        return false;
    }
    const bool statsChanged = updated.trackListeningStats != settings_.trackListeningStats;
    if (statsChanged && settings_.trackListeningStats && !updated.trackListeningStats) {
        // Settle time collected under the old setting before turning recording
        // off; otherwise it would remain in the accumulator and be attributed
        // to a later session if stats are enabled again.
        flushStats();
        stats_.save();
    }
    settings_ = updated;
    if (statsChanged) {
        statsTimer_.restart();
        if (!settings_.trackListeningStats) {
            statsAccum_ = 0.0;
            statsPendingPath_.clear();
            statsPendingGeneration_ = 0;
        } else {
            const EngineSnapshot snap = engine_.snapshot();
            if (snap.loaded && !snap.loading && !snap.failed && !snap.paused
                && snap.playing) {
                statsPendingPath_ = snap.path;
                statsPendingGeneration_ = snap.songGeneration;
            }
        }
    }
    if (audioChanged)
        engine_.applySettings(settings_);
    uiTimer_->setInterval(std::max(1000 / std::clamp(settings_.uiFps, 5, 120), 8));
    tracker_->setSmoothScrolling(settings_.smoothTrackerScrolling);
    applyTheme();
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
    flushStats();
    stats_.save();
    StatsDialog dialog(this, &stats_);
    connect(&dialog, &StatsDialog::statsReset, this, [this] {
        statsAccum_ = 0.0;
        statsTimer_.restart();
        statsCountedGeneration_ = 0;
        const EngineSnapshot snap = engine_.snapshot();
        if (settings_.trackListeningStats && snap.loaded && !snap.loading
            && !snap.failed && !snap.paused && snap.playing) {
            statsPendingPath_ = snap.path;
            statsPendingGeneration_ = snap.songGeneration;
        }
    });
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
    toolbarRow_->setVisible(!show);
    queueBar_->setVisible(!show);
    if (!show)
        return;
    trackerSeen_ = true;
    syncTrackerSong();
}

void MainWindow::syncTrackerSong()
{
    const EngineSnapshot snap = engine_.snapshot();
    const QString key = snap.path + QLatin1Char('|') + QString::number(snap.subsong)
        + QLatin1Char('|') + QString::number(snap.songGeneration);
    if (snap.path.isEmpty()) {
        tracker_->clearSong();
        songTokenKey_.clear();
        return;
    }
    if (!snap.loaded || snap.loading || key == songTokenKey_)
        return;
    songTokenKey_ = key;
    ++songToken_;
    engine_.requestSongData(songToken_);
}

// ============================================================================
// tick + panels
// ============================================================================
void MainWindow::tick()
{
    syncTrackerSong();
    const EngineSnapshot snap = engine_.snapshot();
    updateTransport(snap);
    updateInfoPanel(snap);
    updateWindowTitle(snap);

    // playingChannels counts active mixer voices (including virtual voices),
    // NOT the module's pattern channels. Never resize bars to that number.
    vu_->setChannels(snap.channels);
    vu_->updateValues(snap.vu, snap.levelL, snap.levelR);

    if (snap.loaded && !snap.loading) {
        const bool running = (snap.playing || snap.ended) && !snap.paused && !snap.failed;
        tracker_->setPlaying(snap.order, snap.row, running, snap.paused);
        trackerHeader_->setText(tr("Order %1/%2, pattern %3, row %4, %5 ch")
                                    .arg(snap.order + 1)
                                    .arg(std::max(1, snap.numOrders))
                                    .arg(snap.pattern)
                                    .arg(snap.row)
                                    .arg(snap.channels)
            + (snap.numSubsongs > 1 ? tr(", subsong %1/%2%3").arg(snap.subsong+1).arg(snap.numSubsongs)
                .arg(snap.playAllSubsongs ? tr(" (all)") : QString()) : QString()));
    }

    if (!snap.loaded || snap.loading)
        trackerHeader_->setText(snap.loading ? tr("Loading tracker…") : tr("No module loaded"));

    // listening stats.  The engine loads asynchronously, so count a play only
    // for the generation requested by playPath() once it is really loaded. The
    // old implementation only recorded elapsed seconds with play=false, which
    // made every play count stay at zero.
    if (statsPath_ != snap.path) {
        flushStats();
        statsPath_ = snap.path;
        statsTitle_ = snap.info.title;
        statsTimer_.restart();
        if (snap.path.isEmpty()) {
            statsPendingPath_.clear();
            statsPendingGeneration_ = 0;
        }
    } else if (!snap.info.title.isEmpty()) {
        statsTitle_ = snap.info.title;
    }

    // EngineSnapshot::ended is a terminal state in the Qt renderer (it stays
    // true until the next load), so it must not be treated as active time.
    const bool statsLoaded = settings_.trackListeningStats && snap.loaded && !snap.loading
        && !snap.failed && !snap.paused;
    const bool statsActive = statsLoaded && snap.playing;
    // A very short module can reach ended between two UI ticks. It still counts
    // as a play, but ended must not contribute an endless stream of seconds.
    recordPendingStat(snap);
    if (statsActive) {
        const double elapsed = statsTimer_.restart() / 1000.0;
        // Do not turn a blocked UI or a suspended machine into fake listening
        // time. Normal refresh intervals are far below this two-second cap.
        if (elapsed >= 0.0 && elapsed <= 2.0)
            statsAccum_ += elapsed;
        if (statsAccum_ >= 15.0) {
            flushStats();
            stats_.save();
        }
    } else {
        statsTimer_.restart();
    }

    if (sessionTimer_.elapsed() >= 15000)
        saveSession();

    healthLabel_->setText(tr("buffer %1 ms").arg(settings_.bufferMs));
    healthLabel_->setToolTip(engine_.outputDescription());

    // transport reflow
    const bool compact = width() < kTransportWrapWidth;
    if (compact != transportCompact_) {
        transportCompact_ = compact;
        auto *grid = qobject_cast<QGridLayout *>(transportBar_->layout());
        QWidget *pos = transportBar_->findChild<QWidget *>(QStringLiteral("posGroup"));
        if (grid && pos) {
            grid->removeWidget(pos);
            if (compact)
                grid->addWidget(pos, 1, 0, 1, 3);
            else
                grid->addWidget(pos, 0, 2);
        }
    }
}

void MainWindow::updateTransport(const EngineSnapshot &snap)
{
    playButton_->setText(snap.playing && !snap.paused ? QStringLiteral("\u25AE\u25AE")
                                                       : QStringLiteral("\u25B6"));
    durationLabel_->setText(snap.durationValid ? formatTime(snap.duration)
                                               : (snap.loaded ? QStringLiteral("\u221E")
                                                              : QStringLiteral("0:00")));
    if (!seeking_) {
        double position = snap.seekPending ? snap.seekTarget : snap.position;
        if (snap.loop && !snap.seekPending && std::isfinite(snap.duration) && snap.duration > 0.0)
            position = std::fmod(position, snap.duration);
        const QSignalBlocker blocker(seekSlider_);
        seekSlider_->setValue(snap.durationValid
                                  ? int(position / snap.duration * 1000.0)
                                  : 0);
        timeLabel_->setText(formatTime(position));
    }
    const int index = queueIndexOf(snap.path);
    queuePosLabel_->setText(QStringLiteral("%1/%2")
                                .arg(index >= 0 ? index + 1 : 0)
                                .arg(queueTracks_.size()));
    loopLabel_->setText(snap.loop ? (snap.playAllSubsongs ? tr("loop all") : tr("looping")) : QString());
    loopCheck_->setToolTip(snap.playAllSubsongs
        ? tr("Repeat the whole subsong sequence; Next and Previous still change files.")
        : tr("Repeat the selected subsong."));
    seekSlider_->setToolTip(tr("Position within the current subsong (%1 of %2)")
        .arg(snap.subsong+1).arg(snap.numSubsongs));
    const QSignalBlocker blocker(volumeSlider_);
    volumeSlider_->setValue(settings_.volume);
    volumeLabel_->setText(QStringLiteral("%1%").arg(settings_.volume));
    if (snap.loading)
        statusLabel_->setText(tr("Loading %1...").arg(QFileInfo(snap.path).fileName()));
    else if (!snap.loadError.isEmpty())
        statusLabel_->setText(snap.loadError);
    else if (snap.loaded && statusLabel_->text().startsWith(QObject::tr("Loading ")))
        statusLabel_->setText(tr("Playing %1").arg(QFileInfo(snap.path).fileName()));
}

void MainWindow::updateInfoPanel(const EngineSnapshot &snap)
{
    const bool has = !snap.path.isEmpty();
    const QFileInfo info(snap.path.isEmpty() ? QString() : snap.path);
    if (has) {
        titleLabel_->setText(snap.info.title.isEmpty() ? info.fileName() : snap.info.title);
        subtitleLabel_->setText(snap.path);
    } else {
        titleLabel_->setText(tr("Nothing playing"));
        subtitleLabel_->setText(queueTracks_.isEmpty()
                                    ? tr("Open a folder to build the queue")
                                    : tr("Double-click a module to play it"));
    }
    favoriteButton_->setEnabled(has);
    ignoreButton_->setEnabled((snap.loaded || snap.loading) && !snap.path.isEmpty()
                              && !ignored_.contains(snap.path));
    revealButton_->setEnabled(has);
    songInfoButton_->setEnabled(snap.loaded);
    const QSignalBlocker blocker(favoriteButton_);
    const bool fav = has && playlists_.isFavorite(snap.path);
    favoriteButton_->setText(fav ? QStringLiteral("\u2605") : QStringLiteral("\u2606"));
    favoriteButton_->setChecked(fav);

    const ModuleInfo &meta = snap.info;
    infoLabels_.value(QStringLiteral("format"))
        ->setText(snap.loaded ? (meta.formatLong.isEmpty() ? meta.format.toUpper()
                                                            : meta.formatLong)
                              : QStringLiteral("-"));
    infoLabels_.value(QStringLiteral("tracker"))->setText(meta.tracker.isEmpty() ? QStringLiteral("-") : meta.tracker);
    infoLabels_.value(QStringLiteral("artist"))->setText(meta.artist.isEmpty() ? QStringLiteral("-") : meta.artist);
    QString moduleLine;
    if (snap.loaded) {
        moduleLine = tr("%1 ch, %2 ord, %3 pat, %4 smp")
                         .arg(meta.channels).arg(meta.orders).arg(meta.patterns).arg(meta.samples);
        if (meta.instruments > 0)
            moduleLine += tr(", %1 inst").arg(meta.instruments);
    }
    infoLabels_.value(QStringLiteral("size"))->setText(moduleLine.isEmpty() ? QStringLiteral("-") : moduleLine);
    infoLabels_.value(QStringLiteral("length"))->setText(
        snap.durationValid ? formatTime(snap.duration) : (snap.loaded ? QStringLiteral("\u221E") : QStringLiteral("-")));
    infoLabels_.value(QStringLiteral("position"))->setText(
        snap.loaded ? tr("order %1, pattern %2, row %3")
                          .arg(snap.order + 1).arg(snap.pattern).arg(snap.row)   // patterns 0-based, as in the tracker header
                    : QStringLiteral("-"));
    infoLabels_.value(QStringLiteral("sequencer"))->setText(
        snap.loaded ? tr("speed %1, tempo %2").arg(snap.speed).arg(snap.tempo) : QStringLiteral("-"));
    auto interpLabel = [](int length) {
        switch (length) {
        case 1: return QObject::tr("No interpolation");
        case 2: return QObject::tr("Linear (2-tap)");
        case 4: return QObject::tr("Cubic (4-tap)");
        default: return QObject::tr("Sinc (8-tap)");
        }
    };
    infoLabels_.value(QStringLiteral("resample"))->setText(interpLabel(snap.interpolation));
    infoLabels_.value(QStringLiteral("output"))->setText(engine_.outputDescription());

    const QSignalBlocker spinBlocker(subsongSpin_);
    const int subsongs = std::max(0, int(meta.subsongs));
    subsongSpin_->setEnabled(subsongs > 1 && snap.loaded);
    // Leave the box alone while the user is typing in it, and only touch it
    // on a change: every tick's setValue reset the edit text.
    if (!subsongSpin_->hasFocus()) {
        if (subsongSpin_->maximum() != std::max(1, subsongs))
            subsongSpin_->setRange(1, std::max(1, subsongs));
        if (subsongSpin_->value() != snap.subsong + 1)
            subsongSpin_->setValue(snap.subsong + 1);
    }
    subsongCountLabel_->setText(subsongs > 1 ? tr("of %1").arg(subsongs) : tr("of 1"));
    const QString name = snap.loaded ? meta.subsongNames.value(snap.subsong).trimmed() : QString();
    subsongNameLabel_->setText(name);
    subsongNameLabel_->setVisible(!name.isEmpty());
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

void MainWindow::recordPendingStat(const EngineSnapshot &snap)
{
    if (!settings_.trackListeningStats || !snap.loaded || snap.loading || snap.failed
        || snap.paused || snap.path.isEmpty() || (!snap.playing && !snap.ended)
        || statsPendingPath_ != snap.path
        || statsPendingGeneration_ != snap.songGeneration
        || statsCountedGeneration_ == snap.songGeneration)
        return;
    if (stats_.record(snap.path, 0.0, true, snap.info.title)) {
        statsDirty_ = true;
        statsCountedGeneration_ = snap.songGeneration;
        statsPendingPath_.clear();
        statsPendingGeneration_ = 0;
    }
}

void MainWindow::flushStats()
{
    // Also settle a pending play here. This covers very short modules and
    // track changes that arrive before the next periodic UI tick.
    recordPendingStat(engine_.snapshot());
    if (statsAccum_ > 0.5 && !statsPath_.isEmpty() && settings_.trackListeningStats) {
        if (stats_.record(statsPath_, statsAccum_, false, statsTitle_))
            statsDirty_ = true;
    }
    statsAccum_ = 0.0;
}

void MainWindow::saveSession(bool force)
{
    if (!settings_.rememberPosition && !force)
        return;
    const EngineSnapshot snap = engine_.snapshot();
    if (!snap.path.isEmpty() && snap.loaded && !snap.loading) {
        settings_.subsong = snap.subsong;
        settings_.lastPath = snap.path;
        settings_.lastPosition = snap.position;
        if (snap.loop && std::isfinite(snap.duration) && snap.duration > 0.0)
            settings_.lastPosition = std::fmod(snap.position, snap.duration);
        if (snap.ended)
            settings_.lastPosition = 0.0;   // a finished song resumes from its start
    } else if (snap.path.isEmpty()) {
        settings_.lastPath.clear();
        settings_.lastPosition = 0.0;
        settings_.subsong = 0;
    }
    sessionTimer_.restart();
    commitSettings();
}

void MainWindow::commitSettings()
{
    saveSettings(settings_);
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
    static_cast<JumpSlider *>(volumeSlider_)->applyPalette(&palette_);
    seekSlider_->applyPalette(&palette_);
    vu_->applyPalette(&palette_);
    tracker_->applyPalette(&palette_);
    queueModel_->setAccent(palette_[Palette::ACCENT_DIM], palette_[Palette::ON_ACCENT]);
    queueModel_->setDirectoryColor(palette_[Palette::ACCENT]);
    queueModel_->setColors(palette_[Palette::BG_STRIPE], palette_[Palette::FG_DIM],
                            palette_[Palette::RED]);
    queueView_->ensureHeaderWidths();
}

void MainWindow::resetLayout()
{
    queueView_->resetColumnWidths(); // retain the chosen visible-column set
    if (splitter_)
        splitter_->setSizes({600, 400});
    queueView_->verticalScrollBar()->setValue(0);
    status(tr("Layout reset - column widths, the pane split and scrolling are back to "
              "how the window opened (Ctrl+0)"));
}

void MainWindow::appendLog(const QString &level, const QString &text)
{
    if (!logView_)
        return;
    QString color = palette_.name(Palette::FG_DIM);
    if (level == QLatin1String("warn"))
        color = palette_.name(Palette::AMBER);
    else if (level == QLatin1String("error"))
        color = palette_.name(Palette::RED);
    else if (level == QLatin1String("debug"))
        color = palette_.name(Palette::FG_FAINT);
    logView_->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>")
                             .arg(color,
                                  level == QLatin1String("info")
                                      ? text.toHtmlEscaped()
                                      : QStringLiteral("[%1] %2").arg(level, text.toHtmlEscaped())));
}

void MainWindow::status(const QString &text)
{
    statusLabel_->setText(text);
    statusLabel_->setToolTip(text);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == seekSlider_ && event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::RightButton) {
            engine_.seekFraction(0.0);
            seekSlider_->setValue(0);
            timeLabel_->setText(formatTime(0.0));
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    closing_ = true;
    flushStats();
    if (statsDirty_)
        stats_.save();
    saveSession(true);
    const QRect rect = geometry();
    settings_.windowGeometry = QStringLiteral("%1x%2+%3+%4")
                                   .arg(rect.width()).arg(rect.height())
                                   .arg(rect.x()).arg(rect.y());
    engine_.stop();
    saveSettings(settings_);
    QMainWindow::closeEvent(event);
}

Settings MainWindow::persistedSettings(const Settings &s) const
{
    // Command-line options are for this session: a field still holding its
    // command-line value is saved with the value it had before. A field the
    // user changed in the app since is saved as usual.
    return withoutSessionOverrides(s, cliSaved_, cliSession_);
}

void MainWindow::applyCliOverrides(const QHash<QString, QString> &overrides)
{
    auto take = [&overrides](const QString &key) { return overrides.value(key); };
    // remember what was saved, so persistedSettings() can write it back
    cliSaved_.insert(QStringLiteral("volume"), QString::number(settings_.volume));
    cliSaved_.insert(QStringLiteral("theme"), settings_.theme);
    cliSaved_.insert(QStringLiteral("interpolation"), settings_.interpolation);
    cliSaved_.insert(QStringLiteral("backend"), settings_.backend);
    if (overrides.contains(QStringLiteral("volume"))) {
        settings_.volume = std::clamp(take(QStringLiteral("volume")).toInt(), 0, 100);
        cliSession_.insert(QStringLiteral("volume"), QString::number(settings_.volume));
        engine_.setVolume(settings_.volume);
        volumeSlider_->setValue(settings_.volume);   // the slider showed the saved volume
    }
    if (overrides.contains(QStringLiteral("theme"))) {
        settings_.theme = Palette::normalizeTheme(take(QStringLiteral("theme")), settings_.customThemes);
        cliSession_.insert(QStringLiteral("theme"), settings_.theme);
        applyTheme();   // the constructor applied the saved theme already
    }
    if (overrides.contains(QStringLiteral("interpolation"))) {
        settings_.interpolation = take(QStringLiteral("interpolation"));
        cliSession_.insert(QStringLiteral("interpolation"), settings_.interpolation);
    }
    if (overrides.contains(QStringLiteral("backend"))) {
        settings_.backend = take(QStringLiteral("backend"));
        cliSession_.insert(QStringLiteral("backend"), settings_.backend);
    }
    engine_.applySettings(settings_);
    if (overrides.contains(QStringLiteral("speed"))) {
        bool ok = false;
        const double speed = take(QStringLiteral("speed")).toDouble(&ok);
        if (ok && speed > 0.0)
            engine_.setTempoFactor(speed);
    }
}

void MainWindow::startupPlay(const QString &track, bool autoplay)
{
    QTimer::singleShot(0, this, [this, track, autoplay] {
        if (!track.isEmpty()) {
            const QString path = QFileInfo::exists(track)
                ? QFileInfo(track).absoluteFilePath()
                : track;
            playPath(path);
            return;
        }
        if (autoplay && !queueTracks_.isEmpty()) {
            playPath(queueTracks_.first().path);
            return;
        }
        if (settings_.rememberPosition && !settings_.lastPath.isEmpty()
            && QFile::exists(settings_.lastPath))
            playPath(settings_.lastPath, settings_.lastPosition, true, settings_.subsong);
    });
}

// ---------------------------------------------------------------------------
// Screenshot driver.  MODJUKE_SHOTS=<dir>[:<w>x<h>] captures the player tab,
// the tracker tab, and every dialog as PNG files, then quits.
// Used together with xvfb-run.
void MainWindow::startShotDriver()
{
    const QString spec = QString::fromLocal8Bit(qgetenv("MODJUKE_SHOTS"));
    const QString dir = spec.split(QLatin1Char(':')).first();
    const QString sizePart =
        spec.contains(QLatin1Char(':')) ? spec.section(QLatin1Char(':'), 1) : QString();
    static const QRegularExpression sizeRe(QStringLiteral("(\\d+)x(\\d+)"));
    const QRegularExpressionMatch sizeMatch = sizeRe.match(sizePart);
    if (sizeMatch.hasMatch())
        resize(sizeMatch.captured(1).toInt(), sizeMatch.captured(2).toInt());
    else
        resize(1180, 720);
    QDir().mkpath(dir);

    auto *timer = new QTimer(this);
    timer->setInterval(1500);
    auto *step = new int(0);
    connect(timer, &QTimer::timeout, this, [this, dir, timer, step] {
        auto saveTab = [this, dir](int index, const QString &name) {
            tabBar_->setCurrentIndex(index);
            qApp->processEvents();
            qApp->processEvents();
            grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
        };
        auto captureDialog = [dir](QDialog *dialog, const QString &name) {
            dialog->setAttribute(Qt::WA_DontShowOnScreen);
            dialog->show();
            qApp->processEvents();
            dialog->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
            dialog->hide();
            dialog->deleteLater();
        };
        switch (*step) {
        case 0:
            saveTab(0, QStringLiteral("player"));
            break;
        case 1:
            saveTab(1, QStringLiteral("tracker"));
            break;
        case 2:
            saveTab(0, QStringLiteral("player_returned"));
            break;
        case 3: {
            OpenMPTLib *lib = OpenMPTLib::instance();
            ModuleInfo info;
            snapShotForShots(info);
            captureDialog(new SongInfoDialog(nullptr, info), QStringLiteral("songinfo"));
            captureDialog(new StatsDialog(nullptr, &stats_), QStringLiteral("stats"));
            captureDialog(new IgnoreDialog(nullptr, &ignored_), QStringLiteral("ignored"));
            captureDialog(new FilterDialog(nullptr, filterFromSettings(), libraryTracks_),
                          QStringLiteral("filter"));
            captureDialog(new SettingsDialog(nullptr, settings_, &ignored_),
                          QStringLiteral("settings"));
            captureDialog(new PlaylistsDialog(nullptr, &playlists_,
                                              [this] { return queueSelectedPaths(); },
                                              [](const QString &, bool) {}, [](const QString &) {},
                                              [](const QString &, const QString &) {},
                                              [](const QString &) {}),
                          QStringLiteral("playlists"));
            captureDialog(new AboutDialog(nullptr, lib ? lib->versionString() : QString(),
                                          lib != nullptr),
                          QStringLiteral("about"));
            break;
        }
        default:
            timer->stop();
            qApp->quit();
            return;
        }
        ++(*step);
    });
    timer->start(2500);
}

bool MainWindow::snapShotForShots(ModuleInfo &outInfo)
{
    const EngineSnapshot snap = engine_.snapshot();
    if (snap.path.isEmpty())
        return false;
    OpenMPTLib *lib = OpenMPTLib::instance();
    if (!lib)
        return false;
    OpenMPTModule module = lib->openFile(snap.path, {}, nullptr);
    if (!module.isOpen())
        return false;
    outInfo = module.info(snap.path);
    return true;
}
