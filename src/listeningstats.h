#pragma once
// Listening statistics: a play counts once a requested song has really
// loaded, and the time it is actually heard adds up. Fed with engine
// snapshots only, so it has no UI of its own.
#include "engine.h"
#include "stores.h"

#include <QElapsedTimer>

class ListeningStats {
public:
    explicit ListeningStats(const QString &path = QString());

    StatsStore &store() { return store_; }
    bool save() { return store_.save(); }   // writes only when something changed

    // The setting; turning it off settles the time collected so far.
    void setEnabled(bool enabled, const EngineSnapshot &now);
    // Around a song change: settle the old song before the engine switches,
    // then wait for this generation to load to count its play.
    void beforeSongChange(const EngineSnapshot &now);
    void songRequested(const QString &path, quint64 generation);
    void update(const EngineSnapshot &now);   // every UI tick
    void flush(const EngineSnapshot &now);    // settle the pending play and the time
    void afterReset(const EngineSnapshot &now);   // all records were deleted

private:
    void recordPending(const EngineSnapshot &snap);
    void armIfPlaying(const EngineSnapshot &snap);

    StatsStore store_;
    bool enabled_ = false;
    QElapsedTimer timer_;
    double accum_ = 0.0;              // seconds heard, not yet recorded
    QString path_, title_;            // the song the time belongs to
    QString pendingPath_;             // requested, its play not counted yet
    quint64 pendingGeneration_ = 0;
    quint64 countedGeneration_ = 0;   // a generation is counted once
};
