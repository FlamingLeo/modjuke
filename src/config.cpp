#include "config.h"

#include "jsonfile.h"
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <initializer_list>

QString modjukeConfigDir()
{
    // os.path.expanduser(os.environ.get("XDG_CONFIG_HOME", "~/.config"))/modjuke
    QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    QString base = xdg.isEmpty() ? QDir(QDir::homePath()).filePath(QStringLiteral(".config"))
                                 : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("modjuke"));
}

QString modjukeConfigFile(const QString &name)
{
    return QDir(modjukeConfigDir()).filePath(name);
}

QString modjukeConfigPath()
{
    return modjukeConfigFile(QStringLiteral("config.json"));
}

const QVector<int> &Settings::sampleRateChoices()
{
    static const QVector<int> choices = {0, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
    return choices;
}

QString Settings::sampleRateLabel(int rate)
{
    if (rate == 0)
        return QObject::tr("Device default");
    QString khz = QString::number(rate / 1000.0, 'g', 6);
    return QObject::tr("%1 kHz").arg(khz);
}

int Settings::normalizeSampleRate(const QJsonValue &value)
{
    const double number = jsonNumber(value, 0.0);
    const int rate = clampedInt(number, 0, 1 << 30);
    return rate == number && sampleRateChoices().contains(rate) ? rate : 0;
}

int Settings::clampUiFps(const QJsonValue &value)
{
    bool ok = false;
    double number = 0;
    if (value.isDouble())
        number = value.toDouble();
    else if (value.isString())
        number = value.toString().toDouble(&ok);
    if ((!ok && !value.isDouble()) || !std::isfinite(number))
        return 60;
    const int fps = qRound(number);
    return qBound(5, fps, 120);
}

namespace {

// coercing, like Python's str(): numbers and bools become text; null or missing: fallback
QString asString(const QJsonObject &raw, const QString &key, const QString &fallback)
{
    const QJsonValue v = raw.value(key);
    if (v.isString())
        return v.toString();
    if (v.isNull() || v.isUndefined())
        return fallback;
    return v.toVariant().toString();
}

double asDouble(const QJsonObject &raw, const QString &key, double fallback)
{
    return jsonNumber(raw.value(key), fallback);
}

int asInt(const QJsonObject &raw, const QString &key, int fallback)
{
    return clampedInt(asDouble(raw, key, fallback), -2000000000, 2000000000);
}

bool asBool(const QJsonObject &raw, const QString &key, bool fallback)
{
    const QJsonValue v = raw.value(key);
    if (v.isBool())
        return v.toBool();
    if (v.isDouble())
        return v.toDouble() != 0.0;
    return fallback;
}

// `value` when it is one of `allowed`, else the fallback
QString oneOf(const QString &value, std::initializer_list<const char *> allowed, const char *fallback)
{
    for (const char *choice : allowed)
        if (value == QLatin1String(choice))
            return value;
    return QString::fromLatin1(fallback);
}

// Stable IDs only; an empty/missing list means all columns are visible.
// Ignore Module, unknown values, duplicates and malformed entries.
QStringList cleanHiddenQueueColumns(const QJsonArray &array)
{
    const QStringList known{QStringLiteral("folder"), QStringLiteral("length"), QStringLiteral("format"),
                            QStringLiteral("channels"), QStringLiteral("subsongs")};
    QStringList result;
    for (const QString &key : known)
        if (array.contains(key)) result << key;
    return result;
}

}  // namespace

Settings Settings::load(const QString &pathIn)
{
    Settings s;
    const QString path = pathIn.isEmpty() ? modjukeConfigPath() : pathIn;
    // Keep Qt-only preferences outside shared config.json: Python drops
    // unknown config keys when saving, which must not re-enable this prompt.
    const QJsonObject ui = readJsonObject(QFileInfo(path).dir().filePath(QStringLiteral("qt-ui.json"))).root;
    s.hiddenQueueColumns = cleanHiddenQueueColumns(ui.value("hidden_queue_columns").toArray());
    if (ui.value("play_all_subsongs").isBool())
        s.playAllSubsongs = ui.value("play_all_subsongs").toBool();
    if (ui.value("confirm_ignore").isBool())
        s.confirmIgnore = ui.value("confirm_ignore").toBool();

    const JsonRead file = readJsonObject(path);
    if (file.status == JsonRead::Missing)
        return s;
    if (!file.ok()) {
        // A damaged config.json would load as defaults and be overwritten by
        // the next save: keep a copy beside it and say so.
        const QString reason = file.status == JsonRead::BadShape ? QObject::tr("not a settings object")
                                                                 : file.reason;
        const QString backup = path + QStringLiteral(".bad");
        QFile::remove(backup);
        const bool copied = QFile::copy(path, backup);
        s.loadWarning = copied ? QObject::tr("Could not read %1 (%2). Default settings are in use; "
                                             "the old file was kept as %3.").arg(path, reason, backup)
                               : QObject::tr("Could not read %1 (%2). Default settings are in use.")
                                     .arg(path, reason);
        return s;
    }
    const QJsonObject &raw = file.root;

    s.lastDirectory = asString(raw, "last_directory", s.lastDirectory);
    s.lastPickerDir = asString(raw, "last_picker_dir", s.lastPickerDir);
    s.folderPicker = asString(raw, "folder_picker", s.folderPicker);
    s.uiFps = clampUiFps(raw.value("ui_fps").isUndefined() ? QJsonValue(s.uiFps) : raw.value("ui_fps"));
    s.smoothTrackerScrolling = asBool(raw, "smooth_tracker_scrolling", s.smoothTrackerScrolling);
    s.windowTitleMode = asString(raw, "window_title", s.windowTitleMode);
    QString queueMode = asString(raw, "queue_mode", s.queueMode);
    if (queueMode == QLatin1String("alpha"))
        queueMode = QStringLiteral("alphabetical");
    if (queueMode == QLatin1String("saved"))
        queueMode = QStringLiteral("playlist");   // early C++ files; Python spells it "playlist"
    s.queueMode = oneOf(queueMode, {"alphabetical", "by directory", "shuffle", "playlist"}, "by directory");
    // Python stores queue_source as a *type* marker ("library"/"playlist") and the
    // playlist name in active_playlist. Internally queueSource holds the name
    // ("" = library), so that is the mapping direction that matters here.
    const QString sourceValue = asString(raw, "queue_source", QString());
    s.queueSource = asString(raw, "active_playlist", s.queueSource);
    if (sourceValue != QLatin1String("library") && sourceValue != QLatin1String("playlist")
        && s.queueSource.isEmpty())
        s.queueSource = sourceValue;   // older C++ files kept the name in queue_source

    for (const QString &item : jsonStringsCoerced(raw.value("filter_formats"))) {
        const QString fmt = item.trimmed().toLower();
        if (!fmt.isEmpty())
            s.filterFormats << fmt;
    }
    s.filterMin = std::clamp(asDouble(raw, "filter_min", 0.0), 0.0, 1e7);
    s.filterMax = std::clamp(asDouble(raw, "filter_max", 0.0), 0.0, 1e7);
    s.filterHideBroken = asBool(raw, "filter_hide_broken", s.filterHideBroken);
    s.shuffleSeed = qint64(std::clamp(asDouble(raw, "shuffle_seed", 0.0), -9.0e15, 9.0e15));
    s.shufflePaths = jsonStringsCoerced(raw.value("shuffle_paths"), 5000);

    s.volume = qBound(0, asInt(raw, "volume", 80), 100);
    s.muted = asBool(raw, "muted", s.muted);
    s.loopTrack = asBool(raw, "loop_track", s.loopTrack);
    s.loopQueue = asBool(raw, "loop_queue", s.loopQueue);
    s.autoAdvance = asBool(raw, "auto_advance", s.autoAdvance);
    s.subsong = qMax(0, asInt(raw, "subsong", 0));

    s.backend = oneOf(asString(raw, "backend", s.backend), {"auto", "sounddevice", "soundcard", "null"}, "auto");
    if (raw.contains("samplerate"))
        s.samplerate = normalizeSampleRate(raw.value("samplerate"));
    s.bufferMs = qBound(20, asInt(raw, "buffer_ms", 220), 5000);
    s.latencyMs = qBound(1, asInt(raw, "latency_ms", 20), 2000);
    s.latencyRevision = asInt(raw, "latency_revision", s.latencyRevision);
    // Old default upgrade, same rule as the Python loader: a file predating the
    // revision key that still holds the old 120ms default gets the 20ms one once.
    if (!raw.contains("latency_revision") && s.latencyMs == 120)
        s.latencyMs = 20;
    s.interpolation = oneOf(asString(raw, "interpolation", s.interpolation),
                            {"off", "linear", "cubic", "sinc"}, "sinc");

    s.stallTimeout = asDouble(raw, "stall_timeout", s.stallTimeout);
    s.silenceStallTimeout = asDouble(raw, "silence_stall_timeout", s.silenceStallTimeout);
    s.hangTimeout = asDouble(raw, "hang_timeout", s.hangTimeout);
    s.maxRestarts = qMax(0, asInt(raw, "max_restarts", s.maxRestarts));
    s.autoSkipBroken = asBool(raw, "auto_skip_broken", s.autoSkipBroken);
    s.overrunGuard = asBool(raw, "overrun_guard", s.overrunGuard);

    s.windowGeometry = asString(raw, "window_geometry", s.windowGeometry);
    s.showMetadata = asBool(raw, "show_metadata", s.showMetadata);
    // same cleaning + alias resolution as config.py: both apps persist the
    // canonical, validated shape
    s.customThemes = Palette::cleanCustomThemes(raw.value("custom_themes").toObject());
    s.theme = Palette::normalizeTheme(asString(raw, "theme", s.theme), s.customThemes);
    s.rememberPosition = asBool(raw, "remember_position", s.rememberPosition);
    s.cacheAnalysis = asBool(raw, "cache_analysis", s.cacheAnalysis);
    s.autoAnalyze = asBool(raw, "auto_analyze", true);   // default on, incl. older files
    s.trackListeningStats = asBool(raw, "track_listening_stats", s.trackListeningStats);
    s.lastPath = asString(raw, "last_path", s.lastPath);
    s.lastPosition = asDouble(raw, "last_position", 0.0);

    // save() writes the file back with its own keys on top, so keys this
    // version doesn't know (a newer one's, Python-only ones) survive
    s.extras = raw;
    return s;
}

bool Settings::save(const QString &pathIn) const
{
    const QString path = pathIn.isEmpty() ? modjukeConfigPath() : pathIn;
    QJsonObject raw = extras;   // the loaded file: unknown keys are kept
    // mirrors config.py fields (asdict order does not matter; keys sort)
    raw.insert("last_directory", lastDirectory);
    raw.insert("last_picker_dir", lastPickerDir);
    raw.insert("folder_picker", folderPicker);
    raw.insert("ui_fps", uiFps);
    raw.insert("smooth_tracker_scrolling", smoothTrackerScrolling);
    raw.insert("window_title", windowTitleMode);
    raw.insert("queue_mode", queueMode);
    raw.insert("queue_source", queueSource.isEmpty() ? QString() : QStringLiteral("playlist"));
    raw.insert("active_playlist", queueSource);
    QJsonArray formats;
    for (const QString &fmt : filterFormats)
        formats.append(fmt);
    raw.insert("filter_formats", formats);
    raw.insert("filter_min", filterMin);
    raw.insert("filter_max", filterMax);
    raw.insert("filter_hide_broken", filterHideBroken);
    raw.insert("shuffle_seed", double(shuffleSeed));
    QJsonArray shuffle;
    for (const QString &p : shufflePaths)
        shuffle.append(p);
    raw.insert("shuffle_paths", shuffle);
    raw.insert("volume", volume);
    raw.insert("muted", muted);
    raw.insert("loop_track", loopTrack);
    raw.insert("loop_queue", loopQueue);
    raw.insert("auto_advance", autoAdvance);
    raw.insert("subsong", subsong);
    raw.insert("backend", backend);
    raw.insert("samplerate", samplerate);
    raw.insert("buffer_ms", bufferMs);
    raw.insert("latency_ms", latencyMs);
    raw.insert("latency_revision", latencyRevision);
    raw.insert("interpolation", interpolation);
    raw.insert("stall_timeout", stallTimeout);
    raw.insert("silence_stall_timeout", silenceStallTimeout);
    raw.insert("hang_timeout", hangTimeout);
    raw.insert("max_restarts", maxRestarts);
    raw.insert("auto_skip_broken", autoSkipBroken);
    raw.insert("overrun_guard", overrunGuard);
    raw.insert("window_geometry", windowGeometry);
    raw.insert("show_metadata", showMetadata);
    raw.insert("theme", theme);
    raw.insert("custom_themes", customThemes);
    raw.insert("remember_position", rememberPosition);
    raw.insert("cache_analysis", cacheAnalysis);
    raw.insert("auto_analyze", autoAnalyze);
    raw.insert("track_listening_stats", trackListeningStats);
    raw.insert("last_path", lastPath);
    raw.insert("last_position", lastPosition);

    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString uiPath = QFileInfo(path).dir().filePath(QStringLiteral("qt-ui.json"));
    QJsonObject ui;
    const JsonRead oldUi = readJsonObject(uiPath);
    if (oldUi.ok()) {
        ui = oldUi.root;
    } else if (oldUi.status != JsonRead::Missing) {
        // An unreadable Qt preference file used to fail every save, so no
        // setting was saved anymore: keep it aside and write a fresh one.
        QFile::remove(uiPath + QStringLiteral(".bad"));
        if (!QFile::rename(uiPath, uiPath + QStringLiteral(".bad")))
            return false;
    }
    ui.insert("confirm_ignore", confirmIgnore);
    ui.insert("play_all_subsongs", playAllSubsongs);
    ui.insert("hidden_queue_columns", QJsonArray::fromStringList(
        cleanHiddenQueueColumns(QJsonArray::fromStringList(hiddenQueueColumns))));
    QSaveFile uiFile(uiPath);
    const QByteArray bytes = QJsonDocument(ui).toJson(QJsonDocument::Indented);
    // Validate and stage both files before committing either. In particular, an
    // unreadable Qt preference file must not publish unsaved theme drafts.
    QSaveFile file(path);
    const QByteArray configBytes = QJsonDocument(raw).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || !uiFile.open(QIODevice::WriteOnly)
        || file.write(configBytes) != configBytes.size() || uiFile.write(bytes) != bytes.size())
        return false;
    return file.commit() && uiFile.commit();
}
