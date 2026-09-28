// Persistent shuffle orders, one per queue source (library folder or playlist).
// Port of modjuke/shuffles.py: a drawn order is only replaced when a new one is
// explicitly drawn ("Shuffle now", or a queue repeat in shuffle mode), so
// switching sources, changing the sort order or restarting brings the same
// sequence back. Orders live in shuffles.json beside the settings file.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

class ShuffleStore {
public:
    explicit ShuffleStore(const QString &path = QString());

    static QString shufflesPath();                                  // <config dir>/shuffles.json
    static QString libraryKey(const QString &directory);            // "library:<abs path>"
    static QString playlistKey(const QString &name);                // "playlist:<casefold name>"

    bool contains(const QString &key) const { return orders_.contains(key); }
    QStringList get(const QString &key) const;                      // [] when never drawn

    /// Install `paths` as the order for `key` and persist it (no rewrite when
    /// identical). Least-recently drawn sources are dropped past 32.
    bool put(const QString &key, const QStringList &paths);
    bool rename(const QString &oldKey, const QString &newKey);
    bool remove(const QString &key);
    bool save();

    QString error;

private:
    static constexpr int kMaxSources = 32;
    static constexpr int kMaxPaths = 250000;

    void prune();

    QString path_;
    QStringList keyOrder_;                       // insertion order == least recent first
    QHash<QString, QStringList> orders_;
};
