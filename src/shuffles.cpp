#include "shuffles.h"

#include "casefold.h"
#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString cleanAbs(const QString &path)
{
    if (path.isEmpty())
        return QString();
    const QFileInfo info(path);
    return QDir::cleanPath(info.absoluteFilePath());
}

}   // namespace

ShuffleStore::ShuffleStore(const QString &path)
    : path_(path.isEmpty() ? shufflesPath() : path)
{
    QFile file(path_);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Could not read saved shuffle orders (could not open the file), "
                               "new ones will be drawn");
        return;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    const QJsonObject root = doc.isObject() ? doc.object() : QJsonObject{};
    if (parseError.error != QJsonParseError::NoError || root.isEmpty()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("orders")).isObject()) {
        orders_.clear();
        keyOrder_.clear();
        error = QStringLiteral("Could not read saved shuffle orders (unknown format), "
                               "new ones will be drawn");
        return;
    }
    const QJsonObject orders = root.value(QStringLiteral("orders")).toObject();
    for (auto it = orders.constBegin(); it != orders.constEnd(); ++it) {
        if (!it.value().isArray())
            continue;
        QStringList paths;
        const QJsonArray array = it.value().toArray();
        for (const QJsonValue &value : array) {
            if (value.isString())
                paths << value.toString();
            if (paths.size() >= kMaxPaths)
                break;
        }
        if (!paths.isEmpty()) {
            keyOrder_ << it.key();
            orders_.insert(it.key(), paths);
        }
    }
}

QString ShuffleStore::shufflesPath()
{
    return QDir(modjukeConfigDir()).filePath(QStringLiteral("shuffles.json"));
}

QString ShuffleStore::libraryKey(const QString &directory)
{
    return QStringLiteral("library:") + cleanAbs(directory);
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
    QStringList clean;
    for (const QString &path : paths) {
        clean << path;
        if (clean.size() >= kMaxPaths)
            break;
    }
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
    QJsonObject orders;
    for (const QString &key : keyOrder_)
        orders.insert(key, QJsonArray::fromStringList(orders_.value(key)));
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("orders"), orders);
    QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!data.endsWith('\n'))
        data.append('\n');
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Could not save the shuffle order: %1").arg(file.errorString());
        return false;
    }
    file.write(data);
    if (!file.commit()) {
        error = QStringLiteral("Could not save the shuffle order: %1").arg(file.error());
        return false;
    }
    error.clear();
    return true;
}
