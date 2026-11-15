// Playback engine: output and rendering run on a dedicated audio thread,
// with a separate metadata decoder/worker and a snapshot for the UI.
// High-level equivalent of modjuke/engine.py (worker thread + ring buffer),
// simplified to a pull device (no separate watchdog / restart budget).
#pragma once

#include "config.h"
#include "library.h"
#include "openmptapi.h"
#include "trackerdata.h"

#include <QAtomicInt>
#include <QHash>
#include <QMutex>
#include <QThreadPool>
#include <QVector>
#include <memory>

class QThread;
class AudioOutput;
class RenderDevice;

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
    int channels = 0;
    int subsong = 0, numSubsongs = 1;
    bool loop = false;
    bool playAllSubsongs = false; // individual native selections, current-song time

    QVector<float> vu;              // per channel, 0..1
    float levelL = 0.f, levelR = 0.f;
    int interpolation = 8;          // filter length the mixer uses

    ModuleInfo info;                // metadata of the loaded module
};

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    // Audio setup from Settings (backend, samplerate, buffer, interpolation).
    void applySettings(const Settings &settings);

    // ---- transport (GUI thread only) ----
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
    void requestSongData(int token);
    void requestPattern(int token, int pattern, int channels);

    // ---- mixer state ----
    void setVolume(int percent);             // 0..100
    void setMuted(bool muted);
    void setLoopTrack(bool loop);

    EngineSnapshot snapshot() const;
    QString outputDescription() const;       // "qtaudio 48kHz, buf 220 ms"

    static int interpolationLength(const QString &mode);           // 1,2,4,8 or -1
    static QString interpolationName(int length);

public slots:
    // Safe to call from any thread (emits logMessage; receivers queue).
    void postLog(const QString &level, const QString &text);

signals:
    void loadReady();                        // prompt GUI refresh, not per audio chunk
    void finished();                         // module reached its end
    void logMessage(const QString &level, const QString &text);
    void songDataReady(SongDataReply reply);
    void patternDataReady(PatternDataReply reply);

private:
    friend class RenderDevice;
    void handleFinished(quint64 transportSerial);

    // A silent stretch of the playing song, to check whether the rest of it
    // is silent too.
    struct SilenceCheck {
        QString path;
        int subsong = 0;
        double position = 0.0;       // where playback has rendered to
        int sampleRate = 44100;
        int interpLen = 8;
        double tempoFactor = 1.0;
        quint64 loadEpoch = 0;
        quint64 silenceRun = 0;      // the renderer's id of the silent stretch
    };
    // Plays the song again on silencePool_, from its start (a seek does not
    // restore every sample that a later note or volume change can make
    // audible). If nothing from the silent stretch on is audible, asks the
    // renderer to end the song now; if the music comes back after a long
    // silence, to go on from there.
    void checkSilence(const SilenceCheck &check);

    QThread *audioThread_ = nullptr;
    AudioOutput *output_ = nullptr;          // lives on audioThread_, also its invokeMethod context
    RenderDevice *device_ = nullptr;         // child of output_
    QThread *metadataThread_ = nullptr;
    MetadataWorker *metadata_ = nullptr;
    QThreadPool silencePool_;
    QAtomicInt metadataToken_{0};
    QAtomicInteger<quint64> loadEpoch_{0}; // invalidate superseded native opens without taking the snapshot lock

    mutable QMutex stateMutex_;
    quint64 songGeneration_ = 0;
    quint64 transportSerial_ = 0;           // guarded by stateMutex_
    EngineSnapshot state_;                   // mirror updated by the audio thread

    Settings settings_;

    QAtomicInt volume_{80};
    QAtomicInt muted_{0};
    QAtomicInt loopTrack_{0};
    QAtomicInt allSubsongsRequested_{0};
};
