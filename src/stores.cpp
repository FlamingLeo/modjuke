#include "stores.h"

#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace {

// os.path.abspath + expanduser + normcase (identity on Linux)
QString absolutePathKey(const QString &path)
{
    QString expanded = path;
    if (expanded.startsWith(QLatin1Char('~'))) {
        const QString home = QDir::homePath();
        if (expanded.size() == 1 || expanded.at(1) == QLatin1Char('/'))
            expanded = home + expanded.mid(1);
    }
    const QFileInfo info(expanded);
    return QDir::cleanPath(info.absoluteFilePath());
}

QString indentJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString compactJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool atomicWrite(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(data);
    return file.commit();
}

}  // namespace

QString analysisCachePath() { return AnalysisCache::pathFor(modjukeConfigDir()); }
QString playlistsPath() { return QDir(modjukeConfigDir()).filePath(QStringLiteral("playlists.json")); }
QString statsPath() { return QDir(modjukeConfigDir()).filePath(QStringLiteral("listening-stats.json")); }
QString ignoredPath() { return QDir(modjukeConfigDir()).filePath(QStringLiteral("ignored.json")); }

// ---------------------------------------------------------------------------
// AnalysisCache
// ---------------------------------------------------------------------------

QString AnalysisCache::pathFor(const QString &dir)
{
    return QDir(dir).filePath(QStringLiteral("analysis.json"));
}

AnalysisCache::AnalysisCache(const QString &path) : path_(path.isEmpty() ? analysisCachePath() : path) {}

int AnalysisCache::load()
{
    entries_.clear();
    order_.clear();
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return 0;
    const QJsonObject entries = doc.object().value(QStringLiteral("entries")).toObject();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject raw = it.value().toObject();
        CachedModule entry;
        entry.size = qint64(raw.value(QStringLiteral("size")).toDouble());
        entry.mtime = raw.value(QStringLiteral("mtime")).toDouble();
        const QJsonValue dur = raw.value(QStringLiteral("dur"));
        if (dur.isString() && dur.toString() == QLatin1String("inf"))
            entry.duration = 1e18;
        else if (dur.isDouble() && dur.toDouble() >= 0.0)
            entry.duration = dur.toDouble();
        else
            entry.duration = -1.0;
        entry.fmt = raw.value(QStringLiteral("fmt")).toString();
        entry.channels = raw.value(QStringLiteral("ch")).isDouble()
                             ? int(raw.value(QStringLiteral("ch")).toDouble()) : -1;
        entry.subsongs = raw.value(QStringLiteral("sub")).isDouble()
                             ? qMax(0, int(raw.value(QStringLiteral("sub")).toDouble())) : 1;
        entry.title = raw.value(QStringLiteral("title")).toString();
        entry.broken = raw.value(QStringLiteral("broken")).toString();
        const QJsonValue chValue = raw.value(QStringLiteral("ch"));
        const QJsonValue subValue = raw.value(QStringLiteral("sub"));
        const bool badCount = [](const QJsonValue &v) {
            return v.isDouble() && (v.toDouble() < 0 || v.toDouble() != std::trunc(v.toDouble()));
        }(chValue) || [](const QJsonValue &v) {
            return v.isDouble() && (v.toDouble() < 0 || v.toDouble() != std::trunc(v.toDouble()));
        }(subValue);
        if (badCount || (dur.isDouble() && dur.toDouble() < 0.0))
            continue;   // unusable record: force a fresh analysis like the Tk app
        entries_.insert(it.key(), entry);
        order_ << it.key();
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
    // touched: LRU move to end
    order_.removeOne(path);
    order_ << path;
    return true;
}

void AnalysisCache::remember(const QString &path, const CachedModule &entry)
{
    auto it = entries_.find(path);
    if (it != entries_.end() && it.value().size == entry.size && it.value().mtime == entry.mtime
        && it.value().fmt == entry.fmt && it.value().title == entry.title
        && it.value().broken == entry.broken && it.value().channels == entry.channels
        && it.value().subsongs == entry.subsongs && it.value().duration == entry.duration)
        return;
    entries_[path] = entry;
    order_.removeOne(path);
    order_ << path;
    dirty_ = true;
}

