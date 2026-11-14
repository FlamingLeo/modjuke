// Duplicate songs in playlists. Copies of one song share a key: a hash of the
// note data (every non-empty pattern cell along the order list), so renamed,
// re-saved and converted copies match, while sample names, comments and the
// file layout don't count. Modules with fewer than 16 such cells, and files
// libopenmpt can't load, use a hash of the file instead. Keys are kept in
// song-keys.json, valid while a file's size and mtime and the libopenmpt
// version stay the same.
#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

class OpenMPTLib;
class QThread;

QString songKeysPath();   // <config dir>/song-keys.json

// "n:" + hash of the notes, "f:" + hash of the file, or "" when it can't be read
QString songKey(const OpenMPTLib &lib, const QString &path);

using KeyOf = std::function<QString(const QString &path)>;   // "" matches nothing

// path -> the other paths of `paths` with the same key
QHash<QString, QStringList> copiesWithin(const QStringList &paths, const KeyOf &keyOf);
// (path, its copy) for each path of `adding` whose song is in `existing` or
// earlier in `adding` under another path; paths already listed are left out
QVector<QPair<QString, QString>> copiesOf(const QStringList &existing, const QStringList &adding,
                                          const KeyOf &keyOf);

class SongKeys : public QObject {
    Q_OBJECT
public:
    explicit SongKeys(const QString &path = QString(), QObject *parent = nullptr);
    ~SongKeys() override;

    QString key(const QString &path) const;   // the last known key, "" if none
    KeyOf keyOf() const;
    // Computes missing and outdated keys in the background, then emits
    // keysChanged(). Paths asked for while it runs are done next.
    void prepare(const QStringList &paths);
    // The same, now, on this thread (songs about to be added).
    void prepareNow(const QStringList &paths);

signals:
    void keysChanged();

private:
    struct Entry {
        qint64 size = 0;
        double mtime = 0.0;
        QString key;   // "": the file is gone
    };
    // the entries of `paths` that differ from `known`
    static QHash<QString, Entry> compute(const QHash<QString, Entry> &known, const QStringList &paths,
                                         const std::atomic_bool &cancel);
    void start(QStringList paths);
    bool apply(const QHash<QString, Entry> &changed);   // false: nothing changed
    void merge(const QHash<QString, Entry> &changed);   // apply, save, keysChanged()
    void save() const;

    QString path_;
    QString version_;                     // libopenmpt's; "" when it isn't available
    QHash<QString, Entry> entries_;
    QThread *thread_ = nullptr;
    QHash<QString, Entry> result_;        // the worker's, read after it finished
    QStringList pending_;                 // asked for while the worker runs
    std::atomic_bool cancel_{false};
};
