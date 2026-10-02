#include "queueplayer.h"

#include "config.h"
#include "engine.h"
#include "ignorestore.h"
#include "listeningstats.h"
#include "playbackqueue.h"

#include <QFileInfo>

QueuePlayer::QueuePlayer(Engine &engine, PlaybackQueue &queue, ListeningStats &stats,
                         const IgnoreStore &ignored, const Settings &settings, QObject *parent)
    : QObject(parent), engine_(engine), queue_(queue), stats_(stats), ignored_(ignored), settings_(settings)
{
}

void QueuePlayer::play(const QString &path, double position, bool paused, int subsong,
                       bool preserveBufferedTail, int skipDirection)
{
    if (path.isEmpty())
        return;
    if (ignored_.contains(path)) {
        emit status(tr("This song is ignored - restore it in Settings → Ignored songs"));
        return;
    }
    const int index = queue_.indexOf(path);
    stats_.beforeSongChange(engine_.snapshot());
    engine_.playPath(path, position, paused, subsong, preserveBufferedTail);
    const EngineSnapshot requested = engine_.snapshot();
    stats_.songRequested(path, requested.songGeneration);
    pendingLoad_ = {requested.songGeneration, skipDirection};
    if (skipDirection == 0)
        skipRun_ = 0;   // a song the user picked starts a new count
    queue_.notePlaying(path);
    emit songStarted(path);
    if (index >= 0)
        emit status(tr("Playing %1").arg(QFileInfo(path).fileName()));
}

void QueuePlayer::playQueued(int index, int direction, bool preserveBufferedTail)
{
    play(queue_.at(index).path, 0.0, false, 0, preserveBufferedTail, direction);
}

void QueuePlayer::playSelected(const QString &selected)
{
    if (selected.isEmpty()) {
        emit status(tr("Nothing selected"));
        return;
    }
    play(selected);
}

void QueuePlayer::togglePlay(const QString &selected)
{
    const EngineSnapshot snap = engine_.snapshot();
    if (snap.playing || snap.paused) {
        engine_.togglePause();
        return;
    }
    if (!snap.path.isEmpty() && (snap.ended || snap.failed)) {
        play(snap.path);
        return;
    }
    if (!selected.isEmpty())
        play(selected);
    else if (!queue_.isEmpty())
        playQueued(0, +1);   // the queue's first song, not a pick
    else
        emit status(tr("Nothing to play"));
}

void QueuePlayer::previous(const QString &selected)
{
    const EngineSnapshot snap = engine_.snapshot();
    if (snap.playing && !snap.paused && snap.position > 3.0) {
        engine_.seek(0.0);
        return;
    }
    if (queue_.isEmpty())
        return;
    PlaybackQueue::Step step = queue_.step(snap.path, -1);
    if (step.kind == PlaybackQueue::Step::Unknown)
        step = queue_.step(selected, -1);
    if (step.kind == PlaybackQueue::Step::Play) {
        playQueued(step.index, -1);
    } else if (settings_.loopQueue) {
        playQueued(queue_.size() - 1, -1);
    } else {
        // like Next at the end: no wrap without Repeat queue (a position that
        // isn't known counts as the start; it used to wrap to the last song)
        if (!snap.path.isEmpty())
            engine_.seek(0.0);
        emit status(tr("Start of the queue"));
    }
}

void QueuePlayer::next(const QString &selected)
{
    if (queue_.isEmpty())
        return;
    PlaybackQueue::Step step = queue_.step(engine_.snapshot().path, +1);
    if (step.kind == PlaybackQueue::Step::Unknown)
        step = queue_.step(selected, +1);
    if (step.kind == PlaybackQueue::Step::Unknown)
        step = {PlaybackQueue::Step::Play, 0};   // nothing known: the first song
    if (step.kind == PlaybackQueue::Step::Play)
        playQueued(step.index, +1);
    else if (settings_.loopQueue)
        restart();
    else
        emit status(tr("End of the queue - enable 'Repeat queue' (R) to start over"));
}

void QueuePlayer::restart(bool preserveBufferedTail)
{
    // Repeat the queue; in shuffle mode draw a new order that does not open
    // with the track that just finished.
    const QString current = engine_.snapshot().path;
    const QString finished = queue_.contains(current) ? current : QString();
    if (orderModeFromName(settings_.queueMode) == OrderMode::Shuffle && queue_.size() > 1) {
        emit reshuffleNeeded(finished);
        emit status(tr("Queue finished - new shuffle order (Ctrl+S, R), starting with %1")
                        .arg(queue_.isEmpty() ? QStringLiteral("?")
                                              : queue_.at(0).displayTitle()));
    } else {
        emit status(tr("Queue finished - starting over (R to repeat)"));
    }
    if (!queue_.isEmpty()) {
        // the new order avoids the finished song only before search/filters
        // narrow it: don't open with it again when it is first anyway
        const int start = queue_.size() > 1 && queue_.at(0).path == finished ? 1 : 0;
        playQueued(start, +1, preserveBufferedTail);
    }
}