bool AnalysisCache::save()
{
    if (!dirty_)
        return false;
    // prune cold entries beyond the cap
    static constexpr int kMaxEntries = 50000;
    while (order_.size() > kMaxEntries) {
        entries_.remove(order_.takeFirst());
    }
    QJsonObject entries;
    for (const QString &path : order_) {
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
        if (entry.channels >= 0)
            raw.insert(QStringLiteral("ch"), entry.channels);
        else
            raw.insert(QStringLiteral("ch"), QJsonValue::Null);
        raw.insert(QStringLiteral("sub"), entry.subsongs);
        raw.insert(QStringLiteral("title"), entry.title);
        if (entry.broken.isEmpty())
            raw.insert(QStringLiteral("broken"), QJsonValue::Null);
        else
            raw.insert(QStringLiteral("broken"), entry.broken);
        entries.insert(path, raw);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("entries"), entries);
    if (atomicWrite(path_, compactJson(payload).toUtf8())) {
        dirty_ = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PlaylistStore
// ---------------------------------------------------------------------------

PlaylistStore::PlaylistStore(const QString &path) : path_(path.isEmpty() ? playlistsPath() : path)
{
    load();
}

void PlaylistStore::reindex()
{
    index_.clear();
    for (int i = 0; i < playlists_.size(); ++i)
        index_.insert(key(playlists_[i].name), i);
}

bool PlaylistStore::load()
{
    playlists_.clear();
    reindex();
    // Favorites always exists (may be filled from the file below)
    Playlist favorites;
    favorites.name = QString::fromLatin1(kFavoritesName);
    playlists_.append(favorites);
    reindex();

    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return false;
    const QJsonArray items = doc.object().value(QStringLiteral("playlists")).toArray();
    QStringList seen;   // the file's own Favorites entry replaces the stub below
    for (const QJsonValue &item : items) {
        if (!item.isObject())
            continue;
        const QJsonObject raw = item.toObject();
        QString nameError;
        const QString name = normalizeName(raw.value(QStringLiteral("name")).toString(), &nameError);
        if (!nameError.isEmpty())
            continue;   // invalid name drops the entry (Python: normalise_name raised)
        const QString k = key(name);
        if (seen.contains(k))
            continue;
        seen << k;
        Playlist playlist;
        playlist.name = name;
        playlist.root = raw.value(QStringLiteral("root")).toString();
        QSet<QString> unique;
        for (const QJsonValue &p : raw.value(QStringLiteral("paths")).toArray()) {
            const QString text = p.toVariant().toString();
            if (!text.isEmpty() && !unique.contains(text)) {
                unique.insert(text);
                playlist.paths << text;
            }
            if (playlist.paths.size() >= 100000)
                break;
        }
        if (k == key(favorites.name))
            playlists_[0] = playlist;
        else
            playlists_.append(playlist);
    }
    reindex();
    return true;
}

QString PlaylistStore::normalizeName(const QString &nameIn, QString *errorOut)
{
    QString text = nameIn.trimmed();
    auto fail = [&](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return QString();
    };
    if (text.isEmpty())
        return fail(QObject::tr("A playlist needs a name"));
    if (text.size() > 80)
        return fail(QObject::tr("A name is 80 characters at most"));
    if (text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('\\')))
        return fail(QObject::tr("A playlist name cannot contain a slash"));
    if (text == QLatin1String(".") || text == QLatin1String(".."))
        return fail(QObject::tr("That name is reserved"));
    if (errorOut)
        errorOut->clear();
    return text;
}

bool PlaylistStore::save() const
{
    QJsonArray items;
    for (const Playlist &playlist : playlists_) {
        // The Python version skips writing the bare Favorites stub? It always
        // writes every playlist (Favorites included) - keep it identical.
        QJsonObject raw;
        raw.insert(QStringLiteral("name"), playlist.name);
        raw.insert(QStringLiteral("root"), playlist.root);
        QJsonArray paths;
        for (const QString &path : playlist.paths)
            paths.append(path);
        raw.insert(QStringLiteral("paths"), paths);
        items.append(raw);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("playlists"), items);
    return atomicWrite(path_, indentJson(payload).toUtf8());
}

bool PlaylistStore::isFavorites(const QString &name)
{
    return name.trimmed().toLower() == QLatin1String("favorites");
}

const Playlist *PlaylistStore::get(const QString &name) const
{
    auto it = index_.constFind(key(name));
    return it == index_.constEnd() ? nullptr : &playlists_[it.value()];
}

Playlist *PlaylistStore::getMutable(const QString &name)
{
    auto it = index_.constFind(key(name));
    return it == index_.constEnd() ? nullptr : &playlists_[it.value()];
}

QStringList PlaylistStore::names() const
{
    QStringList out;
    for (const Playlist &playlist : playlists_)
        out << playlist.name;
    return out;
}

bool PlaylistStore::create(const QString &nameIn, const QStringList &paths, const QString &root)
{
    QString nameError;
    const QString name = normalizeName(nameIn, &nameError);
    if (!nameError.isEmpty()) {
        error = nameError;
        return false;
    }
    if (has(name)) {
        error = QObject::tr("A playlist called '%1' already exists").arg(name);
        return false;
    }
    if (playlists_.size() - 1 >= 500) {
        error = QObject::tr("500 playlists is the limit");
        return false;
    }
    if (paths.size() > 100000) {
        error = QObject::tr("100000 tracks is the playlist limit");
        return false;
    }
    Playlist playlist;
    playlist.name = name;
    playlist.root = root;
    QSet<QString> seen;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !seen.contains(path)) {
            seen.insert(path);
            playlist.paths << path;
        }
    }
    playlists_.append(playlist);
    reindex();
    save();
    error.clear();
    return true;
}

