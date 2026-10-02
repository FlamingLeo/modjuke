#include "queuebuilder.h"

#include "casefold.h"
#include "musiclibrary.h"
#include "shuffles.h"
#include "stores.h"

#include <QFileInfo>
#include <QRandomGenerator>

namespace {

quint32 freshShuffleSeed(qint64 previous)
{
    quint32 seed;
    do { seed = QRandomGenerator::global()->bounded(1u, 2000000000u); }
    while (seed == quint32(previous));
    return seed;
}

}   // namespace

QueueBuilder::QueueBuilder(Settings &settings, const MusicLibrary &library, PlaylistStore &playlists,
                           const IgnoreStore &ignored, ShuffleStore &shuffles, QObject *parent)
    : QObject(parent), settings_(settings), library_(library), playlists_(playlists),
      ignored_(ignored), shuffles_(shuffles)
{
}

const Playlist *QueueBuilder::sourcePlaylist() const
{
    return settings_.queueSource.isEmpty() ? nullptr : playlists_.get(settings_.queueSource);
}

QVector<Track> QueueBuilder::sourceTracks() const
{
    QVector<Track> base;
    if (const Playlist *playlist = sourcePlaylist()) {
        for (const QString &path : playlist->paths) {
            if (ignored_.contains(path))
                continue;
            if (const Track *known = library_.find(path)) {
                base.append(*known);
                continue;
            }
            Track track;                                // external playlist entry
            track.path = path;
            const QFileInfo info(path);
            track.name = info.fileName();
            track.relDir =
                info.absolutePath() == library_.root() ? QString() : info.absolutePath();
            track.ext = info.suffix().toLower();
            track.fmt = track.ext;
            track.size = info.size();
            if (!info.exists())
                track.broken = QStringLiteral("missing");
            base.append(track);
        }
        return base;
    }
    base.reserve(library_.tracks().size());
    for (const Track &track : library_.tracks()) {
        if (!ignored_.contains(track.path))
            base.append(track);
    }
    return base;
}

QString QueueBuilder::sourceKey() const
{
    return settings_.queueSource.isEmpty() ? ShuffleStore::libraryKey(library_.root())
                                           : ShuffleStore::playlistKey(settings_.queueSource);
}

bool QueueBuilder::reorderable() const
{
    return mode() == OrderMode::Saved && sourcePlaylist();
}

QueueFilter QueueBuilder::filter() const
{
    QueueFilter filter;
    filter.formats = settings_.filterFormats;
    filter.minSeconds = settings_.filterMin;
    filter.maxSeconds = settings_.filterMax;
    filter.hideBroken = settings_.filterHideBroken;
    return filter;
}

void QueueBuilder::setFilter(const QueueFilter &filter)
{
    settings_.filterFormats = filter.formats;
    settings_.filterMin = filter.minSeconds;
    settings_.filterMax = filter.maxSeconds;
    settings_.filterHideBroken = filter.hideBroken;
    emit settingsChanged();
}

QVector<Track> QueueBuilder::ordered(const QString &anchor)
{
    const Playlist *playlist = sourcePlaylist();
    QVector<Track> base = sourceTracks();
    const OrderMode order = mode();
    if (order == OrderMode::Shuffle)
        return applyPathOrder(base, shufflePlan(base, anchor));
    if (order == OrderMode::Saved && playlist)
        return applyPathOrder(base, playlist->paths);
    if (order == OrderMode::Saved)
        return base;          // "playlist" order without a playlist: leave as scanned
    return orderTracks(base, order, quint32(settings_.shuffleSeed));
}

QStringList QueueBuilder::shufflePlan(const QVector<Track> &source, const QString &anchor)
{
    // The drawn order of this source: drawn only when it never had one. Songs
    // added since (a rescan, new favorites) get random places in it; they
    // used to be appended in alphabetical order.
    const QString key = sourceKey();
    QStringList plan = shuffles_.get(key);
    if (plan.isEmpty())
        return source.isEmpty() ? plan : drawPlan(0, anchor, QString());
    const QSet<QString> planned(plan.cbegin(), plan.cend());
    QStringList fresh;
    for (const Track &track : source) {
        if (!planned.contains(track.path))
            fresh << track.path;
    }
    if (fresh.isEmpty())
        return plan;
    if (fresh.size() > plan.size())
        return drawPlan(0, anchor, QString());   // mostly new: a fresh order fits better
    plan = ShuffleStore::mergeIntoPlan(plan, fresh, QRandomGenerator::global());
    putPlan(key, plan);
    return plan;
}

