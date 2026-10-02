// The JSON stores that live beside config.json, file-compatible with the
// Python version: analysis.json, playlists.json, listening-stats.json,
// ignored.json.
#pragma once

#include "casefold.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// --- JSON key order -----------------------------------------------------------
// QJsonObject sorts its keys, but the Python files keep insertion order and the
// stores use it for least-recently-used pruning. These read the keys of a
// top-level member object in file order, and write a JSON string literal for
// building an object in a chosen order.
QStringList jsonMemberKeyOrder(const QByteArray &json, const QString &member);
QByteArray jsonStringLiteral(const QString &text);

// --- path helpers -----------------------------------------------------------
QString analysisCachePath();
QString playlistsPath();
QString statsPath();
QString ignoredPath();

// --- analysis cache (cache.py) ----------------------------------------------
struct CachedModule {
    qint64 size = 0;
    double mtime = 0.0;
    double duration = -1.0;      // -1 unknown, >=1e18 endless
    QString fmt;
    int channels = -1;
    int subsongs = 1;
    QString title;
    QString broken;

    bool hasDuration() const { return duration >= 0.0; }
};

class AnalysisCache {
public:
    explicit AnalysisCache(const QString &path = QString());
    int load();
    // Fill the details when the record matches size+mtime; returns true on hit.
    bool apply(const QString &path, qint64 size, double mtime, CachedModule *out);
    void remember(const QString &path, const CachedModule &entry);
    bool save();
    int entryCount() const { return entries_.size(); }
    static QString pathFor(const QString &dir);

private:
    void touch(const QString &path) { used_[path] = ++clock_; }
    QString path_;
    QHash<QString, CachedModule> entries_;
    QHash<QString, quint64> used_;   // recency stamp per entry (LRU pruning)
    quint64 clock_ = 0;
    bool dirty_ = false;
};

// --- playlists (playlists.py) -------------------------------------------------
struct Playlist {
    QString name;
    QStringList paths;
    QString root;
};

class PlaylistStore {
public:
    static constexpr const char *kFavoritesName = "Favorites";
    explicit PlaylistStore(const QString &path = QString());

    bool load();
    bool save() const;

    QStringList names() const;                     // in file order, Favorites first
    const Playlist *get(const QString &name) const;
    Playlist *getMutable(const QString &name);
    bool has(const QString &name) const { return get(name) != nullptr; }
    static bool isFavorites(const QString &name);

    QString error;
    bool readError() const { return readError_; }   // the file exists but couldn't be read

    bool create(const QString &name, const QStringList &paths, const QString &root = QString());
    bool replace(const QString &name, const QStringList &paths, const QString &root = QString());
    bool rename(const QString &oldName, const QString &newName);
    bool remove(const QString &name);
    bool addPaths(const QString &name, const QStringList &paths, const QString &root = QString());
    bool removePaths(const QString &name, const QSet<QString> &paths);
    bool toggleFavorite(const QString &path);      // returns true when now favourited
    bool isFavorite(const QString &path) const;
    bool clearFavorites();

    static QString normalizeName(const QString &name, QString *errorOut = nullptr);
    // A free name from `base`: base itself, or base + suffix (e.g. " %1" or
    // " (%1)") counting from 2, shortened so it stays within 80 characters.
    // Empty (with errorOut) when base isn't a valid name.
    QString uniqueName(const QString &base, const QString &suffix, QString *errorOut = nullptr) const;

private:
    static QString key(const QString &name) { return caseFold(name.trimmed()); }
    QString path_;
    bool readError_ = false;                       // file exists but could not be trusted
    QVector<Playlist> playlists_;                  // insertion order, Favorites first
    QHash<QString, int> index_;                    // key -> index in playlists_
    void reindex();
};

// M3U import/export (paths resolved beside the file; missing counted)
int writeM3u(const QStringList &paths, const QString &path);
QStringList readM3u(const QString &path, int *skippedOut = nullptr);

// --- listening stats (stats.py) -----------------------------------------------
struct ModuleStats {
    QString path;
    QString title;
    qint64 plays = 0;
    double seconds = 0.0;
    double lastPlayed = 0.0;      // epoch seconds
};

class StatsStore {
public:
    explicit StatsStore(const QString &path = QString());
    void load();
    bool record(const QString &path, double seconds, bool play, const QString &title = QString());
    bool save();
    QVector<ModuleStats> mostPlayed(int limit) const;   // by plays desc
    void resetAll();
    QString error;
    int count() const { return records_.size(); }
    double totalSeconds() const;
    double since() const { return since_; }

private:
    static QString pathKey(const QString &path);
    QString path_;
    QHash<QString, ModuleStats> records_;
    double since_ = 0.0;
    bool dirty_ = false;
    bool blocked_ = false;
};

// --- ignore list (ignored.py) ---------------------------------------------------
class IgnoreStore {
public:
    explicit IgnoreStore(const QString &path = QString());
    bool contains(const QString &path) const;
    QStringList paths() const;                    // sorted, display paths
    int count() const { return paths_.size(); }
    bool change(const QStringList &add, const QStringList &remove = {});
    QString error;

private:
    static QString pathKey(const QString &path);
    QString path_;
    QSet<QString> paths_;
    bool readError_ = false;   // file exists but could not be trusted
};