bool PlaylistStore::replace(const QString &nameIn, const QStringList &paths, const QString &root)
{
    Playlist *playlist = getMutable(nameIn);
    if (!playlist) {
        error = QObject::tr("No playlist called '%1'").arg(nameIn);
        return false;
    }
    QSet<QString> seen;
    QStringList unique;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !seen.contains(path)) {
            seen.insert(path);
            unique << path;
        }
    }
    if (unique.size() > 100000) {
        error = QObject::tr("100000 tracks is the playlist limit");
        return false;
    }
    playlist->paths = unique;
    if (!root.isEmpty())
        playlist->root = root;
    save();
    error.clear();
    return true;
}

bool PlaylistStore::addPaths(const QString &nameIn, const QStringList &paths, const QString &root)
{
    Playlist *playlist = getMutable(nameIn);
    if (!playlist) {
        error = QObject::tr("No playlist called '%1'").arg(nameIn);
        return false;
    }
    QSet<QString> existing(playlist->paths.begin(), playlist->paths.end());
    for (const QString &path : paths) {
        if (!path.isEmpty() && !existing.contains(path)) {
            existing.insert(path);
            playlist->paths << path;
        }
    }
    if (!root.isEmpty() && playlist->root.isEmpty())
        playlist->root = root;
    save();
    return true;
}

bool PlaylistStore::removePaths(const QString &nameIn, const QSet<QString> &paths)
{
    Playlist *playlist = getMutable(nameIn);
    if (!playlist) {
        error = QObject::tr("No playlist called '%1'").arg(nameIn);
        return false;
    }
    QStringList kept;
    for (const QString &path : playlist->paths) {
        if (!paths.contains(path))
            kept << path;
    }
    playlist->paths = kept;
    save();
    return true;
}

bool PlaylistStore::rename(const QString &oldName, const QString &newNameIn)
{
    if (isFavorites(oldName)) {
        error = QObject::tr("Favorites cannot be renamed");
        return false;
    }
    QString nameError;
    const QString newName = normalizeName(newNameIn, &nameError);
    if (!nameError.isEmpty()) {
        error = nameError;
        return false;
    }
    Playlist *playlist = getMutable(oldName);
    if (!playlist) {
        error = QObject::tr("No playlist called '%1'").arg(oldName);
        return false;
    }
    if (has(newName)) {
        error = QObject::tr("A playlist called '%1' already exists").arg(newName);
        return false;
    }
    playlist->name = newName;
    reindex();
    save();
    error.clear();
    return true;
}

