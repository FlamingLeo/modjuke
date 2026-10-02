#include "library.h"

#include <sys/stat.h>
#ifdef Q_OS_UNIX
#include <dirent.h>
#endif

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <random>

// ---------------------------------------------------------------------------
QString formatTime(double seconds)
{
    if (seconds < 0.0)
        return QStringLiteral("--:--");
    if (seconds >= 1e18)
        return QStringLiteral("\u221e");
    // 64-bit and clamped: a damaged cache value (1e15) overflowed int
    const qint64 total = qint64(std::min(seconds, 1e15));
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

namespace {

// Names that aren't valid UTF-8 (old Latin-1/CP437 module archives) never
// appear in QDir listings: Qt drops what it can't decode. Count them from the
// raw directory so the scan can say why files are missing.
int undecodableEntries(const QString &dirPath, const QSet<QString> &wanted)
{
#ifdef Q_OS_UNIX
    DIR *d = opendir(QFile::encodeName(dirPath).constData());
    if (!d)
        return 0;
    int count = 0;
    while (const dirent *e = readdir(d)) {
        const QByteArray raw(e->d_name);
        if (raw == "." || raw == ".." || QFile::encodeName(QFile::decodeName(raw)) == raw)
            continue;
        const int dot = raw.lastIndexOf('.');
        const QString ext = dot >= 0 ? QString::fromLatin1(raw.mid(dot + 1)).toLower() : QString();
        if (e->d_type == DT_DIR || wanted.contains(ext))
            ++count;
    }
    closedir(d);
    return count;
#else
    Q_UNUSED(dirPath);
    Q_UNUSED(wanted);
    return 0;
#endif
}

// Build once per field, not inside O(n log n) comparisons. Parts refer to
// offsets in one folded string: no regex captures or per-comparison allocation.
struct NaturalKey {
    struct Part { qsizetype start, size; bool number; };
    QString text;
    QVector<Part> parts;
    explicit NaturalKey(const QString &value = {}) : text(value.toLower()) {
        auto digit = [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); };
        for (qsizetype begin = 0; begin < text.size();) {
            const bool number = digit(text[begin]);
            qsizetype end = begin + 1;
            while (end < text.size() && digit(text[end]) == number) ++end;
            qsizetype start = begin;
            if (number) while (start + 1 < end && text[start] == QLatin1Char('0')) ++start;
            parts.append({start, end - start, number});
            begin = end;
        }
    }
};

int compareNatural(const NaturalKey &a, const NaturalKey &b)
{
    const auto count = std::min(a.parts.size(), b.parts.size());
    for (qsizetype i = 0; i < count; ++i) {
        const auto &l = a.parts[i], &r = b.parts[i];
        if (l.number != r.number) return l.number ? -1 : 1;
        if (l.number && l.size != r.size) return l.size < r.size ? -1 : 1;
        const int cmp = QStringView(a.text).mid(l.start, l.size).compare(QStringView(b.text).mid(r.start, r.size));
        if (cmp) return cmp;
    }
    return (a.parts.size() > b.parts.size()) - (a.parts.size() < b.parts.size());
}

QVector<Track> sortedNatural(const QVector<Track> &tracks, bool byDirectory)
{
    struct Keys { NaturalKey name, directory; QVector<NaturalKey> parts; QString path; };
    QVector<Keys> keys; keys.reserve(tracks.size());
    QVector<int> order; order.reserve(tracks.size());
    for (const Track &track : tracks) {
        Keys k{NaturalKey(track.name), NaturalKey(byDirectory ? QString() : track.relDir), {}, track.path.toLower()};
        if (byDirectory) {
            const auto parts = track.relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            k.parts.reserve(parts.size());
            for (const auto &part : parts) k.parts.append(NaturalKey(part));
        }
        order.append(int(keys.size())); keys.append(std::move(k));
    }
    // Sort compact indices, not Track objects and their many implicitly shared fields.
    std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
        const auto &a = keys[left], &b = keys[right];
        if (byDirectory) {
            for (qsizetype i = 0, n = std::min(a.parts.size(), b.parts.size()); i < n; ++i) {
                const int cmp = compareNatural(a.parts[i], b.parts[i]);
                if (cmp) return cmp < 0;
            }
            if (a.parts.size() != b.parts.size()) return a.parts.size() < b.parts.size();
        }
        int cmp = compareNatural(a.name, b.name);
        if (cmp) return cmp < 0;
        if (!byDirectory) { cmp = compareNatural(a.directory, b.directory); if (cmp) return cmp < 0; }
        return a.path < b.path;
    });
    QVector<Track> result; result.reserve(tracks.size());
    for (int i : order) result.append(tracks[i]);
    return result;
}

QVector<Track> sortedAlphabetical(const QVector<Track> &tracks) { return sortedNatural(tracks, false); }

