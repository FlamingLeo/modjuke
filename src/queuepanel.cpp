#include "queuepanel.h"
#include "widgets.h"

#include "playliststore.h"
#include "queue.h"

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <initializer_list>

QueuePanel::QueuePanel(const QStringList &hiddenColumns, QWidget *window) : QWidget(nullptr)
{
    // a QWidget subclass paints its stylesheet background only with this
    setAttribute(Qt::WA_StyledBackground);
    buildFolderRow(window);
    buildControlsRow(window);
    buildTable(hiddenColumns);
}

void QueuePanel::buildFolderRow(QWidget *window)
{
    auto *bar = new QWidget(window);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 6, 10, 2);
    auto *openButton = new QPushButton(tr("Open folder\u2026"), bar);
    openButton->setObjectName(QStringLiteral("accentButton"));
    auto *rescanButton = new QPushButton(tr("Rescan"), bar);
    analyzeButton_ = new QPushButton(tr("Analyze"), bar);
    dirField_ = new QLineEdit(bar);
    dirField_->setReadOnly(true);
    dirField_->setPlaceholderText(tr("no library folder"));
    countLabel_ = new QLabel(bar);
    countLabel_->setObjectName(QStringLiteral("dimLabel"));
    layout->addWidget(openButton);
    layout->addWidget(rescanButton);
    layout->addWidget(analyzeButton_);
    layout->addWidget(dirField_, 1);
    layout->addWidget(countLabel_);
    folderRow_ = bar;
    folderRow_->setObjectName(QStringLiteral("libraryToolbar"));

    connect(openButton, &QPushButton::clicked, this, &QueuePanel::openFolderClicked);
    connect(rescanButton, &QPushButton::clicked, this, &QueuePanel::rescanClicked);
    connect(analyzeButton_, &QPushButton::clicked, this, &QueuePanel::analyzeClicked);
}

