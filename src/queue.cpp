#include "queue.h"

#include <QApplication>
#include <QDrag>
#include <QDataStream>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QIcon>
#include <QStyle>
#include <QTimer>
#include <QEvent>

namespace {
constexpr const char *kMimeType = "application/x-modjuke-rows";

class QueueHeaderView : public QHeaderView {
public:
    explicit QueueHeaderView(QWidget *parent) : QHeaderView(Qt::Horizontal, parent) {}
    // Unlike sectionSizeHint(), this also works for temporarily hidden columns.
    int contentWidth(int column) const { return sectionSizeFromContents(column).width(); }
};
}

QueueModel::QueueModel(QObject *parent) : QAbstractTableModel(parent) {}

QString QueueModel::columnKey(int column)
{
    static const char *const keys[] = {"module", "folder", "length", "format", "channels", "subsongs"};
    return column >= 0 && column < COLUMN_COUNT ? QString::fromLatin1(keys[column]) : QString();
}

QString QueueModel::columnName(int column)
{
    switch (column) {
    case Module: return tr("Module");
    case Folder: return tr("Folder");
    case Length: return tr("Length");
    case Format: return tr("Format");
    case Channels: return tr("Channels");
    case Subsongs: return tr("Subsongs");
    default: return {};
    }
}

void QueueModel::setAccent(const QColor &background, const QColor &foreground)
{
    accentBg_ = background;
    accentFg_ = foreground;
}

void QueueModel::setColors(const QColor &stripe, const QColor &dim, const QColor &red)
{
    stripe_ = stripe;
    dim_ = dim;
    red_ = red;
}

void QueueModel::setRows(const QVector<Track> &queue, bool showDirectoryRows)
{
    beginResetModel();
    queue_ = queue;
    rebuildRows(showDirectoryRows);
    endResetModel();
}

void QueueModel::rebuildRows(bool showDirectoryRows)
{
    rows_.clear(); rows_.reserve(queue_.size());
    rowByPath_.clear(); rowByPath_.reserve(queue_.size());
    auto appendTrack = [this](int i, int depth) {
        if (!rowByPath_.contains(queue_[i].path)) rowByPath_.insert(queue_[i].path, int(rows_.size()));
        rows_.append(Row{TrackRow, i, QString(), QString(), depth});
    };
    if (!showDirectoryRows) {
        for (int i = 0; i < queue_.size(); ++i)
            appendTrack(i, 0);
        return;
    }

    // Keep the directory rows in the same flat model, but retain the complete
    // relative path for each one.  Using only the displayed folder name would
    // make two folders such as "A/Music" and "B/Music" collapse together.
    QStringList openPaths;
    for (int i = 0; i < queue_.size(); ++i) {
        const QStringList parts = queue_[i].relDir.isEmpty()
                                      ? QStringList{}
                                      : queue_[i].relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        int common = 0;
        while (common < parts.size() && common < openPaths.size()
               && openPaths[common].section(QLatin1Char('/'), -1) == parts[common])
            ++common;
        openPaths.resize(common);

        // A collapsed ancestor hides all of its descendants, but remains in
        // openPaths so that the header itself is not duplicated for every file.
        bool hiddenByAncestor = false;
        for (const QString &path : openPaths) {
            if (collapsedDirs_.contains(path)) {
                hiddenByAncestor = true;
                break;
            }
        }
        if (hiddenByAncestor)
            continue;

        for (int d = common; d < parts.size(); ++d) {
            const QString prefix = parts.mid(0, d + 1).join(QLatin1Char('/'));
            const bool collapsed = collapsedDirs_.contains(prefix);
            rows_.append(Row{DirRow, -1, parts[d], prefix, d});
            openPaths << prefix;
            if (collapsed) {
                hiddenByAncestor = true;
                break;
            }
        }
        if (!hiddenByAncestor)
            // Keep files just inside their deepest folder; root-level files
            // remain flush-left while nested files get a small tree indent.
            appendTrack(i, parts.size());
    }
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int QueueModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return COLUMN_COUNT;
}

QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rows_.size())
        return {};
    const Row &row = rows_[index.row()];
    if (row.kind == DirRow) {
        if (role == Qt::DisplayRole && index.column() == Module) {
            const QString arrow = collapsedDirs_.contains(row.dirPath)
                ? QStringLiteral("\u25b8 ") : QStringLiteral("\u25be ");
            return QStringLiteral("%1%2%3")
                .arg(QString(2 * row.depth, QLatin1Char(' ')), arrow, row.dir);
        }
        if (role == Qt::ToolTipRole)
            return collapsedDirs_.contains(row.dirPath)
                ? tr("Click to expand this folder") : tr("Click to collapse this folder");
        if (role == Qt::ForegroundRole)
            return directoryColor_.isValid() ? directoryColor_ : QColor("#5aa9ff");
        if (role == Qt::DecorationRole && index.column() == Module)
            return QApplication::style()->standardIcon(QStyle::SP_DirIcon);
        if (role == KindRole)
            return int(DirRow);
        return {};
    }
    const Track &track = queue_[row.queueIndex];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Module:
            return row.depth > 0
                ? QString(2 * row.depth, QLatin1Char(' ')) + track.name
                : track.name;
        case Folder: return track.relDir.isEmpty() ? QStringLiteral(".") : track.relDir;
        case Length: return track.durationText();
        case Format: return track.fmt.toUpper();
        case Channels: return track.channels > 0 ? QString::number(track.channels) : QString();
        case Subsongs: return track.subsongs > 0 ? QString::number(track.subsongs) : QString();
        }
        return {};
    }
    if (role == Qt::TextAlignmentRole) {
        if (index.column() == Length || index.column() == Channels || index.column() == Subsongs)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole)
        return track.path;
    if (role == PathRole)
        return track.path;
    if (role == KindRole)
        return int(TrackRow);
    if (role == BrokenRole)
        return !track.broken.isEmpty();
    const bool playing = !playingPath_.isEmpty() && track.path == playingPath_;
    if (playing) {
        if (role == Qt::BackgroundRole)
            return accentBg_;
        if (role == Qt::ForegroundRole)
            return accentFg_;
        return {};
    }
    if (role == Qt::ForegroundRole) {
        if (!track.broken.isEmpty())
            return red_;
        if (missing_.contains(track.path))
            return dim_;
    }
    return {};
}

QVariant QueueModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::ToolTipRole)
        return columnName(section);
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case Module: return tr("Module");
        case Folder: return tr("Folder");
        case Length: return tr("Len");
        case Format: return tr("Fmt");
        case Channels: return tr("Ch");
        case Subsongs: return tr("Ss");
        }
    }
    return {};
}

Qt::ItemFlags QueueModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags base = QAbstractTableModel::flags(index);
    if (rows_[index.row()].kind == DirRow)
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    if (reorderFlags_)
        return base | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    return base & ~Qt::ItemIsDropEnabled;
}

QString QueueModel::pathAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= rows_.size())
        return {};
    const Row &row = rows_[index.row()];
    return row.kind == TrackRow ? queue_[row.queueIndex].path : QString();
}

bool QueueModel::isDirectory(const QModelIndex &index) const
{
    return index.isValid() && index.row() >= 0 && index.row() < rows_.size()
        && rows_[index.row()].kind == DirRow;
}

void QueueModel::toggleDirectory(const QModelIndex &index)
{
    if (!isDirectory(index))
        return;
    const QString path = rows_[index.row()].dirPath;
    if (collapsedDirs_.contains(path))
        collapsedDirs_.remove(path);
    else
        collapsedDirs_.insert(path);
    beginResetModel();
    rebuildRows(true);
    endResetModel();
}

int QueueModel::queueIndexAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= rows_.size())
        return -1;
    const Row &row = rows_[index.row()];
    return row.kind == TrackRow ? row.queueIndex : -1;
}

QModelIndex QueueModel::indexOfPath(const QString &path) const
{
    const auto it = rowByPath_.constFind(path);
    return it == rowByPath_.constEnd() ? QModelIndex() : index(it.value(), 0);
}

void QueueModel::setPlayingPath(const QString &path)
{
    if (playingPath_ == path)
        return;
    const QModelIndex old = indexOfPath(playingPath_);
    const QModelIndex now = indexOfPath(path);
    playingPath_ = path;
    if (old.isValid())
        emit dataChanged(index(old.row(), 0), index(old.row(), COLUMN_COUNT - 1));
    if (now.isValid())
        emit dataChanged(index(now.row(), 0), index(now.row(), COLUMN_COUNT - 1));
}

void QueueModel::setMissingPaths(const QSet<QString> &missing)
{
    if (missing_ == missing)
        return;
    missing_ = missing;
    if (!rows_.isEmpty())
        emit dataChanged(index(0, 0), index(rows_.size() - 1, COLUMN_COUNT - 1));
}

// ---- drag & drop -------------------------------------------------------------

QStringList QueueModel::mimeTypes() const
{
    return {QLatin1String(kMimeType)};
}

