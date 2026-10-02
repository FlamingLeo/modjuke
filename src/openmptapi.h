// Dynamic loader + thin C++ wrapper around the libopenmpt shared library.
// Mirrors modjuke/openmpt.py: the library is discovered at runtime
// (MODJUKE_LIBOPENMPT override, then soname fallbacks) so the build never
// hard-links against libopenmpt.
#pragma once

#include <QByteArray>
#include <QLibrary>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

// One analyzed module (the shape the analysis cache, song info and UI use).
struct ModuleInfo {
    QString path;
    QString title;
    QString format;        // short type, e.g. "xm"
    QString formatLong;    // long type, e.g. "FastTracker II v2.00"
    QString tracker;
    QString artist;
    double duration = 0.0;   // seconds; <=0 -> invalid, >=1e18 -> endless
    int channels = 0;
    int orders = 0;
    int patterns = 0;
    int instruments = 0;
    int samples = 0;
    int subsongs = 0;
    QStringList subsongNames;
    QStringList sampleNames;
    QStringList instrumentNames;
    QString message;         // embedded comment / message
    bool ok = false;

    bool durationValid() const { return duration > 0.0 && duration < 1e18; }
    bool endless() const { return duration >= 1e18; }
};

class OpenMPTLib;

// Owns one openmpt_module* handle. Not thread safe: keep to a single thread
// (the render thread owns the playing module; the analyzer opens its own).
class OpenMPTModule {
public:
    OpenMPTModule() = default;
    ~OpenMPTModule();
    OpenMPTModule(OpenMPTModule &&other) noexcept;
    OpenMPTModule &operator=(OpenMPTModule &&other) noexcept;
    OpenMPTModule(const OpenMPTModule &) = delete;
    OpenMPTModule &operator=(const OpenMPTModule &) = delete;

    void close();
    bool isOpen() const { return handle_ != nullptr; }

    // ---- rendering / transport ----
    size_t readFloatStereo(int sampleRate, size_t frames, float *interleavedOut) const;
    double durationSeconds() const;
    double positionSeconds() const;
    double seekSeconds(double seconds) const;
    double seekOrderRow(int order, int row) const;
    int currentOrder() const;
    int currentPattern() const;
    int currentRow() const;
    int currentSpeed() const;
    int currentTempo() const;
    int playingChannels() const;
    float channelVu(int channel) const;
    bool selectSubsong(int index) const;
    int numChannels() const;
    int numOrders() const;
    int numPatterns() const;
    int numInstruments() const;
    int numSamples() const;
    int numSubsongs() const;
    int orderPattern(int order) const;
    int patternRows(int pattern) const;

    bool setRepeatCount(int count) const;
    bool setCtlText(const char *ctl, const QString &value) const;
    bool setCtlDouble(const char *ctl, double value) const;
    bool setInterpolationLength(int length) const;
    bool setTempoFactor(double factor) const;   // play.tempo_factor
    void setAtEndStop() const;                   // analyzer: no endless follow

    QStringList metadataKeys() const;
    QString metadata(const QString &key) const;
    QString instrumentName(int index) const;
    QString sampleName(int index) const;
    QStringList subsongNameList() const;

    enum CommandType { Note = 0, Instrument = 1, VolColEffect = 2, Effect = 3, Volume = 4, Parameter = 5 };
    QString formatCommand(int pattern, int row, int channel, CommandType type) const;
    // Per-character highlight hints from libopenmpt: ' ' empty, '.' filler,
    // 'n' note, 'i' instrument, 'e' effect, 'v' volume (may be empty).
    QString highlightRow(int pattern, int row, int channel, CommandType type) const;

    ModuleInfo info(const QString &path = QString()) const;

private:
    OpenMPTModule(const OpenMPTLib *lib, void *handle) : lib_(lib), handle_(handle) {}
    const OpenMPTLib *lib_ = nullptr;
    void *handle_ = nullptr;
    friend class OpenMPTLib;
};

// Process-wide loader for the shared library + resolved C API pointers.
class OpenMPTLib {
public:
    struct Api;

    // Load once; nullptr + error message when libopenmpt cannot be found.
    static OpenMPTLib *instance(QString *errorOut = nullptr);

    const Api &api() const { return *api_; }
    QString versionString() const { return versionString_; }
    QString libraryPath() const { return library_.fileName(); }   // the file that was loaded
    bool extensionSupported(const QByteArray &extLower) const;
    QStringList supportedExtensions() const;   // without dots, lower case

    OpenMPTModule openMemory(const QByteArray &data,
                             const QVector<QPair<QByteArray, QByteArray>> &ctls = {},
                             QString *errorOut = nullptr) const;
    OpenMPTModule openFile(const QString &path,
                           const QVector<QPair<QByteArray, QByteArray>> &ctls = {},
                           QString *errorOut = nullptr) const;

    // Analyzer helper (thread safe: opens its own handle): metadata only.
    static bool analyzeFile(const QString &path, ModuleInfo *out, QString *errorOut);

private:
    OpenMPTLib();
    ~OpenMPTLib();
    QLibrary library_;
    Api *api_ = nullptr;
    QString versionString_;
    QStringList extensions_;
    friend class OpenMPTModule;
};
