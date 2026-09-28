#include "mainwindow.h"
#include "casefold.h"

#include "openmptapi.h"
#include "stores.h"
#include "reveal.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDockWidget>
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
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)x(\\d+)([+-]\\d+)([+-]\\d+)$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return QRect();
    int x = m.captured(3).toInt(), y = m.captured(4).toInt();
    if (x < 0)
        x = 100 + x;
    if (y < 0)
        y = 100 + y;
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
    connect(&engine_, &Engine::loadReady, this, &MainWindow::tick);
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
        if (!settings_.queueSource.isEmpty() && !playlists_.get(settings_.queueSource)) {
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
    refreshSourceCombo();
    migrateShuffleOrder();

    const QRect geometry = parseGeometry(settings_.windowGeometry);
    if (geometry.isValid())
        setGeometry(geometry);
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
                            static_cast<QWidget *>(sourceCombo_), orderCombo_, filterLabel_})
        widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
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
                    QString name = PlaylistStore::normalizeName(base, &err);
                    int suffix = 2;
                    while (name.isEmpty() ? false : playlists_.has(name))
                        name = QStringLiteral("%1 %2").arg(base).arg(suffix++);
                    if (name.isEmpty()) {
                        status(err);
                        return;
                    }
                    QStringList paths = selected.isEmpty() ? QStringList{path} : selected;
                    playlists_.create(name, paths, libraryRoot_);
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
    queueView_->viewport()->installEventFilter(this);
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
    for (int c = QueueModel::Module; c < QueueModel::COLUMN_COUNT; ++c) {
        auto *action = columnsMenu->addAction(QueueModel::columnName(c));
        action->setObjectName(QStringLiteral("queueColumn_") + QueueModel::columnKey(c));
        action->setCheckable(true);
        action->setChecked(!queueView_->isColumnHidden(c));
        if (c == QueueModel::Module) {
            action->setEnabled(false);
            action->setToolTip(tr("Module is always shown."));
            continue;
        }
        connect(action, &QAction::toggled, this, [this, action, c](bool shown) {
            const QStringList previous = settings_.hiddenQueueColumns;
            const QString key = QueueModel::columnKey(c);
            settings_.hiddenQueueColumns.removeAll(key);
            if (!shown) settings_.hiddenQueueColumns << key;
            if (!settings_.save()) {
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
    // the Tk layout ends with a status row and the transport below it; stack
    // both in one chrome-less bottom dock, status first.
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("barPanel"));
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 2, 10, 2);
    statusLabel_ = new QLabel(tr("Ready"), bar);
    statusLabel_->setObjectName(QStringLiteral("dimLabel"));
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

    auto *bottom = new QWidget(this);
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    bl->addWidget(bar);
    bl->addWidget(transportBar_);
    auto *dock = new QDockWidget(this);
    dock->setObjectName(QStringLiteral("bottomDock"));
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    auto *emptyTitle = new QWidget(dock);
    emptyTitle->setFixedHeight(0);
    dock->setTitleBarWidget(emptyTitle);
    dock->setWidget(bottom);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void MainWindow::bindShortcuts()
{
    auto key = [this](const QKeySequence &sequence, std::function<void()> action) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, action = std::move(action)] {
            if (typing())
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
    QWidget *focus = QApplication::focusWidget();
    return qobject_cast<QLineEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus)
        || qobject_cast<QSpinBox *>(focus);
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
    if (!result.errors.isEmpty())
        status(tr("Scan finished with %1 problems").arg(result.errors.size()));
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
    QSet<QString> missing;
    for (const Track &track : queueTracks_) {
        if (!QFile::exists(track.path))
            missing.insert(track.path);
    }
    queueModel_->setMissingPaths(missing);
    queueModel_->setPlayingPath(currentPath());
    queueView_->setReorderEnabled(savedMode);
    filterLabel_->setText(filter.active() ? filter.describe() : QString());

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
    QStringList kept;
    QSet<QString> queued;
    for (const Track &track : queueTracks_)
        queued.insert(track.path);
    for (const QString &path : playlist->paths) {
        if (!queued.contains(path))
            kept << path;
    }
    playlists_.replace(settings_.queueSource, newOrder + kept);
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
    return queueModel_->queueIndexAt(queueModel_->indexOfPath(path));
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
    engine_.playPath(path, position, paused, subsong, preserveBufferedTail);
    queueModel_->setPlayingPath(path);
    if (index >= 0) {
        queueView_->setCurrentIndex(queueModel_->indexOfPath(path));
        queueView_->scrollTo(queueModel_->indexOfPath(path));
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
    if (path.isEmpty() && !queueTracks_.isEmpty())
        path = queueTracks_.first().path;
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
    index = (index <= 0) ? queueTracks_.size() - 1 : index - 1;
    playPath(queueTracks_[index].path);
}

void MainWindow::playNext()
{
    if (queueTracks_.isEmpty())
        return;
    int index = queueIndexOf(currentPath());
    if (index < 0)
        index = queueIndexOf(selectedPath());
    if (index + 1 < queueTracks_.size()) {
        playPath(queueTracks_[index + 1].path);
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
    if (!queueTracks_.isEmpty())
        playPath(queueTracks_.first().path, 0.0, false, 0, preserveBufferedTail);
}

void MainWindow::onFinished()
{
    flushStats();
    const EngineSnapshot snap = engine_.snapshot();
    if (!settings_.autoAdvance || queueTracks_.isEmpty())
        return;
    const int index = queueIndexOf(snap.path);
    if (index < 0)
        return;
    if (index + 1 < queueTracks_.size()) {
        playPath(queueTracks_[index + 1].path, 0.0, false, 0, true);
        return;
    }
    if (settings_.loopQueue) {
        restartQueueImpl(true);
        return;
    }
    status(tr("Queue finished - enable 'Repeat queue' (R) to start over "
              "(shuffle then draws new order, Ctrl+S)"));
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
        if (!settings_.save()) {
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
        QString name = PlaylistStore::normalizeName(base, &err);
        int suffix = 2;
        while (!name.isEmpty() && playlists_.has(name))
            name = QStringLiteral("%1 %2").arg(base).arg(suffix++);
        if (name.isEmpty()) {
            status(err.isEmpty() ? tr("Invalid playlist name") : err);
            return;
        }
        playlists_.create(name, paths, libraryRoot_);
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
    FilterDialog dialog(this, filterFromSettings(), libraryTracks_);
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
        [this](const QString &name) {
            settings_.queueSource = name;
            settings_.activePlaylist = name;
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
            playlists_.replace(name, paths);
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
    if (!updated.save()) {
        QMessageBox::warning(this, tr("Settings"),
                             tr("Could not save settings. Check the config folder and qt-ui.json."));
        return false;
    }
    settings_ = updated;
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

    // listening stats
    if (settings_.trackListeningStats && snap.playing && !snap.paused && snap.loaded) {
        if (statsPath_ != snap.path) {
            flushStats();
            statsPath_ = snap.path;
        }
        statsAccum_ += statsTimer_.restart() / 1000.0;
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
                          .arg(snap.order + 1).arg(snap.pattern + 1).arg(snap.row)
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
    subsongSpin_->setRange(1, std::max(1, subsongs));
    subsongSpin_->setValue(snap.subsong + 1);
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

void MainWindow::flushStats()
{
    if (statsAccum_ > 0.5 && !statsPath_.isEmpty() && settings_.trackListeningStats) {
        const EngineSnapshot snap = engine_.snapshot();
        stats_.record(statsPath_, statsAccum_, false, snap.info.title);
        statsAccum_ = 0.0;
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
    settings_.save();
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
    if (queueView_ && watched == queueView_->viewport() && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
            // let the view move the caret, then play that row (like the Tk version)
            QTimer::singleShot(0, this, [this] {
                const QString path = queueModel_->pathAt(queueView_->currentIndex());
                if (!path.isEmpty())
                    playPath(path);
            });
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
    settings_.save();
    QMainWindow::closeEvent(event);
}

void MainWindow::applyCliOverrides(const QHash<QString, QString> &overrides)
{
    auto take = [&overrides](const QString &key) { return overrides.value(key); };
    if (overrides.contains(QStringLiteral("volume"))) {
        settings_.volume = std::clamp(take(QStringLiteral("volume")).toInt(), 0, 100);
        engine_.setVolume(settings_.volume);
    }
    if (overrides.contains(QStringLiteral("theme")))
        settings_.theme = Palette::normalizeTheme(take(QStringLiteral("theme")), settings_.customThemes);
    if (overrides.contains(QStringLiteral("interpolation")))
        settings_.interpolation = take(QStringLiteral("interpolation"));
    if (overrides.contains(QStringLiteral("backend")))
        settings_.backend = take(QStringLiteral("backend"));
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
                                              [](const QString &) {}, [](const QString &) {},
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
