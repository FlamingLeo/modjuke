#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

// The keys of the top-level object member `member`, in file order. A small
// scanner over already-valid JSON (callers parse it first).
QStringList jsonMemberKeyOrder(const QByteArray &json, const QString &member)
{
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
    auto skipValue = [&] {
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

// a value as compact JSON text (a string becomes its quoted literal)
QByteArray compactJson(const QJsonValue &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);   // [...] -> ...
}

}  // namespace

JsonRead readJsonObject(const QString &path, qint64 maxBytes, int requireVersion)
{
    JsonRead read;
    const QFileInfo info(path);
    if (!info.exists())
        return read;
    if (maxBytes > 0 && info.size() > maxBytes) {
        read.status = JsonRead::TooLarge;
        return read;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        read.status = JsonRead::OpenFailed;
        read.reason = file.errorString();
        return read;
    }
    read.bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(read.bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        read.status = JsonRead::ParseFailed;
        read.reason = parseError.errorString();
        return read;
    }
    if (!doc.isObject()
        || (requireVersion && doc.object().value(QStringLiteral("version")).toInt() != requireVersion)) {
        read.status = JsonRead::BadShape;
        return read;
    }
    read.status = JsonRead::Ok;
    read.root = doc.object();
    return read;
}

bool writeFileAtomic(const QString &path, const QByteArray &bytes, WriteFailure *why)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (why)
            *why = {false, file.errorString(), int(file.error())};
        return false;
    }
    // a failed write also makes QSaveFile discard the temp file in commit()
    const bool written = file.write(bytes) == bytes.size();
    if (file.commit() && written)
        return true;
    if (why)
        *why = {true, file.errorString(), int(file.error())};
    return false;
}

JsonEntries jsonMemberEntries(const JsonRead &file, const QString &member)
{
    const QJsonObject object = file.root.value(member).toObject();
    QStringList order = jsonMemberKeyOrder(file.bytes, member);
    const QSet<QString> scanned(order.cbegin(), order.cend());
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!scanned.contains(it.key()))
            order << it.key();
    JsonEntries entries;
    entries.reserve(order.size());
    for (const QString &key : std::as_const(order)) {
        const auto it = object.constFind(key);
        if (it != object.constEnd())
            entries.append({key, it.value()});
    }
    return entries;
}

QByteArray versionedMemberJson(const QString &member, const JsonEntries &entries)
{
    QByteArray body = '{' + compactJson(member) + ":{";
    bool first = true;
    for (const auto &[key, value] : entries) {
        if (!first)
            body += ',';
        first = false;
        body += compactJson(key) + ':' + compactJson(value);
    }
    body += "},\"version\":1}";
    return body;
}

double jsonNumber(const QJsonValue &value, double fallback, double lo, double hi)
{
    if (!value.isDouble())
        return fallback;
    const double v = value.toDouble();
    return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
}

int clampedInt(double value, int lo, int hi)
{
    if (!std::isfinite(value))
        return lo;
    return int(std::clamp(value, double(lo), double(hi)));
}

QStringList jsonStringsCoerced(const QJsonValue &value, int maxCount)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toVariant().toString();
        if (!text.isEmpty())
            out << text;
        if (maxCount > 0 && out.size() >= maxCount)
            break;
    }
    return out;
}

QStringList jsonStringsStrict(const QJsonValue &value, int maxCount)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString())
            out << item.toString();
        if (maxCount > 0 && out.size() >= maxCount)
            break;
    }
    return out;
}

QString absolutePathKey(const QString &path, HomeTilde tilde)
{
    QString expanded = path;
    if (tilde == HomeTilde::Expand && expanded.startsWith(QLatin1Char('~'))) {
        if (expanded.size() == 1 || expanded.at(1) == QLatin1Char('/'))
            expanded = QDir::homePath() + expanded.mid(1);
    }
    return QDir::cleanPath(QFileInfo(expanded).absoluteFilePath());
}