void QueuePanel::buildControlsRow(QWidget *window)
{
    auto *bar = new QWidget(window);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 2, 10, 4);
    auto *sourceLabel = new QLabel(tr("Source:"), bar);
    sourceCombo_ = new QComboBox(bar);
    sourceCombo_->setMinimumWidth(140);
    auto *orderLabel = new QLabel(tr("Order:"), bar);
    orderCombo_ = new QComboBox(bar);
    orderCombo_->setMinimumWidth(110);
    shuffleBtn_ = new QPushButton(tr("Shuffle now"), bar);
    auto *playlistsBtn = new QPushButton(tr("Playlists"), bar);
    playlistsBtn->setToolTip(tr("Save the queue as a playlist (Ctrl+P)"));
    auto *filterBtn = new QPushButton(tr("Filter"), bar);
    filterBtn->setToolTip(tr("Filter formats, length, broken files (Ctrl+Shift+F)"));
    auto *searchLabel = new QLabel(tr("Search:"), bar);
    searchEdit_ = new QLineEdit(bar);
    searchEdit_->setPlaceholderText(tr("title or file name"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMaximumWidth(220);
    filterLabel_ = new ElidedLabel(bar);
    filterLabel_->setObjectName(QStringLiteral("dimLabel"));
    // Spare width belongs to the trailing stretch, not to the text labels.
    for (QWidget *widget : std::initializer_list<QWidget *>{static_cast<QWidget *>(sourceLabel), orderLabel, searchLabel,
                            static_cast<QWidget *>(sourceCombo_), orderCombo_})
        widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    // A long filter description must not widen the window's minimum size
    // (that clipped the transport instead): it gets the spare width and is
    // shortened with "…" beyond that; the tooltip has it all.
    filterLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    filterLabel_->setMinimumWidth(40);
    layout->setSpacing(6);
    layout->addWidget(sourceLabel);
    layout->addWidget(sourceCombo_);
    layout->addWidget(orderLabel);
    layout->addWidget(orderCombo_);
    layout->addWidget(shuffleBtn_);
    layout->addWidget(playlistsBtn);
    layout->addWidget(filterBtn);
    layout->addWidget(searchLabel);
    layout->addWidget(searchEdit_, 1);
    layout->addWidget(filterLabel_, 1);

    connect(sourceCombo_, &QComboBox::activated, this,
            [this](int) { emit sourceChosen(sourceCombo_->currentData().toString()); });
    connect(orderCombo_, &QComboBox::activated, this,
            [this](int) { emit orderChosen(orderCombo_->currentData().toString()); });
    connect(shuffleBtn_, &QPushButton::clicked, this, &QueuePanel::shuffleClicked);
    connect(playlistsBtn, &QPushButton::clicked, this, &QueuePanel::playlistsClicked);
    connect(filterBtn, &QPushButton::clicked, this, &QueuePanel::filterClicked);
    connect(searchEdit_, &QLineEdit::textChanged, this, &QueuePanel::searchChanged);
    controlsRow_ = bar;
    controlsRow_->setObjectName(QStringLiteral("queueControls"));
}

void QueuePanel::buildTable(const QStringList &hiddenColumns)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *actions = new QHBoxLayout;
    auto *addToPlaylistBtn = new QPushButton(tr("Add to playlist\u2026"), this);
    removeFromPlaylistBtn_ = new QPushButton(tr("Remove from playlist"), this);
    actions->addWidget(addToPlaylistBtn);
    actions->addWidget(removeFromPlaylistBtn_);
    actions->addStretch();
    layout->addLayout(actions);

    view_ = new QueueTableView(this);
    model_ = new QueueModel(this);   // after the view: deleted after it, too
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setShowGrid(false);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setWordWrap(false);
    view_->verticalHeader()->setVisible(false);
    auto *header = view_->horizontalHeader();
    view_->resetColumnWidths();
    for (int c = QueueModel::Folder; c < QueueModel::COLUMN_COUNT; ++c)
        view_->setColumnHidden(c, hiddenColumns.contains(QueueModel::columnKey(c)));

    auto *columnsMenu = new QMenu(tr("Columns"), view_);
    columnsMenu->setObjectName(QStringLiteral("queueColumnsMenu"));
    columnsMenu->setToolTipsVisible(true);
    // Module is the queue's identity column and is always visible. Do not
    // include it in this menu: an unavailable option is clearer when omitted
    // than when it looks like a disabled toggle.
    columnActions_.fill(nullptr, QueueModel::COLUMN_COUNT);
    for (int c = QueueModel::Folder; c < QueueModel::COLUMN_COUNT; ++c) {
        auto *action = columnsMenu->addAction(QueueModel::columnName(c));
        action->setObjectName(QStringLiteral("queueColumn_") + QueueModel::columnKey(c));
        action->setCheckable(true);
        action->setChecked(!view_->isColumnHidden(c));
        connect(action, &QAction::toggled, this, [this, c](bool shown) { emit columnToggled(c, shown); });
        columnActions_[c] = action;
    }
    auto *columnsButton = new QToolButton(this);
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
    layout->addWidget(view_, 1);

    connect(addToPlaylistBtn, &QPushButton::clicked, this, &QueuePanel::addToPlaylistClicked);
    connect(removeFromPlaylistBtn_, &QPushButton::clicked, this, &QueuePanel::removeFromPlaylistClicked);
    connect(view_, &QueueTableView::activatedPath, this, &QueuePanel::songActivated);
    connect(view_, &QueueTableView::contextMenuFor, this, &QueuePanel::contextMenuRequested);
    connect(model_, &QueueModel::orderEdited, this, &QueuePanel::orderEdited);
}

void QueuePanel::setRowsVisible(bool visible)
{
    folderRow_->setVisible(visible);
    controlsRow_->setVisible(visible);
}

QString QueuePanel::folder() const
{
    return dirField_->text();
}

void QueuePanel::setFolder(const QString &root)
{
    dirField_->setText(root);
    dirField_->setToolTip(root);
}

void QueuePanel::setTrackCount(int tracks, int folders)
{
    countLabel_->setText(QStringLiteral("%1 tracks, %2 folders").arg(tracks).arg(folders));
}

void QueuePanel::showAnalysisProgress(int done, int total)
{
    analyzeButton_->setText(tr("Analyzing %1/%2").arg(done).arg(total));
}

void QueuePanel::showAnalysisIdle()
{
    analyzeButton_->setText(tr("Analyze"));
}

void QueuePanel::setSources(const PlaylistStore &playlists, const QString &current)
{
    const QSignalBlocker blocker(sourceCombo_);
    sourceCombo_->clear();
    sourceCombo_->addItem(tr("Library"), QString());
    for (const QString &name : playlists.names()) {
        if (PlaylistStore::isFavorites(name))
            sourceCombo_->addItem(tr("Favorites (%1)").arg(playlists.get(name)->paths.size()),
                                  name);
        else
            sourceCombo_->addItem(name, name);
    }
    for (int i = 0; i < sourceCombo_->count(); ++i)
        if (sourceCombo_->itemData(i).toString() == current)
            sourceCombo_->setCurrentIndex(i);
}