bool PlaylistStore::remove(const QString &nameIn)
{
    const QString name = nameIn.trimmed();
    if (isFavorites(name)) {
        error = QObject::tr("Favorites cannot be deleted, clear it instead");
        return false;
    }
    auto it = index_.constFind(key(name));
    if (it == index_.constEnd()) {
        error = QObject::tr("No playlist called '%1'").arg(name);
        return false;
    }
    playlists_.remove(it.value());
    reindex();
    save();
    error.clear();
    return true;
}

bool PlaylistStore::toggleFavorite(const QString &path)
{
    Playlist *favorites = getMutable(QString::fromLatin1(kFavoritesName));
    if (!favorites || path.isEmpty())
        return false;
    const int index = favorites->paths.indexOf(path);
    if (index >= 0) {
        favorites->paths.removeAt(index);
        save();
        return false;
    }
    if (favorites->paths.size() >= 100000) {
        error = QObject::tr("100000 tracks is the playlist limit");
        return false;
    }
    favorites->paths << path;
    save();
    return true;
}

bool PlaylistStore::isFavorite(const QString &path) const
{
    const Playlist *favorites = get(QString::fromLatin1(kFavoritesName));
    return favorites && favorites->paths.contains(path);
}

bool PlaylistStore::clearFavorites()
{
    Playlist *favorites = getMutable(QString::fromLatin1(kFavoritesName));
    if (!favorites)
        return false;
    favorites->paths.clear();
    save();
    return true;
}

// ---------------------------------------------------------------------------
// M3U
// ---------------------------------------------------------------------------

int writeM3u(const QStringList &paths, const QString &path)
{
    QStringList lines{QStringLiteral("#EXTM3U")};
    int count = 0;
    for (const QString &entry : paths) {
        const QString text = entry.trimmed();
        if (text.isEmpty())
            continue;
        lines << text;
        ++count;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 0;
    file.write((lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8());
    file.close();
    return count;
}

QStringList readM3u(const QString &path, int *skippedOut)
{
    QStringList out;
    int skipped = 0;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (skippedOut)
            *skippedOut = 0;
        return out;
    }
    const QString base = QFileInfo(path).absolutePath();
    QSet<QString> seen;
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    while (!stream.atEnd()) {
        QString entry = stream.readLine().trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')))
            continue;
        if (entry.size() >= 2 && (entry.front() == QLatin1Char('"') && entry.back() == entry.front()
                                  || entry.front() == QLatin1Char('\'') && entry.back() == entry.front()))
            entry = entry.mid(1, entry.size() - 2).trimmed();
        if (entry.isEmpty())
            continue;
        if (!entry.startsWith(QLatin1Char('/')))
            entry = QDir(base).filePath(entry);
        entry = QDir::cleanPath(entry);
        if (seen.contains(entry))
            continue;
        seen.insert(entry);
        if (QFileInfo(entry).isFile())
            out << entry;
        else
            ++skipped;
    }
    if (skippedOut)
        *skippedOut = skipped;
    return out;
}

// ---------------------------------------------------------------------------
// StatsStore
// ---------------------------------------------------------------------------

QString StatsStore::pathKey(const QString &path) { return absolutePathKey(path); }

StatsStore::StatsStore(const QString &path)
    : path_(path.isEmpty() ? statsPath() : path),
      since_(double(QDateTime::currentMSecsSinceEpoch()) / 1000.0)
{
    load();
}

namespace {
// stats.py _number(): finite, 0..1e15 or the default
double statsNumber(const QJsonValue &value, double fallback)
{
    if (value.isBool())
        return fallback;
    if (!value.isDouble())
        return fallback;
    const double v = value.toDouble();
    if (!std::isfinite(v) || v < 0.0 || v > 1e15)
        return fallback;
    return v;
}
}  // namespace

void StatsStore::load()
{
    records_.clear();
    QFileInfo info(path_);
    if (!info.exists())
        return;
    if (info.size() > 64LL * 1024 * 1024) {
        error = QObject::tr("Could not read listening stats: file is too large. Existing file left untouched.");
        blocked_ = true;
        return;
    }
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Could not read listening stats: %1. Existing file left untouched.").arg(file.errorString());
        blocked_ = true;
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = doc.isObject() ? doc.object() : QJsonObject{};
    const QJsonArray modulesArray = root.value(QStringLiteral("modules")).toArray();
    const bool badFormat = parseError.error != QJsonParseError::NoError || !doc.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("modules")).isArray();
    if (!badFormat && modulesArray.size() > 100000) {
        error = QObject::tr("Could not read listening stats: Too many modules. Existing file left untouched.");
        blocked_ = true;
        return;
    }
    if (badFormat) {
        error = QObject::tr("Could not read listening stats: Unrecognised statistics format. "
                            "Existing file left untouched.");
        blocked_ = true;
        return;
    }
    if (root.contains(QStringLiteral("since")))
        since_ = statsNumber(root.value(QStringLiteral("since")), since_);
    for (const QJsonValue &item : modulesArray) {
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
        if (records_.size() >= 100000) {
            error = QObject::tr("Listening stats limit reached, existing history is kept.");
            return false;
        }
        it = records_.insert(key, ModuleStats{key});
    }
    ModuleStats &row = it.value();
    row.seconds += qMax(0.0, seconds);
    if (play)
        row.plays += 1;
    row.lastPlayed = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
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
    if (atomicWrite(path_, compactJson(payload).toUtf8())) {
        dirty_ = false;
        error.clear();
        return true;
    }
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
    while (rows.size() > limit)
        rows.remove(limit);
    return rows;
}

