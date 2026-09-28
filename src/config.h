// Settings mirroring modjuke/config.py: the same JSON file, keys and defaults.
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

QString modjukeConfigDir();          // $XDG_CONFIG_HOME/modjuke or ~/.config/modjuke
QString modjukeConfigPath();         // <dir>/config.json

struct Settings {
    // library
    QString lastDirectory;
    QString lastPickerDir;
    QString folderPicker = QStringLiteral("built-in");

    int uiFps = 60;
    bool smoothTrackerScrolling = false;

    QString windowTitleMode = QStringLiteral("track");   // track|title|filename|none
    QString queueMode = QStringLiteral("by directory");  // order for the library source
    QString queueSource;              // "" = Library, otherwise a playlist name
    QString activePlaylist;
    QStringList filterFormats;
    double filterMin = 0.0;
    double filterMax = 0.0;
    bool filterHideBroken = false;
    qint64 shuffleSeed = 0;
    QStringList shufflePaths;         // remembered drawn shuffle order (paths)

    // playback
    int volume = 80;                  // 0..100
    bool muted = false;
    bool loopTrack = false;
    bool loopQueue = false;
    bool autoAdvance = true;
    int subsong = 0;                  // zero-based selection paired with lastPath/lastPosition

    // audio ("auto" = default output, "null" = silent)
    QString backend = QStringLiteral("auto");
    int samplerate = 0;               // 0 = device default
    int bufferMs = 220;
    int latencyMs = 20;
    int latencyRevision = 1;
    QString interpolation = QStringLiteral("sinc");   // off|linear|cubic|sinc

    // safety knobs kept for file compatibility (no watchdog in this port)
    double stallTimeout = 45.0;
    double silenceStallTimeout = 25.0;
    double hangTimeout = 8.0;
    int maxRestarts = 3;
    bool autoSkipBroken = true;
    bool overrunGuard = true;

    // ui
    QString windowGeometry;           // e.g. "1200x740+100+80"
    bool showMetadata = true;
    QString theme = QStringLiteral("dark");
    QJsonObject customThemes;
    bool rememberPosition = true;
    bool cacheAnalysis = true;
    bool autoAnalyze = true;
    bool trackListeningStats = true;
    bool playAllSubsongs = false;    // Qt-only opt-in policy (qt-ui.json)
    QStringList hiddenQueueColumns;  // Qt-only optional column IDs; Module is never hidden
    bool confirmIgnore = true;        // Qt-only UI preference (qt-ui.json)
    QString lastPath;
    double lastPosition = 0.0;

    // unknown keys read from the file are preserved on save
    QJsonObject extras;

    static Settings load(const QString &path = QString());
    bool save(const QString &path = QString()) const;

    // value normalization helpers
    static int normalizeSampleRate(const QJsonValue &value);
    static int clampUiFps(const QJsonValue &value);
    static const QVector<int> &sampleRateChoices();
    static QString sampleRateLabel(int rate);
    static const QString &appName()
    {
        static const QString name = QStringLiteral("modjuke");
        return name;
    }
};
