#include "shuffles.h"

#include <QRandomGenerator>

#include <algorithm>

#include "casefold.h"
#include "config.h"
#include "jsonfile.h"

#include <QJsonArray>

ShuffleStore::ShuffleStore(const QString &path)
    : path_(path.isEmpty() ? shufflesPath() : path)
{
    const JsonRead file = readJsonObject(path_, 0, 1);
    if (file.status == JsonRead::Missing)
        return;
    if (file.status == JsonRead::OpenFailed) {
        error = QStringLiteral("Could not read saved shuffle orders (could not open the file), "
                               "new ones will be drawn");
        return;
    }
    if (!file.ok() || !file.root.value(QStringLiteral("orders")).isObject()) {
        error = QStringLiteral("Could not read saved shuffle orders (unknown format), "
                               "new ones will be drawn");
        return;
    }
    // least recently drawn first: the file's own key order (QJsonObject would
    // iterate alphabetically, and pruning would drop the wrong orders)
    for (const auto &[key, value] : jsonMemberEntries(file, QStringLiteral("orders"))) {
        if (!value.isArray())
            continue;
        const QStringList paths = jsonStringsStrict(value, kMaxPaths);
        if (!paths.isEmpty()) {
            keyOrder_ << key;
            orders_.insert(key, paths);
        }
    }
}

QString ShuffleStore::shufflesPath()
{
    return modjukeConfigFile(QStringLiteral("shuffles.json"));
}

QString ShuffleStore::libraryKey(const QString &directory)
{
    return QStringLiteral("library:") + absolutePathKey(directory, HomeTilde::Keep);
}

QString ShuffleStore::playlistKey(const QString &name)
{
    return QStringLiteral("playlist:") + caseFold(QString(name).trimmed());
}

QStringList ShuffleStore::get(const QString &key) const
{
    return orders_.value(key);
}

bool ShuffleStore::put(const QString &key, const QStringList &paths)
{
    const QStringList clean = paths.mid(0, kMaxPaths);
    if (clean.isEmpty())
        return false;
    if (orders_.value(key) == clean)
        return true;
    keyOrder_.removeAll(key);
    orders_.remove(key);
    keyOrder_ << key;
    orders_.insert(key, clean);
    prune();
    return save();
}

bool ShuffleStore::rename(const QString &oldKey, const QString &newKey)
{
    if (oldKey == newKey || !orders_.contains(oldKey))
        return true;
    orders_.insert(newKey, orders_.take(oldKey));
    keyOrder_.removeAll(oldKey);
    keyOrder_.removeAll(newKey);   // a stale order under the new name is replaced
    keyOrder_ << newKey;
    return save();
}

bool ShuffleStore::remove(const QString &key)
{
    if (!orders_.contains(key))
        return true;
    orders_.remove(key);
    keyOrder_.removeAll(key);
    return save();
}

void ShuffleStore::prune()
{
    while (keyOrder_.size() > kMaxSources) {
        const QString oldest = keyOrder_.takeFirst();
        orders_.remove(oldest);
    }
}

bool ShuffleStore::save()
{
    // keys in least-recently-drawn order, like the Python writer (a QJsonObject
    // would sort them and lose the order pruning relies on)
    JsonEntries entries;
    entries.reserve(keyOrder_.size());
    for (const QString &key : std::as_const(keyOrder_))
        entries.append({key, QJsonArray::fromStringList(orders_.value(key))});
    WriteFailure why;
    if (!writeFileAtomic(path_, versionedMemberJson(QStringLiteral("orders"), entries) + '\n', &why)) {
        // a failed commit has always been reported by its error code
        error = QStringLiteral("Could not save the shuffle order: %1")
                    .arg(why.opened ? QString::number(why.code) : why.text);
        return false;
    }
    error.clear();
    return true;
}

QStringList ShuffleStore::mergeIntoPlan(const QStringList &plan, const QStringList &fresh,
                                        QRandomGenerator *rng)
{
    // each new song gets a random slot; one merge pass keeps it O(n)
    QVector<QPair<int, QString>> places;
    places.reserve(fresh.size());
    for (const QString &path : fresh)
        places.append({int(rng->bounded(plan.size() + 1)), path});
    std::sort(places.begin(), places.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QStringList merged;
    merged.reserve(plan.size() + places.size());
    int s = 0;
    for (int i = 0; i <= plan.size(); ++i) {
        while (s < places.size() && places[s].first == i)
            merged << places[s++].second;
        if (i < plan.size())
            merged << plan[i];
    }
    return merged;
}
