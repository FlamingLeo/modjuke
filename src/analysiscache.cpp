#include "analysiscache.h"

#include "config.h"
#include "jsonfile.h"

#include <QDir>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

QString analysisCachePath() { return AnalysisCache::pathFor(modjukeConfigDir()); }

QString AnalysisCache::pathFor(const QString &dir)
{
    return QDir(dir).filePath(QStringLiteral("analysis.json"));
}

AnalysisCache::AnalysisCache(const QString &path) : path_(path.isEmpty() ? analysisCachePath() : path) {}

int AnalysisCache::load()
{
    entries_.clear();
    used_.clear();
    clock_ = 0;
    const JsonRead file = readJsonObject(path_);
    if (!file.ok())
        return 0;
    // a negative or fractional count marks an unusable record
    auto badCount = [](const QJsonValue &v) {
        return v.isDouble() && (v.toDouble() < 0 || v.toDouble() != std::trunc(v.toDouble()));
    };
    // recency = order in the file (least recent first), as Python writes it
    for (const auto &[key, value] : jsonMemberEntries(file, QStringLiteral("entries"))) {
        if (!value.isObject())
            continue;
        const QJsonObject raw = value.toObject();
        const QJsonValue dur = raw.value(QStringLiteral("dur"));
        const QJsonValue ch = raw.value(QStringLiteral("ch"));
        const QJsonValue sub = raw.value(QStringLiteral("sub"));
        if (badCount(ch) || badCount(sub) || (dur.isDouble() && dur.toDouble() < 0.0))
            continue;   // force a fresh analysis like the Tk app
        CachedModule entry;
        const double sizeValue = raw.value(QStringLiteral("size")).toDouble();
        entry.size = std::isfinite(sizeValue) ? qint64(std::clamp(sizeValue, -1.0, 9.0e15)) : -1;
        entry.mtime = raw.value(QStringLiteral("mtime")).toDouble();
        if (dur.isString() && dur.toString() == QLatin1String("inf"))
            entry.duration = 1e18;
        else if (dur.isDouble())
            entry.duration = std::min(dur.toDouble(), 1e18);   // beyond = endless
        entry.fmt = raw.value(QStringLiteral("fmt")).toString();
        entry.channels = clampedInt(jsonNumber(ch, -1), -1, 4096);
        entry.subsongs = clampedInt(jsonNumber(sub, 1), 0, 1 << 20);
        entry.title = raw.value(QStringLiteral("title")).toString();
        entry.broken = raw.value(QStringLiteral("broken")).toString();
        entries_.insert(key, entry);
        used_.insert(key, ++clock_);
    }
    return entries_.size();
}

bool AnalysisCache::apply(const QString &path, qint64 size, double mtime, CachedModule *out)
{
    auto it = entries_.find(path);
    if (it == entries_.end())
        return false;
    const CachedModule &entry = it.value();
    if (entry.size != size || std::abs(entry.mtime - mtime) >= 1e-6)
        return false;
    if (out)
        *out = entry;
    touch(path);   // O(1): a rescan applies every track of the library
    return true;
}

void AnalysisCache::remember(const QString &path, const CachedModule &entry)
{
    auto it = entries_.find(path);
    if (it != entries_.end() && it.value() == entry)
        return;
    entries_[path] = entry;
    touch(path);
    dirty_ = true;
}

bool AnalysisCache::save()
{
    if (!dirty_)
        return false;
    // least recently used first; prune cold entries beyond the cap
    static constexpr int kMaxEntries = 50000;
    QVector<QPair<quint64, QString>> order;
    order.reserve(entries_.size());
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it)
        order.append({used_.value(it.key()), it.key()});
    std::sort(order.begin(), order.end());
    while (order.size() > kMaxEntries) {
        entries_.remove(order.first().second);
        used_.remove(order.first().second);
        order.removeFirst();
    }
    JsonEntries records;
    records.reserve(order.size());
    for (const auto &item : std::as_const(order)) {
        const QString &path = item.second;
        auto it = entries_.constFind(path);
        if (it == entries_.constEnd())
            continue;
        const CachedModule &entry = it.value();
        QJsonObject raw;
        raw.insert(QStringLiteral("size"), double(entry.size));
        raw.insert(QStringLiteral("mtime"), entry.mtime);
        if (entry.duration >= 1e18)
            raw.insert(QStringLiteral("dur"), QStringLiteral("inf"));
        else if (entry.duration >= 0.0)
            raw.insert(QStringLiteral("dur"), qRound(entry.duration * 1000.0) / 1000.0);
        else
            raw.insert(QStringLiteral("dur"), QJsonValue::Null);
        raw.insert(QStringLiteral("fmt"), entry.fmt);
        raw.insert(QStringLiteral("ch"), entry.channels >= 0 ? QJsonValue(entry.channels) : QJsonValue::Null);
        raw.insert(QStringLiteral("sub"), entry.subsongs);
        raw.insert(QStringLiteral("title"), entry.title);
        raw.insert(QStringLiteral("broken"), entry.broken.isEmpty() ? QJsonValue::Null : QJsonValue(entry.broken));
        records.append({path, raw});
    }
    if (!writeFileAtomic(path_, versionedMemberJson(QStringLiteral("entries"), records)))
        return false;
    dirty_ = false;
    return true;
}