void QueuePlayer::stop()
{
    stats_.flush(engine_.snapshot());
    engine_.stop();
    emit stopped();
    emit status(tr("Stopped"));
}

void QueuePlayer::songFinished()
{
    stats_.flush(engine_.snapshot());
    const EngineSnapshot snap = engine_.snapshot();
    if (!settings_.autoAdvance || queue_.isEmpty())
        return;
    const PlaybackQueue::Step step = queue_.step(snap.path, +1);
    if (step.kind == PlaybackQueue::Step::Play)
        playQueued(step.index, +1, true);
    else if (step.kind == PlaybackQueue::Step::Unknown)
        return;
    else if (settings_.loopQueue)
        restart(true);
    else
        emit status(tr("Queue finished - enable 'Repeat queue' (R) to start over "
                       "(shuffle then draws new order, Ctrl+S)"));
}

void QueuePlayer::loadDone()
{
    // A file that can't be loaded (deleted after the scan, damaged) used to
    // stop the queue. When the app chose the song itself (auto-advance,
    // Next/Previous, Play on an unselected queue) it skips on in the same
    // direction; a song the user picked directly keeps the error on screen.
    const EngineSnapshot snap = engine_.snapshot();
    if (pendingLoad_.generation == 0 || snap.songGeneration != pendingLoad_.generation || snap.loading)
        return;   // not the load we asked for, or still loading
    const int direction = pendingLoad_.direction;
    pendingLoad_ = {};
    if (!snap.failed || direction == 0 || queue_.isEmpty()) {
        skipRun_ = 0;
        return;
    }
    const QString name = QFileInfo(snap.path).fileName();
    emit logMessage(QStringLiteral("warn"), tr("Skipped %1: %2").arg(name, snap.loadError));
    // at most one pass over the queue: a queue of only broken files stops
    const PlaybackQueue::Step step = queue_.step(snap.path, direction);
    if (++skipRun_ >= queue_.size() || step.kind == PlaybackQueue::Step::Unknown) {
        if (skipRun_ >= queue_.size())
            emit status(tr("No song in the queue could be loaded"));
        skipRun_ = 0;
        return;
    }
    int next = step.index;
    if (step.kind == PlaybackQueue::Step::PastEnd) {
        if (!settings_.loopQueue) {
            skipRun_ = 0;
            emit status(direction > 0 ? tr("Queue finished - %1 could not be loaded").arg(name)
                                      : tr("Start of the queue - %1 could not be loaded").arg(name));
            return;
        }
        next = direction > 0 ? 0 : queue_.size() - 1;
    }
    playQueued(next, direction);
    emit status(tr("Skipped %1 (could not be loaded)").arg(name));
}

QueuePlayer::Successor QueuePlayer::stopIgnored(const EngineSnapshot &snap)
{
    // Mirror PlayerApp.change_ignored: select from the pre-change queue, not
    // the rebuilt queue (where the old index no longer identifies this song).
    const QVector<Track> oldQueue = queue_.tracks();
    Successor next;
    next.ignored = snap.path;
    next.paused = snap.paused || (!snap.playing && !snap.loading);
    int index = -1;
    for (int i = 0; i < oldQueue.size(); ++i)
        if (oldQueue[i].path == snap.path) { index = i; break; }
    auto consider = [&](int begin, int end) {
        for (int i = begin; i < end && next.path.isEmpty(); ++i)
            if (!ignored_.contains(oldQueue[i].path))
                next.path = oldQueue[i].path;
    };
    consider(index + 1, oldQueue.size());
    if (settings_.loopQueue && next.path.isEmpty()) {
        next.reshuffle = orderModeFromName(settings_.queueMode) == OrderMode::Shuffle;
        consider(0, index + 1);
    }
    stats_.flush(engine_.snapshot());
    engine_.pause();
    stop();
    return next;
}

void QueuePlayer::playSuccessor(const Successor &next)
{
    QString path = next.path;
    if (next.reshuffle && !queue_.isEmpty()) {
        // The ignored song has already been excluded. Do not use a first
        // path captured from the previous shuffle and never include it again.
        emit reshuffleNeeded(next.ignored);
        path = queue_.isEmpty() ? QString() : queue_.at(0).path;
    }
    if (!path.isEmpty() && queue_.contains(path))
        play(path, 0.0, next.paused, 0, false, +1);   // the app's choice: skip on if broken
    else
        emit status(tr("Song ignored - no next eligible song in this queue "
                       "(Settings → Ignored songs to restore)"));
}