const QStringList &skipDirs()
{
    static const QStringList skip = {
        QStringLiteral("$RECYCLE.BIN"), QStringLiteral("System Volume Information"),
        QStringLiteral("node_modules"), QStringLiteral("__pycache__"),
        QStringLiteral(".git"), QStringLiteral(".svn"), QStringLiteral(".hg"),
        QStringLiteral(".cache"), QStringLiteral(".Trash"), QStringLiteral("AppData"),
    };
    return skip;
}

}  // namespace

bool naturalLess(const QString &a, const QString &b) { return compareNatural(NaturalKey(a), NaturalKey(b)) < 0; }

QString Track::durationText() const
{
    if (duration < 0.0)
        return QString();
    return formatTime(duration);
}

// ---------------------------------------------------------------------------
ScanResult scanLibrary(const QString &root, const QStringList &extensionsIn,
                       const std::function<bool()> &cancel)
{
    ScanResult result;
    QFileInfo rootInfo(root);
    result.root = rootInfo.absoluteFilePath();
    QSet<QString> wanted;
    for (const QString &ext : extensionsIn) {
        QString e = ext.toLower();
        while (e.startsWith(QLatin1Char('.')))
            e.remove(0, 1);
        if (!e.isEmpty())
            wanted.insert(e);
    }

    struct Pending { QString path; int depth; QSet<QString> ancestors; };
    QVector<Pending> stack{{result.root, 0, {}}};
    QSet<QString> seenDirs;
    while (!stack.isEmpty()) {
        if (cancel && cancel()) {
            result.canceled = true;
            return result;
        }
        const Pending pending = stack.takeLast();
        QFileInfo dirInfo(pending.path);
        const QString dirKey = dirInfo.absoluteFilePath();
        if (seenDirs.contains(dirKey))
            continue;
        seenDirs.insert(dirKey);
        // Lexical absolute paths keep growing through a symlink cycle. Compare
        // physical ancestor paths instead, without changing visible file paths
        // or suppressing legitimate links in other branches.
        const QString physical = dirInfo.canonicalFilePath();
        if (!physical.isEmpty() && pending.ancestors.contains(physical))
            continue;
        QSet<QString> ancestors = pending.ancestors;
        if (!physical.isEmpty())
            ancestors.insert(physical);
        QDir dir(pending.path);
        if (!dir.exists()) {
            result.errors << QStringLiteral("%1: not a directory").arg(pending.path);
            continue;
        }
        ++result.dirs;
        if (const int bad = undecodableEntries(pending.path, wanted))
            result.errors << QObject::tr("%1: %2 module file(s) or folder(s) skipped, their names "
                                         "aren't valid UTF-8 (rename them to see them)")
                                 .arg(pending.path).arg(bad);
        const QFileInfoList entries =
            dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
        for (const QFileInfo &entry : entries) {
            const QString name = entry.fileName();
            if (name.startsWith(QLatin1Char('.')))
                continue;           // hidden
            if (entry.isDir()) {
                if (pending.depth + 1 > 32 || skipDirs().contains(name))
                    continue;
                stack.append({entry.absoluteFilePath(), pending.depth + 1, ancestors});
            } else if (entry.isFile()) {
                QString ext = entry.suffix().toLower();
                if (ext.isEmpty() || !wanted.contains(ext))
                    continue;
                Track track;
                track.path = entry.absoluteFilePath();
                track.name = name;
                track.ext = ext;
                track.size = entry.size();
                track.mtime = fileMTime(track.path);
                const QString rel = QDir(result.root).relativeFilePath(track.path);
                track.relDir = QFileInfo(rel).path();
                if (track.relDir == QLatin1String("."))
                    track.relDir.clear();
                result.tracks.append(track);
            }
        }
    }
    return result;
}

OrderMode orderModeFromName(const QString &name)
{
    if (name == QLatin1String("alphabetical") || name == QLatin1String("alpha"))
        return OrderMode::Alphabetical;
    if (name == QLatin1String("shuffle"))
        return OrderMode::Shuffle;
    if (name == QLatin1String("playlist"))
        return OrderMode::Saved;
    return OrderMode::ByDirectory;
}

double fileMTime(const QString &path)
{
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) == 0)
        return double(st.st_mtime) + double(st.st_mtim.tv_nsec) / 1e9;
    return double(QFileInfo(path).lastModified().toMSecsSinceEpoch()) / 1000.0;
}

QString orderModeName(OrderMode mode)
{
    switch (mode) {
    case OrderMode::Alphabetical: return QStringLiteral("alphabetical");
    case OrderMode::Shuffle: return QStringLiteral("shuffle");
    case OrderMode::Saved: return QStringLiteral("playlist");
    case OrderMode::ByDirectory: break;
    }
    return QStringLiteral("by directory");
}

