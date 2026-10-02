#pragma once
// The queue's source in play order: the library folder or a playlist, minus
// ignored songs, ordered as the settings say (source, order, seed). Keeps the
// shuffle orders (drawn once per source, kept in shuffles.json; port of the
// shuffle parts of ui.py) and saves a saved-order playlist reordered in the
// queue. No UI: the window shows the messages and saves the settings when
// they change (settingsChanged).
#include "config.h"
#include "library.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class IgnoreStore;
class MusicLibrary;
class PlaylistStore;
class ShuffleStore;
struct Playlist;

class QueueBuilder : public QObject {
    Q_OBJECT
public:
    QueueBuilder(Settings &settings, const MusicLibrary &library, PlaylistStore &playlists,
                 const IgnoreStore &ignored, ShuffleStore &shuffles, QObject *parent = nullptr);

    QVector<Track> sourceTracks() const;    // the source, unordered and unfiltered
    QString sourceKey() const;              // its key in shuffles.json
    OrderMode mode() const { return orderModeFromName(settings_.queueMode); }
    bool reorderable() const;               // the saved order of a playlist that exists
    QueueFilter filter() const;
    void setFilter(const QueueFilter &filter);

    // The source in play order. Shuffle brings back the source's drawn order,
    // with songs added since at random places; only a source that never had
    // one gets a fresh order, opening with `anchor`.
    QVector<Track> ordered(const QString &anchor);
    // A new shuffle order with a new seed: `first` opens it, `avoidFirst` doesn't.
    void drawShuffle(const QString &first, const QString &avoidFirst = QString());
    void reshuffle(const QString &first);   // "Shuffle now": switches to shuffle, too

    // "" is the library. Saved order exists only for playlists: the library
    // falls back to By directory, a new or imported list opens in it.
    void setSource(const QString &playlist, bool savedOrder = false);
    void setMode(const QString &mode);
    // At startup: a restored playlist source that is gone means the library.
    void dropMissingSource();
    void migrateLegacyShuffle();            // settings.shuffle_paths -> shuffles.json
    // A playlist was renamed or deleted: its shuffle order follows it. True
    // when it was the source (which then follows, too).
    bool playlistRenamed(const QString &oldName, const QString &newName);
    bool playlistRemoved(const QString &name);

    // The visible songs `visible` of the saved-order playlist were reordered
    // to `newOrder`; saves the playlist (false: playlists.error says why).
    bool saveOrder(const QStringList &visible, const QStringList &newOrder);
    // The reordered songs fill their own slots in `saved`; songs hidden by
    // search, filters or the ignore list stay where they are.
    static QStringList mergeOrder(const QStringList &saved, const QSet<QString> &visible,
                                  const QStringList &newOrder);

signals:
    void settingsChanged();                 // to be saved
    void warning(const QString &text);      // for the log

private:
    const Playlist *sourcePlaylist() const;
    QStringList shufflePlan(const QVector<Track> &source, const QString &anchor);
    QStringList drawPlan(quint32 seed, const QString &first, const QString &avoidFirst);
    void putPlan(const QString &key, const QStringList &plan);

    Settings &settings_;
    const MusicLibrary &library_;
    PlaylistStore &playlists_;
    const IgnoreStore &ignored_;
    ShuffleStore &shuffles_;
};
