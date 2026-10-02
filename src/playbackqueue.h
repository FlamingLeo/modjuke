#pragma once
// The play queue: the current source in its play order, and the part of it
// that search and filters leave visible. Plain data with no UI: MainWindow
// decides what to play, this answers "which song comes before or after".
#include "library.h"

#include <QHash>
#include <QStringList>
#include <QVector>

class PlaybackQueue {
public:
    // The source (library folder or playlist, `sourceKey` identifies it) in
    // play order. A different source forgets the playing song's position.
    void setOrdered(QVector<Track> ordered, const QString &sourceKey);
    // Search and filters narrow the ordered source to the visible queue.
    void narrow(const QString &needle, const QueueFilter &filter, const QString &playingPath);

    const QVector<Track> &tracks() const { return tracks_; }
    const Track &at(int index) const { return tracks_[index]; }
    int size() const { return tracks_.size(); }
    bool isEmpty() const { return tracks_.isEmpty(); }
    QStringList paths() const;
    // all visible songs, also those inside a collapsed folder; -1 when hidden
    int indexOf(const QString &path) const { return index_.value(path, -1); }
    bool contains(const QString &path) const { return index_.contains(path); }

    // The playing song's queue position, so its neighbors are still known
    // after it leaves the visible queue (unfavorited, removed, filtered).
    void notePlaying(const QString &path) { playingIndex_ = indexOf(path); }

    // The neighbor of `from` in `direction` (+1 next, -1 previous).
    struct Step {
        enum Kind { Play, PastEnd, Unknown } kind;
        int index = -1;   // Play: the visible song to play
    };
    Step step(const QString &from, int direction) const;

private:
    QString sourceKey_;
    QVector<Track> ordered_;          // the source in play order, before search and filters
    QVector<Track> tracks_;           // the visible queue
    QHash<QString, int> index_;       // path -> index in tracks_
    int playingIndex_ = -1;           // last known index of the playing song
};
