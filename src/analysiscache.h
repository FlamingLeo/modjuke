// The analysis cache (cache.py): duration, format and counts per module file,
// valid while the file's size and mtime match. Stored in analysis.json.
#pragma once

#include <QHash>
#include <QString>

QString analysisCachePath();   // <config dir>/analysis.json

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
    bool operator==(const CachedModule &) const = default;
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
