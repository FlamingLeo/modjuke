#include "ignorestore.h"

#include "config.h"
#include "jsonfile.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

QString ignoredPath() { return modjukeConfigFile(QStringLiteral("ignored.json")); }

QString IgnoreStore::pathKey(const QString &path) { return absolutePathKey(path, HomeTilde::Expand); }

IgnoreStore::IgnoreStore(const QString &path) : path_(path.isEmpty() ? ignoredPath() : path)
{
    const JsonRead file = readJsonObject(path_, 0, 1);
    if (file.status == JsonRead::Missing)
        return;   // missing == empty list
    const QJsonValue pathsValue = file.root.value(QStringLiteral("paths"));
    const QJsonArray array = pathsValue.toArray();
    // ignored.py: every entry must be a non-empty absolute path
    auto absolute = [](const QJsonValue &item) {
        return item.isString() && !item.toString().isEmpty() && QDir::isAbsolutePath(item.toString());
    };
    QString problem;
    if (file.status == JsonRead::OpenFailed)
        problem = QObject::tr("Permission denied");
    else if (!file.ok() || !pathsValue.isArray() || array.size() > kMaxPaths)
        problem = QObject::tr("Invalid ignore-list format");
    else if (!std::all_of(array.begin(), array.end(), absolute))
        problem = QObject::tr("Ignored paths must be absolute file paths");
    if (!problem.isEmpty()) {
        readError_ = true;
        error = QObject::tr("Could not read ignored songs: %1. File kept unchanged: %2").arg(problem, path_);
        return;
    }
    for (const QJsonValue &item : array)
        paths_.insert(pathKey(item.toString()));   // stored normalized, like python
}

bool IgnoreStore::contains(const QString &path) const
{
    if (paths_.isEmpty() || path.isEmpty())
        return false;
    return paths_.contains(path) || paths_.contains(pathKey(path));
}

QStringList IgnoreStore::paths() const
{
    QStringList out = paths_.values();
    out.sort();
    return out;
}

bool IgnoreStore::change(const QStringList &add, const QStringList &remove)
{
    if (readError_)
        return false;   // ignored.py raises: the damaged file stays untouched
    QSet<QString> next = paths_;
    for (const QString &path : add) {
        if (!path.isEmpty())
            next.insert(pathKey(path));
    }
    for (const QString &path : remove)
        next.remove(pathKey(path));
    if (next.size() > kMaxPaths) {
        error = QObject::tr("The ignore list is limited to 100000 songs");
        return false;
    }
    if (next == paths_)
        return false;   // change() returns False when there is nothing to do (python parity)
    QStringList sorted = next.values();
    sorted.sort();
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("paths"), QJsonArray::fromStringList(sorted));
    if (!writeFileAtomic(path_, QJsonDocument(payload).toJson(QJsonDocument::Indented) + '\n')) {
        error = QObject::tr("Could not save ignored songs: %1").arg(path_);
        return false;
    }
    paths_ = next;
    error.clear();
    return true;
}