QMimeData *QueueModel::mimeData(const QModelIndexList &indexes) const
{
    QMimeData *mime = new QMimeData;
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    QList<int> selected;
    for (const QModelIndex &index : indexes) {
        if (index.column() != Module)
            continue;
        const int queueIndex = queueIndexAt(index);
        if (queueIndex >= 0 && !selected.contains(queueIndex))
            selected << queueIndex;
    }
    stream << selected;
    mime->setData(QLatin1String(kMimeType), bytes);
    return mime;
}

bool QueueModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int,
                             const QModelIndex &parent)
{
    if (action != Qt::MoveAction || !data->hasFormat(QLatin1String(kMimeType)))
        return false;
    QList<int> moved;
    {
        QByteArray bytes = data->data(QLatin1String(kMimeType));
        QDataStream stream(&bytes, QIODevice::ReadOnly);
        stream >> moved;
    }
    if (moved.isEmpty())
        return false;

    // Display row -> queue index just before the drop position.
    int insertRow = row;
    if (insertRow < 0 && parent.isValid())
        insertRow = parent.row();
    if (insertRow < 0)
        insertRow = rows_.size();
    int targetQueue = 0;
    for (int i = 0; i < insertRow && i < rows_.size(); ++i) {
        if (rows_[i].kind == TrackRow)
            ++targetQueue;
    }

    QVector<int> movedSorted;
    QSet<int> movedSet;
    for (int queueIndex : moved) {
        if (queueIndex >= 0 && queueIndex < queue_.size() && !movedSet.contains(queueIndex)) {
            movedSet.insert(queueIndex);
            movedSorted << queueIndex;
        }
    }
    std::sort(movedSorted.begin(), movedSorted.end());

    QVector<int> rest;
    for (int i = 0; i < queue_.size(); ++i) {
        if (!movedSet.contains(i))
            rest << i;
    }
    int insertAt = 0;
    for (int i = 0; i < targetQueue; ++i) {
        if (!movedSet.contains(i))
            ++insertAt;
    }
    insertAt = std::clamp(insertAt, 0, int(rest.size()));

    QVector<Track> newQueue;
    for (int i = 0; i < rest.size(); ++i) {
        if (i == insertAt)
            for (int m : movedSorted)
                newQueue << queue_[m];
        newQueue << queue_[rest[i]];
    }
    if (insertAt == rest.size())
        for (int m : movedSorted)
            newQueue << queue_[m];

    const bool showDirs = rows_.size() != queue_.size();
    beginResetModel();
    queue_ = newQueue;
    rebuildRows(showDirs);
    endResetModel();
    QStringList paths;
    for (const Track &track : queue_)
        paths << track.path;
    emit orderEdited(paths);
    return true;
}

bool QueueModel::moveRows(const QModelIndex &sourceParent, int start, int count,
                          const QModelIndex &destinationParent, int destination)
{
    // QAbstractItemModel passes a count, not a last-row index. Reordering is
    // defined only for flat saved-order rows, never directory header rows.
    if (!reorderFlags_ || sourceParent.isValid() || destinationParent.isValid()
        || rows_.size() != queue_.size() || start < 0 || count <= 0
        || start >= queue_.size() || count > queue_.size() - start
        || destination < 0 || destination > queue_.size()
        || (destination >= start && destination <= start + count))
        return false;
    if (!beginMoveRows({}, start, start + count - 1, {}, destination))
        return false;
    const QVector<Track> chunk = queue_.mid(start, count);
    queue_.remove(start, count);
    const int target = destination > start ? destination - count : destination;
    for (int i = 0; i < chunk.size(); ++i)
        queue_.insert(target + i, chunk[i]);
    rebuildRows(false);
    endMoveRows();
    QStringList paths;
    for (const Track &track : queue_)
        paths << track.path;
    emit orderEdited(paths);
    return true;
}

// ---------------------------------------------------------------------------
// QueueTableView
// ---------------------------------------------------------------------------