QStringList QueueBuilder::drawPlan(quint32 seed, const QString &first, const QString &avoidFirst)
{
    // seed 0: the current one (a random one if there is none)
    if (!seed) {
        seed = quint32(settings_.shuffleSeed)
            ? quint32(settings_.shuffleSeed)
            : quint32(QRandomGenerator::global()->bounded(1, int(2e9)));
    }
    settings_.shuffleSeed = seed;
    const QVector<Track> plan = orderTracks(sourceTracks(), OrderMode::Shuffle, seed, first,
                                            avoidFirst);
    QStringList paths;
    paths.reserve(plan.size());
    for (const Track &track : plan)
        paths << track.path;
    putPlan(sourceKey(), paths);
    emit settingsChanged();          // the seed travels with the plan
    return paths;
}

void QueueBuilder::putPlan(const QString &key, const QStringList &plan)
{
    if (!shuffles_.put(key, plan) && !shuffles_.error.isEmpty())
        emit warning(shuffles_.error);
}

void QueueBuilder::drawShuffle(const QString &first, const QString &avoidFirst)
{
    drawPlan(freshShuffleSeed(settings_.shuffleSeed), first, avoidFirst);
}

void QueueBuilder::reshuffle(const QString &first)
{
    settings_.queueMode = QStringLiteral("shuffle");
    drawShuffle(first);
}

void QueueBuilder::setSource(const QString &playlist, bool savedOrder)
{
    settings_.queueSource = playlist;
    if (savedOrder)
        settings_.queueMode = QStringLiteral("playlist");
    else if (playlist.isEmpty() && settings_.queueMode == QLatin1String("playlist"))
        settings_.queueMode = QStringLiteral("by directory");
    emit settingsChanged();
}

void QueueBuilder::setMode(const QString &mode)
{
    // changing the ordering never redraws a saved shuffle
    settings_.queueMode = mode;
    emit settingsChanged();
}

void QueueBuilder::dropMissingSource()
{
    // same startup normalization as ui.py __init__: a restored playlist
    // source that vanished falls back to the library (its dangling shuffle
    // order and legacy list are dropped), and "playlist" ordering without a
    // playlist resets to "by directory"
    bool changed = false;
    // (an unreadable playlists file isn't "the playlist is gone")
    if (!settings_.queueSource.isEmpty() && !playlists_.get(settings_.queueSource)
        && !playlists_.readError()) {
        settings_.queueSource.clear();
        settings_.shufflePaths.clear();
        changed = true;
    }
    if (settings_.queueMode == QLatin1String("playlist") && settings_.queueSource.isEmpty()) {
        settings_.queueMode = QStringLiteral("by directory");
        changed = true;
    }
    if (changed)
        emit settingsChanged();
}

void QueueBuilder::migrateLegacyShuffle()
{
    const QStringList legacy = settings_.shufflePaths;
    if (legacy.isEmpty())
        return;
    const QString key =
        settings_.queueSource.isEmpty() ? ShuffleStore::libraryKey(settings_.lastDirectory)
                                        : ShuffleStore::playlistKey(settings_.queueSource);
    if (!shuffles_.contains(key) && !shuffles_.put(key, legacy))
        return;   // keep the old copy until it can be saved
    settings_.shufflePaths.clear();
    emit settingsChanged();
}

bool QueueBuilder::playlistRenamed(const QString &oldName, const QString &newName)
{
    shuffles_.rename(ShuffleStore::playlistKey(oldName), ShuffleStore::playlistKey(newName));
    if (caseFold(settings_.queueSource) != caseFold(oldName))
        return false;
    setSource(newName);
    return true;
}

bool QueueBuilder::playlistRemoved(const QString &name)
{
    shuffles_.remove(ShuffleStore::playlistKey(name));
    if (caseFold(settings_.queueSource) != caseFold(name))
        return false;
    setSource(QString());
    return true;
}

QStringList QueueBuilder::mergeOrder(const QStringList &saved, const QSet<QString> &visible,
                                     const QStringList &newOrder)
{
    // (hidden entries used to move to the end)
    QStringList result;
    result.reserve(saved.size());
    int next = 0;
    for (const QString &path : saved) {
        if (visible.contains(path) && next < newOrder.size())
            result << newOrder[next++];
        else
            result << path;
    }
    while (next < newOrder.size())
        result << newOrder[next++];
    return result;
}

bool QueueBuilder::saveOrder(const QStringList &visible, const QStringList &newOrder)
{
    const Playlist *playlist = sourcePlaylist();
    if (!playlist)
        return false;
    return playlists_.replace(settings_.queueSource,
                              mergeOrder(playlist->paths, QSet<QString>(visible.cbegin(), visible.cend()),
                                         newOrder));
}
