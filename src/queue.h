// The queue table: a flat display of the visible queue with optional folder
// header rows (matching the "by directory" tree of the Tk version).
#pragma once

#include "library.h"

#include <QAbstractTableModel>
#include <QAbstractItemView>
#include <QTableView>
#include <QColor>
#include <QSet>
#include <QVector>

class QueueModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Module = 0, Folder, Length, Format, Channels, Subsongs, COLUMN_COUNT };
    enum Role { PathRole = Qt::UserRole, KindRole, BrokenRole };
    enum Kind { TrackRow, DirRow };

    explicit QueueModel(QObject *parent = nullptr);
    static QString columnKey(int column);     // stable Qt preference ID
    static QString columnName(int column);    // full, translated menu/tooltip name

    void setRows(const QVector<Track> &queue, bool showDirectoryRows);
    const QVector<Track> &queue() const { return queue_; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // drag & drop reorder (saved-order playlists)
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    bool moveRows(const QModelIndex &parent, int start, int count,
                  const QModelIndex &destParent, int destinationRow) override;

    QString pathAt(const QModelIndex &index) const;
    int queueIndexAt(const QModelIndex &index) const;
    QModelIndex indexOfPath(const QString &path) const;
    bool isDirectory(const QModelIndex &index) const;
    void toggleDirectory(const QModelIndex &index);

    void setReorderFlags(bool on) { reorderFlags_ = on; }
    bool reorderFlags_ = false;

    void setPlayingPath(const QString &path);
    void setMissingPaths(const QSet<QString> &missing);
    void setAccent(const QColor &background, const QColor &foreground);
    void setColors(const QColor &stripe, const QColor &dim, const QColor &red);
    void setDirectoryColor(const QColor &color) { directoryColor_ = color; }

signals:
    void orderEdited(const QStringList &newPathOrder);   // after a drop

private:
    struct Row {
        Kind kind = TrackRow;
        int queueIndex = -1;          // TrackRow
        QString dir;                  // DirRow label
        QString dirPath;              // DirRow path relative to the source root
        int depth = 0;                // visual tree depth; files are children of their deepest folder
    };

    QVector<Row> rows_;
    QHash<QString, int> rowByPath_; // first visible row; rebuilt with the model
    QSet<QString> collapsedDirs_;   // relative directory paths, only used in directory order
    QVector<Track> queue_;
    QSet<QString> missing_;
    QString playingPath_;
    QColor accentBg_, accentFg_, stripe_, dim_, red_, directoryColor_;
    void rebuildRows(bool showDirectoryRows);
};

// QTableView with row drag (saved-order) + double-click / Enter helpers.
class QueueTableView : public QTableView {
    Q_OBJECT
public:
    explicit QueueTableView(QWidget *parent = nullptr);
    void setReorderEnabled(bool enabled);
    void resetColumnWidths();
    void ensureHeaderWidths();
    int minimumColumnWidth(int column) const;

signals:
    void activatedPath(const QString &path);
    void contextMenuFor(const QPoint &globalPos, const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    QModelIndex currentIndexAtSelected();

private:
    void startRowDrag();
    bool widthUpdatePending_ = false;
    void scheduleWidthUpdate();
    bool reorderEnabled_ = false;
    QPoint dragStart_;
};
