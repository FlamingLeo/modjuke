// Module library: scanning, ordering, search (mirrors modjuke/library.py).
#pragma once

#include <QString>
#include <QVector>

#include <functional>

struct CachedModule;

struct Track {
    QString path;
    QString relDir;              // directory relative to the library root ("" = root)
    QString name;                // file name
    QString ext;
    qint64 size = 0;
    double mtime = 0.0;

    // filled in by the analyzer
    bool analyzed = false;
    double duration = -1.0;      // -1 = unknown, >=1e18 = endless
    QString fmt;
    int channels = -1;
    int subsongs = 0;
    QString title;
    QString broken;              // non-empty: failed to load

    // Take the analyzer's (or the cache's) details and mark the track analyzed.
    void applyAnalysis(const CachedModule &entry);

    QString durationText() const;
    QString displayTitle() const { return title.isEmpty() ? name : title; }
};

QString formatTime(double seconds);           // m:ss / h:mm:ss, "--:--" unknown, inf -> "∞"
bool naturalLess(const QString &a, const QString &b);

struct ScanResult {
    QString root;
    QVector<Track> tracks;
    int dirs = 0;
    QStringList errors;
    bool canceled = false;
};

// Recursive scan for module files (skip hidden dirs, symlink-loop guard,
// SKIP_DIRS); progress callback receives (files so far, dir).
ScanResult scanLibrary(const QString &root, const QStringList &extensions,
                       const std::function<bool()> &cancel = {});

enum class OrderMode { Alphabetical, ByDirectory, Shuffle, Saved };

// Modification time with the same (sub-microsecond) precision as os.path.getmtime -
// QFileInfo::lastModified is millisecond-only and would miss python's cache records.
double fileMTime(const QString &path);

OrderMode orderModeFromName(const QString &name);
QString orderModeName(OrderMode mode);

// Natural sort keys used by the queue.
QVector<Track> orderTracks(const QVector<Track> &tracks, OrderMode mode, quint32 shuffleSeed,
                           const QString &anchorPath = QString(), const QString &avoidPath = QString());

// Apply a saved path order; unlisted tracks follow alphabetically.
QVector<Track> applyPathOrder(const QVector<Track> &tracks, const QStringList &pathOrder);

// Search: all space-separated terms must occur in "reldir/name fmt title".
QVector<Track> searchFilter(const QVector<Track> &tracks, const QString &needle);

// Queue filter (filters.py): formats, duration bounds, hide broken.
struct QueueFilter {
    QStringList formats;         // lower case; empty = all
    double minSeconds = 0.0;     // 0 -> none
    double maxSeconds = 0.0;
    bool hideBroken = false;

    bool active() const { return !formats.isEmpty() || minSeconds > 0 || maxSeconds > 0 || hideBroken; }
    bool matches(const Track &track) const;
    int count(const QVector<Track> &tracks) const;
    QVector<Track> select(const QVector<Track> &tracks) const;
    QString describe() const;
};
