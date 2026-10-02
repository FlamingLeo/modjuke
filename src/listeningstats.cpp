#include "listeningstats.h"

ListeningStats::ListeningStats(const QString &path) : store_(path)
{
    timer_.start();
}

void ListeningStats::setEnabled(bool enabled, const EngineSnapshot &now)
{
    if (enabled == enabled_)
        return;
    if (!enabled) {
        // Settle time collected under the old setting before turning recording
        // off; otherwise it would remain in the accumulator and be attributed
        // to a later session if stats are enabled again.
        flush(now);
        store_.save();
    }
    enabled_ = enabled;
    timer_.restart();
    if (enabled_) {
        armIfPlaying(now);
    } else {
        accum_ = 0.0;
        pendingPath_.clear();
        pendingGeneration_ = 0;
    }
}

void ListeningStats::beforeSongChange(const EngineSnapshot &now)
{
    // A manual track change can happen before the next UI tick (including a
    // restart of the same path), so settle the previous interval now rather
    // than attributing it to the new playback generation.
    if (!enabled_)
        return;
    flush(now);
    timer_.restart();
}

void ListeningStats::songRequested(const QString &path, quint64 generation)
{
    // Count a play only after the asynchronous load succeeds. Keeping the
    // generation lets a quick A→B change discard A rather than recording a
    // failed or superseded load, while a paused session is counted on resume.
    pendingPath_ = enabled_ ? path : QString();
    pendingGeneration_ = enabled_ ? generation : 0;
}

void ListeningStats::update(const EngineSnapshot &now)
{
    // The engine loads asynchronously, so a play counts only for the
    // generation songRequested() named, once it is really loaded. The old
    // implementation only recorded elapsed seconds with play=false, which
    // made every play count stay at zero.
    if (path_ != now.path) {
        flush(now);
        path_ = now.path;
        title_ = now.info.title;
        timer_.restart();
        if (now.path.isEmpty()) {
            pendingPath_.clear();
            pendingGeneration_ = 0;
        }
    } else if (!now.info.title.isEmpty()) {
        title_ = now.info.title;
    }

    // EngineSnapshot::ended is a terminal state in the Qt renderer (it stays
    // true until the next load), so it must not be treated as active time.
    const bool active = enabled_ && now.loaded && !now.loading && !now.failed && !now.paused
                        && now.playing;
    // A very short module can reach ended between two UI ticks. It still counts
    // as a play, but ended must not contribute an endless stream of seconds.
    recordPending(now);
    if (!active) {
        timer_.restart();
        return;
    }
    const double elapsed = timer_.restart() / 1000.0;
    // Do not turn a blocked UI or a suspended machine into fake listening
    // time. Normal refresh intervals are far below this two-second cap.
    if (elapsed >= 0.0 && elapsed <= 2.0)
        accum_ += elapsed;
    if (accum_ >= 15.0) {
        flush(now);
        store_.save();
    }
}

void ListeningStats::flush(const EngineSnapshot &now)
{
    // Also settle a pending play here. This covers very short modules and
    // track changes that arrive before the next periodic UI tick.
    recordPending(now);
    if (accum_ > 0.5 && !path_.isEmpty() && enabled_)
        store_.record(path_, accum_, false, title_);
    accum_ = 0.0;
}

void ListeningStats::afterReset(const EngineSnapshot &now)
{
    accum_ = 0.0;
    timer_.restart();
    countedGeneration_ = 0;
    if (enabled_)
        armIfPlaying(now);
}

void ListeningStats::recordPending(const EngineSnapshot &snap)
{
    if (!enabled_ || !snap.loaded || snap.loading || snap.failed || snap.paused
        || snap.path.isEmpty() || (!snap.playing && !snap.ended)
        || pendingPath_ != snap.path || pendingGeneration_ != snap.songGeneration
        || countedGeneration_ == snap.songGeneration)
        return;
    if (store_.record(snap.path, 0.0, true, snap.info.title)) {
        countedGeneration_ = snap.songGeneration;
        pendingPath_.clear();
        pendingGeneration_ = 0;
    }
}

void ListeningStats::armIfPlaying(const EngineSnapshot &snap)
{
    // the song that is playing when recording starts counts too
    if (snap.loaded && !snap.loading && !snap.failed && !snap.paused && snap.playing) {
        pendingPath_ = snap.path;
        pendingGeneration_ = snap.songGeneration;
    }
}
