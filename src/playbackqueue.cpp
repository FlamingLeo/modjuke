#include "playbackqueue.h"

void PlaybackQueue::setOrdered(QVector<Track> ordered, const QString &sourceKey)
{
    // a position in another source says nothing about this one (Next used to
    // jump to the same index in the new source)
    if (sourceKey != sourceKey_)
        playingIndex_ = -1;
    sourceKey_ = sourceKey;
    ordered_ = std::move(ordered);
}

void PlaybackQueue::narrow(const QString &needle, const QueueFilter &filter, const QString &playingPath)
{
    tracks_ = filter.select(needle.trimmed().isEmpty() ? ordered_ : searchFilter(ordered_, needle));
    index_.clear();
    index_.reserve(tracks_.size());
    for (int i = 0; i < tracks_.size(); ++i)
        index_.insert(tracks_[i].path, i);
    if (index_.contains(playingPath))
        playingIndex_ = index_.value(playingPath);
}

QStringList PlaybackQueue::paths() const
{
    QStringList out;
    out.reserve(tracks_.size());
    for (const Track &track : tracks_)
        out << track.path;
    return out;
}

PlaybackQueue::Step PlaybackQueue::step(const QString &from, int direction) const
{
    // Where the neighbor sits: a visible index, -1 / size() past either end.
    auto result = [this](int target) {
        if (target < 0 || target >= tracks_.size())
            return Step{Step::PastEnd};
        return Step{Step::Play, target};
    };
    if (from.isEmpty())
        return {Step::Unknown};
    const int here = indexOf(from);
    if (here >= 0)
        return result(here + direction);
    // Not visible (search, filter, unfavorited, removed): the nearest visible
    // song in that direction from its place in the unfiltered order ...
    for (int k = 0; k < ordered_.size(); ++k) {
        if (ordered_[k].path != from)
            continue;
        for (int j = k + direction; j >= 0 && j < ordered_.size(); j += direction) {
            const int q = indexOf(ordered_[j].path);
            if (q >= 0)
                return result(q);
        }
        return {Step::PastEnd};
    }
    // ... or, gone from the source, its last known position: its successor
    // moved up into that slot
    if (playingIndex_ >= 0)
        return result(direction > 0 ? std::min(playingIndex_, int(tracks_.size())) : playingIndex_ - 1);
    return {Step::Unknown};
}
