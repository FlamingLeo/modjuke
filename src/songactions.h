#pragma once
// What the queue offers for its songs besides playing them: favorites,
// adding to and removing from playlists, a new playlist from songs, saving
// the queue as a playlist, and the queue's context menu with all of these.
// Asks the user where needed (in `window`); the window refreshes the source
// list and the queue, and does what needs playback (signals).
#include <QObject>
#include <QString>
#include <QStringList>

class MusicLibrary;
class IgnoreStore;
class PlaylistStore;
class QPoint;
class QueueBuilder;
class QWidget;
struct Settings;

class SongActions : public QObject {
    Q_OBJECT
public:
    SongActions(QWidget *window, const Settings &settings, PlaylistStore &playlists,
                const IgnoreStore &ignored, const MusicLibrary &library, QueueBuilder &builder);

    bool toggleFavorite(const QString &path);           // true: now a favorite
    void showAddMenu(const QStringList &paths);         // the "Add to playlist..." button
    void createPlaylist(const QStringList &paths);      // asks for the name
    void removeFromSource(const QStringList &paths);    // from the playlist the queue shows
    void saveQueueAs(const QString &name, const QStringList &paths);   // asks before replacing
    // `selected`: the queue's selection, which "Add to playlist" adds instead of `path`
    void showContextMenu(const QPoint &globalPos, const QString &path, const QStringList &selected);
    void reveal(const QString &path);                   // "Show in folder"

signals:
    void playlistsChanged();                            // the source list shows their sizes
    void queueChanged();                                // the source changed: rebuild the queue
    void status(const QString &text);
    void playRequested(const QString &path);
    void ignoreToggled(const QString &path);            // context menu: (un-)ignore

private:
    QWidget *window_;
    const Settings &settings_;
    PlaylistStore &playlists_;
    const IgnoreStore &ignored_;
    const MusicLibrary &library_;
    QueueBuilder &builder_;
};
