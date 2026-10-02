// Saved playlists (playlists.py), stored in playlists.json, and M3U import/export.
#pragma once

#include "casefold.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

QString playlistsPath();   // <config dir>/playlists.json

struct Playlist {
    QString name;
    QStringList paths;
    QString root;
};

class PlaylistStore {
public:
    static constexpr const char *kFavoritesName = "Favorites";
    static constexpr int kMaxTracks = 100000;      // per playlist
    explicit PlaylistStore(const QString &path = QString());

    bool load();
    bool save() const;

    QStringList names() const;                     // in file order, Favorites first
    const Playlist *get(const QString &name) const;
    Playlist *getMutable(const QString &name);
    bool has(const QString &name) const { return get(name) != nullptr; }
    static bool isFavorites(const QString &name);

    QString error;
    bool readError() const { return readError_; }   // the file exists but couldn't be read

    // The mutators save at once; they report the edit, not whether saving worked.
    bool create(const QString &name, const QStringList &paths, const QString &root = QString());
    bool replace(const QString &name, const QStringList &paths, const QString &root = QString());
    bool rename(const QString &oldName, const QString &newName);
    bool remove(const QString &name);
    bool addPaths(const QString &name, const QStringList &paths, const QString &root = QString());
    bool removePaths(const QString &name, const QSet<QString> &paths);
    bool toggleFavorite(const QString &path);      // returns true when now favourited
    bool isFavorite(const QString &path) const;
    bool clearFavorites();

    static QString normalizeName(const QString &name, QString *errorOut = nullptr);
    // A free name from `base`: base itself, or base + suffix (e.g. " %1" or
    // " (%1)") counting from 2, shortened so it stays within 80 characters.
    // Empty (with errorOut) when base isn't a valid name.
    QString uniqueName(const QString &base, const QString &suffix, QString *errorOut = nullptr) const;

private:
    static QString key(const QString &name) { return caseFold(name.trimmed()); }
    Playlist *find(const QString &name);           // sets error when there is none
    QString path_;
    bool readError_ = false;                       // file exists but could not be trusted
    QVector<Playlist> playlists_;                  // insertion order, Favorites first
    QHash<QString, int> index_;                    // key -> index in playlists_
    void reindex();
};

// M3U import/export (paths resolved beside the file; missing counted)
int writeM3u(const QStringList &paths, const QString &path);
QStringList readM3u(const QString &path, int *skippedOut = nullptr);
