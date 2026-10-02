#include "stores.h"

#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>

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

// double -> integer without undefined behavior for huge or odd JSON values
int boundedInt(double v, int lo, int hi)
{
    if (!std::isfinite(v))
        return lo;
    return int(std::clamp(v, double(lo), double(hi)));
}

}  // namespace

QByteArray jsonStringLiteral(const QString &text)
{
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);   // ["..."] -> "..."
}

QStringList jsonMemberKeyOrder(const QByteArray &json, const QString &member)
{
    // A small scanner over already-valid JSON (callers parse it first): it
    // walks the top-level object, finds `member` and lists that object's keys.
    QStringList keys;
    const char *p = json.constData();
    const char *end = p + json.size();
    auto ws = [&] { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; };
    auto readString = [&]() -> QByteArray {   // at '"': returns the raw literal, quotes included
        const char *start = p++;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) ++p;
            ++p;
        }
        if (p < end) ++p;
        return QByteArray(start, int(p - start));
    };
    auto decode = [](const QByteArray &literal) {
        const QJsonDocument doc = QJsonDocument::fromJson("[" + literal + "]");
        return doc.isArray() ? doc.array().at(0).toString() : QString();
    };
    std::function<void()> skipValue = [&] {
        ws();
        if (p >= end) return;
        if (*p == '"') { readString(); return; }
        if (*p == '{' || *p == '[') {
            int depth = 0;
            while (p < end) {
                if (*p == '"') { readString(); continue; }
                if (*p == '{' || *p == '[') ++depth;
                else if (*p == '}' || *p == ']') { --depth; if (depth == 0) { ++p; return; } }
                ++p;
            }
            return;
        }
        while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;   // number/true/false/null
    };
    ws();
    if (p >= end || *p != '{') return keys;
    ++p;
    while (p < end) {
        ws();
        if (p >= end || *p != '"') break;
        const QString name = decode(readString());
        ws();
        if (p >= end || *p != ':') break;
        ++p;
        ws();
        if (name == member && p < end && *p == '{') {
            ++p;
            while (p < end) {
                ws();
                if (p >= end || *p != '"') break;
                keys << decode(readString());
                ws();
                if (p >= end || *p != ':') break;
                ++p;
                skipValue();
                ws();
                if (p < end && *p == ',') ++p;
            }
            return keys;
        }
        skipValue();
        ws();
        if (p < end && *p == ',') ++p;
    }
    return keys;
}

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
    used_.clear();
    clock_ = 0;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return 0;
    const QJsonObject entries = doc.object().value(QStringLiteral("entries")).toObject();
    // recency = order in the file (least recent first), as Python writes it
    for (const QString &key : jsonMemberKeyOrder(bytes, QStringLiteral("entries")))
        used_.insert(key, ++clock_);
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject raw = it.value().toObject();
        CachedModule entry;
        const double sizeValue = raw.value(QStringLiteral("size")).toDouble();
        entry.size = std::isfinite(sizeValue) ? qint64(std::clamp(sizeValue, -1.0, 9.0e15)) : -1;
        entry.mtime = raw.value(QStringLiteral("mtime")).toDouble();
        const QJsonValue dur = raw.value(QStringLiteral("dur"));
        if (dur.isString() && dur.toString() == QLatin1String("inf"))
            entry.duration = 1e18;
        else if (dur.isDouble() && dur.toDouble() >= 0.0)
            entry.duration = std::min(dur.toDouble(), 1e18);   // beyond = endless
        else
            entry.duration = -1.0;
        entry.fmt = raw.value(QStringLiteral("fmt")).toString();
        entry.channels = raw.value(QStringLiteral("ch")).isDouble()
                             ? boundedInt(raw.value(QStringLiteral("ch")).toDouble(), -1, 4096) : -1;
        entry.subsongs = raw.value(QStringLiteral("sub")).isDouble()
                             ? boundedInt(raw.value(QStringLiteral("sub")).toDouble(), 0, 1 << 20) : 1;
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
        if (!used_.contains(it.key()))
            used_.insert(it.key(), ++clock_);
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
    if (it != entries_.end() && it.value().size == entry.size && it.value().mtime == entry.mtime
        && it.value().fmt == entry.fmt && it.value().title == entry.title
        && it.value().broken == entry.broken && it.value().channels == entry.channels
        && it.value().subsongs == entry.subsongs && it.value().duration == entry.duration)
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
    // written in that order (QJsonObject would sort the keys alphabetically)
    QByteArray body = "{\"entries\":{";
    bool first = true;
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
        if (!first)
            body += ',';
        first = false;
        body += jsonStringLiteral(path) + ':' + QJsonDocument(raw).toJson(QJsonDocument::Compact);
    }
    body += "},\"version\":1}";
    if (atomicWrite(path_, body)) {
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
    readError_ = false;
    if (!file.exists())
        return false;   // first run: nothing saved yet
    // A file that exists but can't be read must survive: saving the
    // Favorites stub over it would delete every playlist.
    auto refuse = [this](const QString &reason) {
        readError_ = true;
        error = QObject::tr("Could not read playlists: %1. File kept unchanged, playlist "
                            "changes are not saved until it is fixed: %2").arg(reason, path_);
        return false;
    };
    if (!file.open(QIODevice::ReadOnly))
        return refuse(file.errorString());
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return refuse(parseError.errorString());
    if (!doc.isObject() || !doc.object().value(QStringLiteral("playlists")).isArray())
        return refuse(QObject::tr("unknown format"));
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
    if (text.toUcs4().size() > 80)   // code points, like Python's len()
        return fail(QObject::tr("A name is 80 characters at most"));
    if (text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('\\')))
        return fail(QObject::tr("A playlist name cannot contain a slash"));
    if (text == QLatin1String(".") || text == QLatin1String(".."))
        return fail(QObject::tr("That name is reserved"));
    if (errorOut)
        errorOut->clear();
    return text;
}

