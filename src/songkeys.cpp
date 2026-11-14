#include "songkeys.h"

#include "config.h"
#include "jsonfile.h"
#include "library.h"
#include "openmptapi.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>
#include <utility>

namespace {

constexpr int kFileVersion = 1;
constexpr int kMinNoteCells = 16;   // below this, unrelated modules share their few notes

}  // namespace

QString songKeysPath() { return modjukeConfigFile(QStringLiteral("song-keys.json")); }

QString songKey(const OpenMPTLib &lib, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray data = file.readAll();
    // the notes need no sample data
    const OpenMPTModule module = lib.openMemory(data, {{"load.skip_samples", "1"}});
    if (module.isOpen()) {
        // every non-empty cell with its place in the song: the row counted
        // along the order list, and the channel
        QCryptographicHash hash(QCryptographicHash::Sha1);
        int cells = 0, songRow = 0;
        const int channels = module.numChannels();
        for (int order = 0; order < module.numOrders(); ++order) {
            const int pattern = module.orderPattern(order);
            const int rows = pattern < 0 ? 0 : module.patternRows(pattern);
            for (int row = 0; row < rows; ++row, ++songRow) {
                for (int channel = 0; channel < channels; ++channel) {
                    char cell[12] = {};
                    bool empty = true;
                    for (int type = 0; type < 6; ++type) {
                        cell[6 + type] = char(module.patternCommand(
                            pattern, row, channel, OpenMPTModule::CommandType(type)));
                        empty = empty && cell[6 + type] == 0;
                    }
                    if (empty)
                        continue;
                    for (int i = 0; i < 4; ++i)
                        cell[i] = char(songRow >> (8 * i));
                    cell[4] = char(channel);
                    cell[5] = char(channel >> 8);
                    hash.addData(QByteArrayView(cell, sizeof cell));
                    ++cells;
                }
            }
        }
        if (cells >= kMinNoteCells)
            return QStringLiteral("n:") + QString::fromLatin1(hash.result().toHex());
    }
    return QStringLiteral("f:")
        + QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

QHash<QString, QStringList> copiesWithin(const QStringList &paths, const KeyOf &keyOf)
{
    QHash<QString, QStringList> byKey;
    for (const QString &path : paths) {
        const QString key = keyOf(path);
        if (!key.isEmpty())
            byKey[key].append(path);
    }
    QHash<QString, QStringList> copies;
    for (const QStringList &group : std::as_const(byKey)) {
        if (group.size() < 2)
            continue;
        for (const QString &path : group) {
            QStringList others = group;
            others.removeAll(path);
            copies.insert(path, others);
        }
    }
    return copies;
}

QVector<QPair<QString, QString>> copiesOf(const QStringList &existing, const QStringList &adding,
                                          const KeyOf &keyOf)
{
    QHash<QString, QString> firstByKey;
    QSet<QString> listed;
    for (const QString &path : existing) {
        listed.insert(path);
        const QString key = keyOf(path);
        if (!key.isEmpty() && !firstByKey.contains(key))
            firstByKey.insert(key, path);
    }
    QVector<QPair<QString, QString>> copies;
    for (const QString &path : adding) {
        if (listed.contains(path))
            continue;
        listed.insert(path);
        const QString key = keyOf(path);
        if (key.isEmpty())
            continue;
        const auto first = firstByKey.constFind(key);
        if (first != firstByKey.cend())
            copies.append({path, first.value()});
        else
            firstByKey.insert(key, path);
    }
    return copies;
}

SongKeys::SongKeys(const QString &path, QObject *parent)
    : QObject(parent), path_(path.isEmpty() ? songKeysPath() : path)
{
    const OpenMPTLib *lib = OpenMPTLib::instance();
    if (!lib)
        return;
    version_ = lib->versionString();
    const JsonRead read = readJsonObject(path_, 0, kFileVersion);
    if (!read.ok() || read.root.value(QStringLiteral("libopenmpt")).toString() != version_)
        return;
    const QJsonObject keys = read.root.value(QStringLiteral("keys")).toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const QJsonObject raw = it.value().toObject();
        Entry entry;
        entry.size = qint64(raw.value(QStringLiteral("size")).toDouble());
        entry.mtime = raw.value(QStringLiteral("mtime")).toDouble();
        entry.key = raw.value(QStringLiteral("key")).toString();
        if (!entry.key.isEmpty())
            entries_.insert(it.key(), entry);
    }
}

SongKeys::~SongKeys()
{
    if (!thread_)
        return;
    // keep what the worker got done; no signal while the owner goes away
    cancel_ = true;
    thread_->wait();
    if (apply(result_))
        save();
}

QString SongKeys::key(const QString &path) const { return entries_.value(path).key; }

KeyOf SongKeys::keyOf() const
{
    return [this](const QString &path) { return key(path); };
}

void SongKeys::prepare(const QStringList &paths)
{
    if (version_.isEmpty())
        return;
    if (thread_)
        pending_ += paths;
    else
        start(paths);
}

void SongKeys::prepareNow(const QStringList &paths)
{
    if (!version_.isEmpty())
        merge(compute(entries_, paths, std::atomic_bool(false)));
}

QHash<QString, SongKeys::Entry> SongKeys::compute(const QHash<QString, Entry> &known,
                                                  const QStringList &paths,
                                                  const std::atomic_bool &cancel)
{
    QHash<QString, Entry> changed;
    const OpenMPTLib *lib = OpenMPTLib::instance();
    if (!lib)
        return changed;
    for (const QString &path : paths) {
        if (cancel)
            break;
        const auto old = known.constFind(path);
        const QFileInfo info(path);
        if (!info.exists()) {
            if (old != known.cend())
                changed.insert(path, Entry());
            continue;
        }
        Entry entry;
        entry.size = info.size();
        entry.mtime = fileMTime(path);
        if (old != known.cend() && old->size == entry.size && old->mtime == entry.mtime)
            continue;
        entry.key = songKey(*lib, path);
        changed.insert(path, entry);
    }
    return changed;
}

void SongKeys::start(QStringList paths)
{
    paths.removeDuplicates();
    cancel_ = false;
    thread_ = QThread::create([this, paths, known = entries_] { result_ = compute(known, paths, cancel_); });
    thread_->setParent(this);
    connect(thread_, &QThread::finished, this, [this] {
        thread_->deleteLater();
        thread_ = nullptr;
        merge(std::exchange(result_, {}));
        if (!pending_.isEmpty())
            start(std::exchange(pending_, {}));
    });
    thread_->start(QThread::LowPriority);
}

bool SongKeys::apply(const QHash<QString, Entry> &changed)
{
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        if (it->key.isEmpty())
            entries_.remove(it.key());
        else
            entries_.insert(it.key(), it.value());
    }
    return !changed.isEmpty();
}

void SongKeys::merge(const QHash<QString, Entry> &changed)
{
    if (!apply(changed))
        return;
    save();
    emit keysChanged();
}

void SongKeys::save() const
{
    QJsonObject keys;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        QJsonObject raw;
        raw.insert(QStringLiteral("size"), double(it->size));
        raw.insert(QStringLiteral("mtime"), it->mtime);
        raw.insert(QStringLiteral("key"), it->key);
        keys.insert(it.key(), raw);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFileVersion);
    root.insert(QStringLiteral("libopenmpt"), version_);
    root.insert(QStringLiteral("keys"), keys);
    writeFileAtomic(path_, QJsonDocument(root).toJson(QJsonDocument::Compact));
}
