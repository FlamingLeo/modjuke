// Playback engine: QAudioSink and rendering run on a dedicated audio thread,
// with a separate metadata decoder/worker and a snapshot for the UI.
// High-level equivalent of modjuke/engine.py (worker thread + ring buffer),
// simplified to a pull device (no separate watchdog / restart budget).
#pragma once

#include "config.h"
#include "library.h"
#include "openmptapi.h"

#include <QAtomicInt>
#include <QAudioDevice>
#include <QHash>
#include <QMutex>
#include <QVector>
#include <memory>

class QAudioSink;
class QTimer;
class QThread;
class Engine;

struct EngineSnapshot {
    QString path;
    QString loadError;
    bool loading = false;
    bool loaded = false;
    bool playing = false;
    bool paused = false;
    bool ended = false;
    bool failed = false;

    double position = 0.0;
    bool seekPending = false;
    double seekTarget = 0.0;
    double duration = 0.0;
    bool durationValid = false;

    int order = 0, pattern = 0, row = 0;
    int numOrders = 0; // authoritative decoder count, independent of metadata replies
    quint64 songGeneration = 0;
    int speed = 0, tempo = 0;
    int channels = 0, playingChannels = 0;
    int subsong = 0, numSubsongs = 1;
    bool loop = false;
    bool playAllSubsongs = false; // individual native selections, current-song time

    QVector<float> vu;              // per channel, 0..1
    float levelL = 0.f, levelR = 0.f;
    int interpolation = 8;          // filter length the mixer uses

    ModuleInfo info;                // metadata of the loaded module
    QStringList orderPatterns;      // pattern index per order entry (as strings for signals)
    QVector<int> orders;            // pattern per order (-1 = skip)
    QHash<int, int> patternRows;
};

// Reply payloads delivered as queued signals from the metadata worker.
struct SongDataReply {
    int token = 0;
    quint64 generation = 0;
    int subsong = 0;
    QString path;
    QVector<int> orders;                  // pattern index per order (-1 = skip)
    QHash<int, int> rows;                 // pattern -> row count
    int channels = 0;
    int numPatterns = 0;
};

struct PatternDataReply {
    int token = 0;
    int pattern = -1;
    int rows = 0;
    int channels = 0;
    struct Cell { QString note, instrument, volume, effect; };
    QVector<QVector<Cell>> cells;
    QString error;
};

class RenderDevice;

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    // Audio setup from Settings (backend, samplerate, buffer, interpolation).
    void applySettings(const Settings &settings);

    bool startOutput();
    void stopOutput();

    // ---- transport (safe from the GUI thread) ----
    void playPath(const QString &path, double position = 0.0, bool paused = false, int subsong = 0,
                  bool preserveBufferedTail = false); // true ONLY for natural EOF queue handoff
    void play();                     // resume or restart a finished song
    void pause();
    void togglePause();
    void stop();                     // stop + unload
    void seek(double seconds);
    void seekRelative(double delta);
    void seekFraction(double fraction);
    void seekOrderRow(int order, int row);
    void setSubsong(int index);
    void setPlayAllSubsongs(bool enabled); // enabling restarts first, preserves pause
    void setTempoFactor(double factor);      // --speed support
    void setInterpolation(const QString &mode);   // off|linear|cubic|sinc
    void requestSongData(int token);
    void requestPattern(int token, int pattern, int channels);

    // ---- mixer state ----
    void setVolume(int percent);             // 0..100
    int volume() const { return volume_; }
    void setMuted(bool muted);
    bool muted() const { return muted_; }
    void setLoopTrack(bool loop);
    bool loopTrack() const { return loopTrack_; }

    EngineSnapshot snapshot() const;
    QString outputDescription() const;       // "qtaudio 48kHz, buf 220 ms"
    int samplerate() const;

    bool isSilentBackend() const;

public slots:
    // Safe to call from any thread (emits logMessage; receivers queue).
    void postLog(const QString &level, const QString &text);

    static int interpolationLength(const QString &mode);           // 1,2,4,8 or -1
    static QString interpolationName(int length);
    static QString interpolationLabel(const QString &mode);

signals:
    void loadReady();                        // prompt GUI refresh, not per audio chunk
    void finished();                         // module reached its end
    void logMessage(const QString &level, const QString &text);
    void songDataReady(SongDataReply reply);
    void patternDataReady(PatternDataReply reply);

private:
    friend class RenderDevice;
    friend class MetadataWorker;
    friend class TestEngine; // thread-affinity/stream-contract regressions
    bool startOutputOnAudioThread(const Settings &config);
    void stopOutputOnAudioThread();
    QThread *audioThread_ = nullptr;
    QObject *audioContext_ = nullptr;
    QThread *metadataThread_ = nullptr;
    QObject *metadataWorker_ = nullptr;
    QAtomicInt metadataToken_{0};
    QAtomicInteger<quint64> loadEpoch_{0}; // invalidate superseded native opens without taking the snapshot lock
    void handleFinished(quint64 transportSerial);

    mutable QMutex stateMutex_;
    quint64 songGeneration_ = 0;
    quint64 transportSerial_ = 0;           // guarded by stateMutex_
    EngineSnapshot state_;                   // mirror updated by the audio thread

    RenderDevice *device_ = nullptr;
    QAudioSink *sink_ = nullptr;
    QAudioDevice outputDevice_;             // audio-thread owned
    qint64 outputBufferBytes_ = 0;
    QTimer *pumpTimer_ = nullptr;            // silent backend: pump on the audio thread
    QVector<float> scratch_;                 // pump buffer

    Settings settings_;
    bool silent_ = false;
    int samplerate_ = 44100;
    QString outputName_;

    QAtomicInt volume_{80};
    QAtomicInt muted_{0};
    QAtomicInt loopTrack_{0};
    QAtomicInt allSubsongsRequested_{0};
};

Q_DECLARE_METATYPE(SongDataReply)
Q_DECLARE_METATYPE(PatternDataReply)