void QueuePanel::setSourceKind(bool playlist, const QString &mode)
{
    // Rebuild the order combo: "Saved order" only for playlists.
    const QString wanted = (playlist || mode != QLatin1String("playlist"))
                               ? mode
                               : QStringLiteral("by directory");
    const QSignalBlocker blocker(orderCombo_);
    orderCombo_->clear();
    orderCombo_->addItem(tr("Alphabetical"), QStringLiteral("alphabetical"));
    orderCombo_->addItem(tr("By directory"), QStringLiteral("by directory"));
    orderCombo_->addItem(tr("Shuffle"), QStringLiteral("shuffle"));
    if (playlist)
        orderCombo_->addItem(tr("Saved order"), QStringLiteral("playlist"));
    for (int i = 0; i < orderCombo_->count(); ++i)
        if (orderCombo_->itemData(i).toString() == wanted)
            orderCombo_->setCurrentIndex(i);
    removeFromPlaylistBtn_->setEnabled(playlist);
}

void QueuePanel::setShuffleEnabled(bool enabled)
{
    shuffleBtn_->setEnabled(enabled);
}

QString QueuePanel::searchText() const
{
    return searchEdit_->text();
}

void QueuePanel::focusSearch()
{
    searchEdit_->setFocus();
}

void QueuePanel::clearSearch()
{
    if (!searchEdit_->text().isEmpty())
        searchEdit_->clear();
}

void QueuePanel::showQueue(const QVector<Track> &queue, bool directoryRows,
                           const QString &playingPath, bool reorderable)
{
    model_->setRows(queue, directoryRows);
    QSet<QString> missing;
    for (const Track &track : queue) {
        if (!QFile::exists(track.path))
            missing.insert(track.path);
    }
    model_->setMissingPaths(missing);
    model_->setPlayingPath(playingPath);
    view_->setReorderEnabled(reorderable);
}

void QueuePanel::setFilterText(const QString &text)
{
    filterLabel_->setFullText(text);
}

void QueuePanel::setPlayingPath(const QString &path)
{
    model_->setPlayingPath(path);
}

void QueuePanel::showPlaying(const QString &path)
{
    model_->setPlayingPath(path);
    // Show the row, but don't replace the user's selection: actions like
    // "Remove from playlist" act on it (a track change used to select the
    // newly playing row and remove that one instead).
    const QModelIndex row = model_->indexOfPath(path);
    if (row.isValid()) {
        if (!view_->selectionModel()->hasSelection())
            view_->setCurrentIndex(row);
        view_->scrollTo(row);
    }
}

void QueuePanel::revealPath(const QString &path)
{
    const auto index = model_->indexOfPath(path);
    if (index.isValid()) {
        view_->setCurrentIndex(index);
        view_->scrollTo(index, QAbstractItemView::PositionAtTop);
    }
}

QString QueuePanel::selectedPath() const
{
    const QModelIndex index = view_->currentIndex().isValid()
        ? view_->currentIndex()
        : view_->selectionModel()->currentIndex();
    const QString path = model_->pathAt(index);
    if (!path.isEmpty())
        return path;
    const QStringList selected = selectedPaths();
    return selected.isEmpty() ? QString() : selected.first();
}

QStringList QueuePanel::selectedPaths() const
{
    QStringList paths;
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const QString path = model_->pathAt(index);
        if (!path.isEmpty())
            paths << path;
    }
    paths.removeDuplicates();
    return paths;
}

QString QueuePanel::currentRowPath() const
{
    return model_->pathAt(view_->currentIndex());
}

void QueuePanel::setCurrentPath(const QString &path)
{
    view_->setCurrentIndex(model_->indexOfPath(path));
}

void QueuePanel::applyColumnToggle(int column, bool shown, bool saved)
{
    if (!saved) {
        const QSignalBlocker blocker(columnActions_[column]);
        columnActions_[column]->setChecked(!view_->isColumnHidden(column));
        return;
    }
    view_->setColumnHidden(column, !shown);
    view_->ensureHeaderWidths();
}

void QueuePanel::resetLayout()
{
    view_->resetColumnWidths(); // retain the chosen visible-column set
    view_->verticalScrollBar()->setValue(0);
}

void QueuePanel::applyPalette(const Palette &palette)
{
    // The playing row is tinted green (the theme's background, 35% toward its
    // GREEN), unlike the selection (ACCENT_DIM). Derived rather than a theme
    // role of its own, as the roles are shared with the Python version.
    const QColor bg = palette[Palette::BG], green = palette[Palette::GREEN];
    const auto mix = [](int a, int b) { return a + qRound((b - a) * 0.35); };
    model_->setPlayingColors(QColor(mix(bg.red(), green.red()), mix(bg.green(), green.green()),
                                    mix(bg.blue(), green.blue())),
                             palette[Palette::FG]);
    model_->setDirectoryColor(palette[Palette::ACCENT]);
    model_->setColors(palette[Palette::FG_DIM], palette[Palette::RED]);
    view_->ensureHeaderWidths();
}