QString PlaylistStore::uniqueName(const QString &baseIn, const QString &suffix, QString *errorOut) const
{
    const QString base = normalizeName(baseIn, errorOut);
    if (base.isEmpty() || !has(base))
        return base;
    const QList<uint> points = base.toUcs4();
    for (int n = 2; n < 100000; ++n) {
        const QString tail = suffix.arg(n);
        const int room = std::max(1, 80 - int(tail.toUcs4().size()));
        const QString head = points.size() > room
                                 ? QString::fromUcs4(reinterpret_cast<const char32_t *>(points.constData()), room).trimmed()
                                 : base;
        const QString candidate = head + tail;
        if (!has(candidate) && normalizeName(candidate).size())
            return candidate;
    }
    if (errorOut)
        *errorOut = QObject::tr("No free playlist name");
    return QString();
}

bool PlaylistStore::save() const
{
    if (readError_)
        return false;   // the damaged file stays untouched (see load)
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
    // a case-only change ("rock" -> "Rock") finds the playlist itself
    if (has(newName) && key(newName) != key(playlist->name)) {
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
    // temp + rename: a full disk must not leave a truncated list behind
    if (!atomicWrite(path, (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8()))
        return -1;
    return count;
}

QStringList readM3u(const QString &path, int *skippedOut)
{
    QStringList out;
    int skipped = 0;
    QFile file(path);
    // a playlist is small; a huge file picked by mistake must not be read whole
    if (QFileInfo(path).size() > (64 << 20) || !file.open(QIODevice::ReadOnly)) {
        if (skippedOut)
            *skippedOut = 0;
        return out;
    }
    const QByteArray bytes = file.readAll();
    // UTF-8, or (old Windows/DOS-era lists) Latin-1 when it isn't valid UTF-8
    QStringDecoder utf8(QStringConverter::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    const QString base = QFileInfo(path).absolutePath();
    QSet<QString> seen;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        QString entry = line.trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')))
            continue;
        if (entry.size() >= 2 && (entry.front() == QLatin1Char('"') && entry.back() == entry.front()
                                  || entry.front() == QLatin1Char('\'') && entry.back() == entry.front()))
            entry = entry.mid(1, entry.size() - 2).trimmed();
        if (entry.isEmpty())
            continue;
        if (entry.startsWith(QLatin1String("file://")))
            entry = QUrl(entry).toLocalFile();
        if (!entry.startsWith(QLatin1Char('/')))
            entry = QDir(base).filePath(entry);
        entry = QDir::cleanPath(entry);
        // relative entries written on Windows use backslashes
        if (entry.contains(QLatin1Char('\\')) && !QFileInfo::exists(entry)) {
            const QString slashed = QDir::cleanPath(QString(entry).replace(QLatin1Char('\\'), QLatin1Char('/')));
            if (QFileInfo::exists(slashed))
                entry = slashed;
        }
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
    since_ = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
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
