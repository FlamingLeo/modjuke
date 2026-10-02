#pragma once
// The queue side of the player tab: the folder row (Open folder, Rescan,
// Analyze, the folder and its track count), the queue controls row (source,
// order, Shuffle now, Playlists, Filter, search) and the pane with the
// playlist buttons and the queue table. It shows what the window gives it;
// what its controls do is up to the window (signals).
//
// The two rows span the whole window above its pages and hide with the
// Tracker tab, so they are created in `window`, which places them; the panel
// itself is the table pane, placed by the window, too.
#include "library.h"
#include "theme.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class ElidedLabel;
class PlaylistStore;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QueueModel;
class QueueTableView;

class QueuePanel : public QWidget {
    Q_OBJECT
public:
    QueuePanel(const QStringList &hiddenColumns, QWidget *window);

    QWidget *folderRow() const { return folderRow_; }
    QWidget *controlsRow() const { return controlsRow_; }
    void setRowsVisible(bool visible);

    // ---- folder row ----
    QString folder() const;
    void setFolder(const QString &root);
    void setTrackCount(int tracks, int folders);
    void showAnalysisProgress(int done, int total);
    void showAnalysisIdle();

    // ---- queue controls ----
    void setSources(const PlaylistStore &playlists, const QString &current);
    // "Saved order" and Remove from playlist are for playlists only
    void setSourceKind(bool playlist, const QString &mode);
    void setShuffleEnabled(bool enabled);
    QString searchText() const;
    void focusSearch();
    void clearSearch();

    // ---- the table ----
    void showQueue(const QVector<Track> &queue, bool directoryRows, const QString &playingPath,
                   bool reorderable);
    void setFilterText(const QString &text);
    void setPlayingPath(const QString &path);
    // Scrolls to the playing row; it becomes the current row only when
    // nothing is selected (actions like Remove from playlist use the selection).
    void showPlaying(const QString &path);
    void revealPath(const QString &path);   // current row, at the top
    QString selectedPath() const;           // caret, or else the first selected
    QStringList selectedPaths() const;
    QString currentRowPath() const;
    void setCurrentPath(const QString &path);
    // after columnToggled: the change was saved (or, false, it wasn't)
    void applyColumnToggle(int column, bool shown, bool saved);
    void resetLayout();                     // column widths and scrolling
    void applyPalette(const Palette &palette);

signals:
    void openFolderClicked();
    void rescanClicked();
    void analyzeClicked();
    void sourceChosen(const QString &playlist);   // "" = the library
    void orderChosen(const QString &mode);
    void shuffleClicked();
    void playlistsClicked();
    void filterClicked();
    void searchChanged();
    void addToPlaylistClicked();
    void removeFromPlaylistClicked();
    void songActivated(const QString &path);
    void contextMenuRequested(const QPoint &globalPos, const QString &path);
    void orderEdited(const QStringList &newPathOrder);   // a drop in a saved-order playlist
    void columnToggled(int column, bool shown);

private:
    void buildFolderRow(QWidget *window);
    void buildControlsRow(QWidget *window);
    void buildTable(const QStringList &hiddenColumns);

    QWidget *folderRow_ = nullptr;
    QWidget *controlsRow_ = nullptr;
    QPushButton *analyzeButton_ = nullptr;
    QLineEdit *dirField_ = nullptr;
    QLabel *countLabel_ = nullptr;
    QComboBox *sourceCombo_ = nullptr;
    QComboBox *orderCombo_ = nullptr;
    QPushButton *shuffleBtn_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    ElidedLabel *filterLabel_ = nullptr;
    QPushButton *removeFromPlaylistBtn_ = nullptr;
    QueueModel *model_ = nullptr;
    QueueTableView *view_ = nullptr;
    QVector<QAction *> columnActions_;      // by column; Module has none
};
