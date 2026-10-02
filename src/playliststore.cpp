#include "playliststore.h"

#include "config.h"
#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>

namespace {

// order kept, empty entries and repeats dropped
QStringList uniquePaths(QStringList paths)
{
    paths.removeAll(QString());
    paths.removeDuplicates();
    return paths;
}

QString trackLimitError() { return QObject::tr("100000 tracks is the playlist limit"); }

}  // namespace

QString playlistsPath() { return modjukeConfigFile(QStringLiteral("playlists.json")); }

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
    // Favorites always exists (may be filled from the file below)
    Playlist favorites;
    favorites.name = QString::fromLatin1(kFavoritesName);
    playlists_ = {favorites};
    reindex();
    readError_ = false;

    const JsonRead file = readJsonObject(path_);
    if (file.status == JsonRead::Missing)
        return false;   // first run: nothing saved yet
    const QJsonValue items = file.root.value(QStringLiteral("playlists"));
    if (!file.ok() || !items.isArray()) {
        // A file that exists but can't be read must survive: saving the
        // Favorites stub over it would delete every playlist.
        const QString reason = file.status == JsonRead::OpenFailed || file.status == JsonRead::ParseFailed
                                   ? file.reason : QObject::tr("unknown format");
        readError_ = true;
        error = QObject::tr("Could not read playlists: %1. File kept unchanged, playlist "
                            "changes are not saved until it is fixed: %2").arg(reason, path_);
        return false;
    }
    QSet<QString> seen;   // the file's own Favorites entry replaces the stub below
    for (const QJsonValue &item : items.toArray()) {
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
        seen.insert(k);
        Playlist playlist;
        playlist.name = name;
        playlist.root = raw.value(QStringLiteral("root")).toString();
        playlist.paths = uniquePaths(jsonStringsCoerced(raw.value(QStringLiteral("paths"))));
        if (playlist.paths.size() > kMaxTracks)
            playlist.paths.resize(kMaxTracks);
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
    // every playlist, the Favorites stub included, like the Python version
    QJsonArray items;
    for (const Playlist &playlist : playlists_) {
        QJsonObject raw;
        raw.insert(QStringLiteral("name"), playlist.name);
        raw.insert(QStringLiteral("root"), playlist.root);
        raw.insert(QStringLiteral("paths"), QJsonArray::fromStringList(playlist.paths));
        items.append(raw);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("playlists"), items);
    return writeFileAtomic(path_, QJsonDocument(payload).toJson(QJsonDocument::Indented));
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

Playlist *PlaylistStore::find(const QString &name)
{
    Playlist *playlist = getMutable(name);
    if (!playlist)
        error = QObject::tr("No playlist called '%1'").arg(name);
    return playlist;
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
    if (paths.size() > kMaxTracks) {   // counted before dropping repeats
        error = trackLimitError();
        return false;
    }
    playlists_.append(Playlist{name, uniquePaths(paths), root});
    reindex();
    save();
    error.clear();
    return true;
}

bool PlaylistStore::replace(const QString &nameIn, const QStringList &paths, const QString &root)
{
    Playlist *playlist = find(nameIn);
    if (!playlist)
        return false;
    const QStringList unique = uniquePaths(paths);
    if (unique.size() > kMaxTracks) {
        error = trackLimitError();
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
    Playlist *playlist = find(nameIn);
    if (!playlist)
        return false;
    playlist->paths = uniquePaths(playlist->paths + paths);
    if (!root.isEmpty() && playlist->root.isEmpty())
        playlist->root = root;
    save();
    return true;
}

bool PlaylistStore::removePaths(const QString &nameIn, const QSet<QString> &paths)
{
    Playlist *playlist = find(nameIn);
    if (!playlist)
        return false;
    playlist->paths.removeIf([&paths](const QString &path) { return paths.contains(path); });
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
    Playlist *playlist = find(oldName);
    if (!playlist)
        return false;
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
    if (!find(name))
        return false;
    playlists_.remove(index_.value(key(name)));
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
    if (favorites->paths.size() >= kMaxTracks) {
        error = trackLimitError();
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
    for (const QString &entry : paths) {
        const QString text = entry.trimmed();
        if (!text.isEmpty())
            lines << text;
    }
    if (!writeFileAtomic(path, (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8()))
        return -1;
    return lines.size() - 1;
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
        if (entry.size() >= 2 && (entry.front() == QLatin1Char('"') || entry.front() == QLatin1Char('\''))
            && entry.back() == entry.front())
            entry = entry.mid(1, entry.size() - 2).trimmed();
        if (entry.isEmpty())
            continue;
        if (entry.startsWith(QLatin1String("file://")))
            entry = QUrl(entry).toLocalFile();
        if (!QDir::isAbsolutePath(entry))   // also C:\... on Windows
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
