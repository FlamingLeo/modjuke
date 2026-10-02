#include "statsstore.h"

#include "config.h"
#include "jsonfile.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {

double now() { return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

// stats.py _number(): finite, 0..1e15 or the default
double statsNumber(const QJsonValue &value, double fallback) { return jsonNumber(value, fallback, 0.0, 1e15); }

}  // namespace

QString statsPath() { return modjukeConfigFile(QStringLiteral("listening-stats.json")); }

QString StatsStore::pathKey(const QString &path) { return absolutePathKey(path, HomeTilde::Expand); }

StatsStore::StatsStore(const QString &path) : path_(path.isEmpty() ? statsPath() : path), since_(now())
{
    load();
}

void StatsStore::load()
{
    records_.clear();
    const JsonRead file = readJsonObject(path_, 64LL * 1024 * 1024, 1);
    if (file.status == JsonRead::Missing)
        return;
    const QJsonValue modules = file.root.value(QStringLiteral("modules"));
    QString problem;
    if (file.status == JsonRead::TooLarge)
        problem = QObject::tr("file is too large");
    else if (file.status == JsonRead::OpenFailed)
        problem = file.reason;
    else if (!file.ok() || !modules.isArray())
        problem = QObject::tr("Unrecognised statistics format");
    else if (modules.toArray().size() > kMaxModules)
        problem = QObject::tr("Too many modules");
    if (!problem.isEmpty()) {
        error = QObject::tr("Could not read listening stats: %1. Existing file left untouched.").arg(problem);
        blocked_ = true;
        return;
    }
    if (file.root.contains(QStringLiteral("since")))
        since_ = statsNumber(file.root.value(QStringLiteral("since")), since_);
    for (const QJsonValue &item : modules.toArray()) {
        if (!item.isObject())
            continue;
        const QJsonObject raw = item.toObject();
        const QString path = raw.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        const QString key = pathKey(path);
        if (records_.contains(key))
            continue;   // duplicates keep the first record (python parity)
        ModuleStats row;
        row.path = key;   // records are keyed by the normalized absolute path
        row.title = raw.value(QStringLiteral("title")).toString().left(1000);
        row.plays = qint64(statsNumber(raw.value(QStringLiteral("plays")), 0.0));
        row.seconds = statsNumber(raw.value(QStringLiteral("seconds")), 0.0);
        row.lastPlayed = statsNumber(raw.value(QStringLiteral("last_played")), 0.0);
        records_.insert(key, row);
    }
}

bool StatsStore::record(const QString &path, double seconds, bool play, const QString &title)
{
    const QString key = pathKey(path);   // like python: recorded in memory even while blocked
    auto it = records_.find(key);
    if (it == records_.end()) {
        if (records_.size() >= kMaxModules) {
            error = QObject::tr("Listening stats limit reached, existing history is kept.");
            return false;
        }
        it = records_.insert(key, ModuleStats{key});
    }
    ModuleStats &row = it.value();
    row.seconds += qMax(0.0, seconds);
    if (play)
        row.plays += 1;
    row.lastPlayed = now();
    if (!title.isEmpty())
        row.title = title.left(1000);
    dirty_ = true;
    return true;
}

bool StatsStore::save()
{
    if (!dirty_)
        return !blocked_;
    if (blocked_)
        return false;   // unreadable history is preserved until an explicit reset
    QJsonArray modules;
    for (const ModuleStats &row : records_) {
        QJsonObject raw;
        raw.insert(QStringLiteral("path"), row.path);
        raw.insert(QStringLiteral("title"), row.title);
        raw.insert(QStringLiteral("plays"), double(row.plays));
        raw.insert(QStringLiteral("seconds"), row.seconds);
        raw.insert(QStringLiteral("last_played"), row.lastPlayed);
        modules.append(raw);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("since"), since_);
    payload.insert(QStringLiteral("modules"), modules);
    if (writeFileAtomic(path_, QJsonDocument(payload).toJson(QJsonDocument::Compact))) {
        dirty_ = false;
        error.clear();
        return true;
    }
    error = QObject::tr("Could not save listening stats: %1").arg(path_);
    return false;
}

QVector<ModuleStats> StatsStore::mostPlayed(int limit) const
{
    QVector<ModuleStats> rows;
    rows.reserve(records_.size());
    for (const ModuleStats &row : records_)
        rows << row;
    std::stable_sort(rows.begin(), rows.end(), [](const ModuleStats &a, const ModuleStats &b) {
        if (a.plays != b.plays)
            return a.plays > b.plays;
        return a.seconds > b.seconds;
    });
    if (limit >= 0 && rows.size() > limit)
        rows.resize(limit);   // one step (removing one at a time was O(n^2))
    return rows;
}

void StatsStore::resetAll()
{
    // A failed reset must not leave the dialog empty while the old file still
    // exists; restore the in-memory history if the replacement cannot be saved.
    const QHash<QString, ModuleStats> oldRecords = records_;
    const double oldSince = since_;
    const bool oldDirty = dirty_;
    const bool oldBlocked = blocked_;
    records_.clear();
    since_ = now();
    dirty_ = true;
    blocked_ = false; // an explicit reset is allowed to replace bad history
    if (save())
        return;
    records_ = oldRecords;
    since_ = oldSince;
    dirty_ = oldDirty;
    blocked_ = oldBlocked;
}

double StatsStore::totalSeconds() const
{
    double total = 0.0;
    for (const ModuleStats &row : records_)
        total += row.seconds;
    return total;
}