void StatsStore::resetAll()
{
    // stats.py reset(): the empty file is written before the memory clears, and
    // an unreadable previous file is replaced on purpose
    records_.clear();
    since_ = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    dirty_ = true;
    blocked_ = false;
    save();
}

double StatsStore::totalSeconds() const
{
    double total = 0.0;
    for (const ModuleStats &row : records_)
        total += row.seconds;
    return total;
}

// ---------------------------------------------------------------------------
// IgnoreStore
// ---------------------------------------------------------------------------

QString IgnoreStore::pathKey(const QString &path) { return absolutePathKey(path); }

IgnoreStore::IgnoreStore(const QString &path) : path_(path.isEmpty() ? ignoredPath() : path)
{
    QFile file(path_);
    if (!file.exists())
        return;   // missing == empty list
    auto refuse = [this](const QString &reason) {
        readError_ = true;
        error = QObject::tr("Could not read ignored songs: %1. File kept unchanged: %2")
                    .arg(reason, path_);
    };
    if (!file.open(QIODevice::ReadOnly)) {
        refuse(QObject::tr("Permission denied"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = doc.isObject() ? doc.object() : QJsonObject{};
    const QJsonArray array = root.value(QStringLiteral("paths")).toArray();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("paths")).isArray() || array.size() > 100000) {
        refuse(QObject::tr("Invalid ignore-list format"));
        return;
    }
    for (const QJsonValue &item : array) {
        // ignored.py: every entry must be a non-empty absolute path
        if (!item.isString() || item.toString().isEmpty()
            || !item.toString().startsWith(QLatin1Char('/'))) {
            refuse(QObject::tr("Ignored paths must be absolute file paths"));
            return;
        }
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
    if (next.size() > 100000) {
        error = QObject::tr("The ignore list is limited to 100000 songs");
        return false;
    }
    if (next == paths_)
        return false;   // change() returns False when there is nothing to do (python parity)
    QJsonArray array;
    QStringList sorted = next.values();
    sorted.sort();
    for (const QString &path : sorted)
        array.append(path);
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("paths"), array);
    if (!atomicWrite(path_, (indentJson(payload) + QLatin1Char('\n')).toUtf8())) {
        error = QObject::tr("Could not save ignored songs: %1").arg(path_);
        return false;
    }
    paths_ = next;
    error.clear();
    return true;
}
