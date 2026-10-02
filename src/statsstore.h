// Listening stats (stats.py): plays and listening time per module, stored in
// listening-stats.json.
#pragma once

#include <QHash>
#include <QString>
#include <QVector>

QString statsPath();   // <config dir>/listening-stats.json

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
    static constexpr int kMaxModules = 100000;
    static QString pathKey(const QString &path);
    QString path_;
    QHash<QString, ModuleStats> records_;
    double since_ = 0.0;
    bool dirty_ = false;
    bool blocked_ = false;   // the file could not be trusted: never overwritten
};