QueueTableView::QueueTableView(QWidget *parent) : QTableView(parent)
{
    setHorizontalHeader(new QueueHeaderView(this));
    horizontalHeader()->installEventFilter(this);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(22);
    horizontalHeader()->setHighlightSections(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setWordWrap(false);
}

int QueueTableView::minimumColumnWidth(int column) const
{
    if (!model() || column < 0 || column >= model()->columnCount()) return 0;
    int width = static_cast<QueueHeaderView *>(horizontalHeader())->contentWidth(column);
    // Reserve typical metadata values as well as the styled caption. Padding
    // scales with the body font, not a fixed "Ch fits in 34 px" guess.
    QString sample;
    if (column == QueueModel::Channels || column == QueueModel::Subsongs) sample = QStringLiteral("9999");
    else if (column == QueueModel::Length) sample = QStringLiteral("00:00");
    else if (column == QueueModel::Format) sample = QStringLiteral("MPTM");
    if (!sample.isEmpty())
        width = std::max(width, fontMetrics().horizontalAdvance(sample)
                               + 2 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    return width;
}

void QueueTableView::resetColumnWidths()
{
    static constexpr int defaults[QueueModel::COLUMN_COUNT] = {300, 170, 54, 52, 44, 44};
    auto *header = horizontalHeader();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(1);
    for (int c = 0; c < QueueModel::COLUMN_COUNT; ++c) {
        header->setSectionResizeMode(c, QHeaderView::Interactive);
        setColumnWidth(c, std::max(defaults[c], minimumColumnWidth(c)));
    }
    header->setSectionResizeMode(QueueModel::Module, QHeaderView::Stretch);
}

void QueueTableView::ensureHeaderWidths()
{
    if (!model()) return;
    for (int c = QueueModel::Folder; c < model()->columnCount(); ++c) {
        if (!isColumnHidden(c) && columnWidth(c) < minimumColumnWidth(c))
            setColumnWidth(c, minimumColumnWidth(c));
    }
}

void QueueTableView::scheduleWidthUpdate()
{
    // Theme/font propagation can still be in progress during these events.
    if (widthUpdatePending_) return;
    widthUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        widthUpdatePending_ = false;
        ensureHeaderWidths();
        verticalHeader()->setDefaultSectionSize(std::max(22, fontMetrics().height()
            + 2 * style()->pixelMetric(QStyle::PM_FocusFrameVMargin, nullptr, this)));
    });
}

bool QueueTableView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == horizontalHeader() && (event->type() == QEvent::StyleChange
        || event->type() == QEvent::FontChange || event->type() == QEvent::Polish))
        scheduleWidthUpdate();
    return QTableView::eventFilter(watched, event);
}

void QueueTableView::changeEvent(QEvent *event)
{
    QTableView::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        scheduleWidthUpdate();
}

void QueueTableView::setReorderEnabled(bool enabled)
{
    reorderEnabled_ = enabled;
    auto *model = qobject_cast<QueueModel *>(this->model());
    // enable internal move drag & drop (the model exposes reorderFlags_)
    setDragEnabled(enabled);
    setDragDropMode(enabled ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
    setDefaultDropAction(enabled ? Qt::MoveAction : Qt::IgnoreAction);
    if (model)
        model->reorderFlags_ = enabled;
}

void QueueTableView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QModelIndex hit = indexAt(event->pos());
    if (hit.isValid()) {
        if (auto *model = qobject_cast<QueueModel *>(this->model())) {
            const QString path = model->pathAt(hit);
            if (!path.isEmpty()) {
                emit activatedPath(path);
                return;
            }
        }
    }
    QTableView::mouseDoubleClickEvent(event);
}

void QueueTableView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        // unused; Enter activation below
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const QModelIndex current = currentIndex();
        if (auto *model = qobject_cast<QueueModel *>(this->model())) {
            const QModelIndex use = current.isValid() ? current : currentIndexAtSelected();
            const QString path = model->pathAt(use);
            if (!path.isEmpty()) {
                emit activatedPath(path);
                event->accept();
                return;
            }
        }
    }
    QTableView::keyPressEvent(event);
}

void QueueTableView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex hit = indexAt(event->pos());
    QString path;
    if (auto *model = qobject_cast<QueueModel *>(this->model()))
        path = model->pathAt(hit);
    if (path.isEmpty()) {
        event->accept();
        return; // directory headers and empty space have no song menu
    }
    emit contextMenuFor(event->globalPos(), path);
}

QModelIndex QueueTableView::currentIndexAtSelected()
{
    const QList<QModelIndex> selected = selectionModel()->selectedRows();
    return selected.isEmpty() ? currentIndex() : selected.first();
}

void QueueTableView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QModelIndex hit = indexAt(event->pos());
        if (auto *model = qobject_cast<QueueModel *>(this->model()); model && model->isDirectory(hit)) {
            model->toggleDirectory(hit);
            event->accept();
            return;
        }
        if (reorderEnabled_)
            dragStart_ = event->pos();
    }
    QTableView::mousePressEvent(event);
}

void QueueTableView::mouseMoveEvent(QMouseEvent *event)
{
    if (reorderEnabled_ && (event->buttons() & Qt::LeftButton)
        && (event->pos() - dragStart_).manhattanLength() >= QApplication::startDragDistance()) {
        startRowDrag();
        return;
    }
    QTableView::mouseMoveEvent(event);
}

void QueueTableView::startRowDrag()
{
    const QModelIndexList selected = selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return;
    QMimeData *mime = model()->mimeData(selected);
    if (!mime)
        return;
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::MoveAction);
}