QVector<Track> orderTracks(const QVector<Track> &tracks, OrderMode mode, quint32 shuffleSeed,
                           const QString &anchorPath, const QString &avoidPath)
{
    switch (mode) {
    case OrderMode::Alphabetical:
        return sortedAlphabetical(tracks);
    case OrderMode::ByDirectory: {
        return sortedNatural(tracks, true);
    }
    case OrderMode::Shuffle: {
        QVector<Track> items = sortedAlphabetical(tracks);
        std::mt19937 rng(shuffleSeed ? shuffleSeed : std::random_device{}());
        std::shuffle(items.begin(), items.end(), rng);
        if (items.size() > 1 && !avoidPath.isEmpty() && items[0].path == avoidPath) {
            std::uniform_int_distribution<int> dist(1, items.size() - 1);
            items.swapItemsAt(0, dist(rng));
        }
        if (!anchorPath.isEmpty()) {
            for (int i = 0; i < items.size(); ++i) {
                if (items[i].path == anchorPath && i > 0) {
                    items.move(i, 0);
                    break;
                }
            }
        }
        return items;
    }
    case OrderMode::Saved:
        return tracks;
    }
    return tracks;
}

QVector<Track> applyPathOrder(const QVector<Track> &tracks, const QStringList &pathOrder)
{
    // Stable counting scatter: O(tracks + plan), with one hash lookup per
    // input path, rather than hash lookups in every sort comparison. Duplicate
    // plan paths retain the old last-rank-wins semantics; equal inputs stay stable.
    QHash<QString, int> rank; rank.reserve(pathOrder.size());
    for (int i = 0; i < pathOrder.size(); ++i) rank.insert(pathOrder[i], i);
    QVector<int> positions(pathOrder.size(), 0), ranks; ranks.reserve(tracks.size());
    QVector<Track> unknown; unknown.reserve(tracks.size());
    int count = 0;
    for (const auto &track : tracks) {
        const auto it = rank.constFind(track.path);
        if (it == rank.constEnd()) { ranks.append(-1); unknown.append(track); }
        else { ranks.append(it.value()); ++positions[it.value()]; ++count; }
    }
    int offset = 0;
    for (int &position : positions) { const int size = position; position = offset; offset += size; }
    QVector<Track> result(count);
    result.reserve(tracks.size());
    for (qsizetype i = 0; i < tracks.size(); ++i)
        if (ranks[i] >= 0) result[positions[ranks[i]]++] = tracks[i];
    result += sortedAlphabetical(unknown);
    return result;
}

QVector<Track> searchFilter(const QVector<Track> &tracks, const QString &needleIn)
{
    const QString needle = needleIn.trimmed().toLower();
    if (needle.isEmpty())
        return tracks;
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const QStringList terms = needle.split(whitespace, Qt::SkipEmptyParts);
    QVector<Track> out;
    for (const Track &track : tracks) {
        // all of it lowercased: folder and file names used to be compared
        // as-is, so "axel" never found Axel_F.MOD
        const QString haystack = QStringLiteral("%1/%2 %3 %4")
                                     .arg(track.relDir, track.name, track.fmt, track.title)
                                     .toLower();
        bool all = true;
        for (const QString &term : terms) {
            if (!haystack.contains(term)) {
                all = false;
                break;
            }
        }
        if (all)
            out << track;
    }
    return out;
}

bool QueueFilter::matches(const Track &track) const
{
    if (hideBroken && !track.broken.isEmpty())
        return false;
    if (!formats.isEmpty()) {
        const QString fmt = track.fmt.toLower().trimmed();
        // "" = not analyzed yet; older saved filters spelled it "unknown"
        if (!formats.contains(fmt) && !(fmt.isEmpty() && formats.contains(QLatin1String("unknown"))))
            return false;
    }
    if (track.duration < 0.0)
        return true;                            // unknown: keep
    if (minSeconds > 0 && track.duration < minSeconds)
        return false;
    if (maxSeconds > 0 && track.duration > maxSeconds)
        return false;
    return true;
}

int QueueFilter::count(const QVector<Track> &tracks) const
{
    int n = 0;
    for (const Track &track : tracks)
        if (matches(track))
            ++n;
    return n;
}

QVector<Track> QueueFilter::select(const QVector<Track> &tracks) const
{
    if (!active()) return tracks; // retain implicit sharing for the common unfiltered queue
    QVector<Track> out; out.reserve(tracks.size());
    for (const Track &track : tracks)
        if (matches(track))
            out << track;
    return out;
}

QString QueueFilter::describe() const
{
    QStringList parts;
    if (!formats.isEmpty()) {
        // a long list made the status line wider than the window
        QStringList sorted = formats;
        sorted.sort();
        for (QString &f : sorted)
            if (f.isEmpty())
                f = QObject::tr("unknown");
        parts << (sorted.size() > 4 ? QObject::tr("%1 formats").arg(sorted.size())
                                    : sorted.join(QLatin1Char('/')));
    }
    auto seconds = [](double value) {
        if (value >= 60.0) {
            const int total = int(value + 0.5);
            return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1s").arg(int(value));
    };
    if (minSeconds > 0)
        parts << QStringLiteral("\u2265%1").arg(seconds(minSeconds));
    if (maxSeconds > 0)
        parts << QStringLiteral("\u2264%1").arg(seconds(maxSeconds));
    if (hideBroken)
        parts << QObject::tr("playable only");
    return parts.isEmpty() ? QObject::tr("no filter") : parts.join(QStringLiteral(", "));
}
