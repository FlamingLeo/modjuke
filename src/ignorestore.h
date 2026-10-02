// Ignored songs (ignored.py): files left out of the library, stored in ignored.json.
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

QString ignoredPath();   // <config dir>/ignored.json

class IgnoreStore {
public:
    explicit IgnoreStore(const QString &path = QString());
    bool contains(const QString &path) const;
    QStringList paths() const;                    // sorted, display paths
    int count() const { return paths_.size(); }
    bool change(const QStringList &add, const QStringList &remove = {});
    QString error;

private:
    static constexpr int kMaxPaths = 100000;
    static QString pathKey(const QString &path);
    QString path_;
    QSet<QString> paths_;
    bool readError_ = false;   // file exists but could not be trusted
};
