// Plumbing shared by the JSON stores (config.json and the files beside it):
// reading a file into an object, atomic replacement, objects whose key order
// matters, and the lenient value readers that mirror the Python loaders.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>

// --- reading ------------------------------------------------------------------
struct JsonRead {
    enum Status {
        Missing,       // no file yet: a first run, not an error
        Ok,
        OpenFailed,    // reason: Qt's error text
        TooLarge,      // bigger than the caller's cap
        ParseFailed,   // reason: the parser's error text
        BadShape,      // not an object, or not the required version
    };
    Status status = Missing;
    QByteArray bytes;  // the raw file, for jsonMemberEntries()
    QJsonObject root;  // empty unless Ok
    QString reason;

    bool ok() const { return status == Ok; }
};

// maxBytes 0 = no cap; requireVersion 0 = any, else root["version"] must equal it
JsonRead readJsonObject(const QString &path, qint64 maxBytes = 0, int requireVersion = 0);

// --- writing ------------------------------------------------------------------
struct WriteFailure {
    bool opened = false;   // false: the temp file could not even be created
    QString text;          // Qt's error text
    int code = 0;          // QFileDevice::FileError
};

// Replaces `path` through a temp file and a rename, so a full disk never leaves
// a truncated file behind; creates the directory first.
bool writeFileAtomic(const QString &path, const QByteArray &bytes, WriteFailure *why = nullptr);

// --- objects in file order -------------------------------------------------------
// QJsonObject sorts its keys, but the Python files keep insertion order and the
// stores use it for least-recently-used pruning.
using JsonEntries = QVector<QPair<QString, QJsonValue>>;

// The members of the object root[member], in file order (keys the scanner
// missed follow in sorted order).
JsonEntries jsonMemberEntries(const JsonRead &file, const QString &member);

// {"<member>":{<entries in this order>},"version":1}, compact like the Python writers
QByteArray versionedMemberJson(const QString &member, const JsonEntries &entries);

// --- lenient value readers ---------------------------------------------------------
// A JSON number within [lo, hi], else fallback (bools, strings and null don't count).
double jsonNumber(const QJsonValue &value, double fallback,
                  double lo = -std::numeric_limits<double>::infinity(),
                  double hi = std::numeric_limits<double>::infinity());
// double -> int clamped to [lo, hi], without undefined behavior for huge
// values; a non-finite value gives lo
int clampedInt(double value, int lo, int hi);
// An array's items as text (numbers and bools converted), empty ones dropped;
// maxCount 0 = no limit. Not an array: empty.
QStringList jsonStringsCoerced(const QJsonValue &value, int maxCount = 0);
// Only an array's string items, empty ones kept; maxCount 0 = no limit.
QStringList jsonStringsStrict(const QJsonValue &value, int maxCount = 0);

// --- path keys ---------------------------------------------------------------------
// The absolute, cleaned path the stores key files and folders by
// (os.path.abspath; Expand also does os.path.expanduser). "" stays "".
enum class HomeTilde { Keep, Expand };
QString absolutePathKey(const QString &path, HomeTilde tilde);
