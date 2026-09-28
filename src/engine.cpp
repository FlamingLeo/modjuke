#include "engine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QFileInfo>
#include <QMetaObject>
#include <QQueue>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Commands posted from the GUI thread to the audio thread.
// ---------------------------------------------------------------------------
namespace {

struct Command {
    enum Kind { Load, Pause, Seek, SeekOrderRow, Subsong, AllSubsongs, Interpolation, Tempo,
                Unload, RequestSong, RequestPattern, Quit } kind = Unload;
    quint64 transportSerial = 0;
    quint64 generation = 0;
    quint64 loadEpoch = 0;
    QString path;
    double position = 0.0;
    double factor = 1.0;
    int index = 0;
    int order = 0, row = 0;
    bool paused = false;
    bool preserveBufferedTail = false;
    int subsong = 0;
    int token = 0;
    int interpLen = 0;
};

// Render at most this many frames per lock acquisition (~23 ms at 44.1 kHz)
// so the GUI thread never waits long for the state lock.
constexpr size_t kChunkFrames = 1024;
constexpr int kSeekRampMs = 6;

int interpolationLength(const QString &mode)
{
    const QString text = mode.trimmed().toLower();
    if (text == QLatin1String("off") || text == QLatin1String("1"))
        return 1;
    if (text == QLatin1String("linear") || text == QLatin1String("2"))
        return 2;
    if (text == QLatin1String("cubic") || text == QLatin1String("4"))
        return 4;
    if (text == QLatin1String("sinc") || text == QLatin1String("8"))
        return 8;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// RenderDevice: a sequential-access QIODevice the QAudioSink pulls from.
// Playback decoder calls stay on the audio thread. File preparation and
// commands run on its event loop; readData() does PCM work, not file IO.
// ---------------------------------------------------------------------------
class RenderDevice : public QIODevice {
public:
    RenderDevice(Engine *engine, QObject *parent)
        : QIODevice(parent), engine_(engine)
    {
        // QIODevice's default read-ahead can retain PCM from before a seek
        // and render ahead of the actual sink request (even in the null backend).
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }
    // This is a live generator, including when paused/stopped (silence).
    // QIODevice's default atEnd()/bytesAvailable() describe an empty finite
    // stream and can make a pull backend drain or stop requesting data.
    bool atEnd() const override { return !isOpen(); }
    qint64 bytesAvailable() const override
    {
        return isOpen() ? qint64(kChunkFrames * 2 * sizeof(float)) + QIODevice::bytesAvailable() : 0;
    }
    // QAudioSink may reset its source while dropping queued output. This is
    // an unbuffered live stream, not a file: transport seeks use commands.
    bool reset() override { return true; }

    void post(Command command)
    {
        if (!command.loadEpoch) command.loadEpoch = engine_->loadEpoch_.loadAcquire();
        {
            QMutexLocker lock(&cmdMutex_);
            if (command.kind == Command::Load || command.kind == Command::Unload) {
                // A new file supersedes queued transport for the old one. Keep
                // global mixer/policy changes, in order, for the new decoder.
                commands_.erase(std::remove_if(commands_.begin(), commands_.end(), [](const Command &c) {
                    return c.kind == Command::Load || c.kind == Command::Unload || c.kind == Command::Pause
                        || c.kind == Command::Seek || c.kind == Command::SeekOrderRow || c.kind == Command::Subsong;
                }), commands_.end());
            }
            commands_.enqueue(std::move(command));
            if (wakePosted_) return;
            wakePosted_ = true;
        }
        // Do not wait for free space / the next backend pull to process Load.
        // Commands run on the owning audio event loop, OUTSIDE readData().
        QMetaObject::invokeMethod(this, [this] {
            QQueue<Command> pending;
            { QMutexLocker lock(&cmdMutex_); pending.swap(commands_); wakePosted_ = false; }
            bool newFile = false, discardOldAudio = false;
            for (const auto &c : std::as_const(pending)) {
                const bool global = c.kind == Command::AllSubsongs || c.kind == Command::Interpolation || c.kind == Command::Tempo;
                if (!global && c.loadEpoch != engine_->loadEpoch_.loadAcquire()) continue;
                if (c.kind == Command::Load) {
                    if (applyLoad(c)) { newFile = true; discardOldAudio = !c.preserveBufferedTail; }
                    continue;
                }
                QMutexLocker lock(&engine_->stateMutex_);
                applyCommand(c);
            }
            // Discard the previous file's device queue on explicit file switches
            // only. Never reset on seeks, metadata requests or subsong traversal.
            // Outside the read stack, reset/start cannot re-enter the decoder while
            // it owns the snapshot lock. Open the replacement BEFORE closing
            // the old stream: a close/open gap can send PulseAudio devices into
            // idle/suspend and cause multi-second wake delays. Only one source
            // can pull at a time on this event loop; old buffered PCM is discarded
            // as soon as the replacement is ready. Seeks never enter this path.
            if (newFile && discardOldAudio && engine_->sink_) {
                auto *old = engine_->sink_;
                auto *next = new QAudioSink(engine_->outputDevice_, old->format(), engine_->audioContext_);
                next->setBufferSize(engine_->outputBufferBytes_);
                next->start(this);
                if (next->error() == QAudio::NoError) {
                    engine_->sink_ = next;
                    old->reset();
                    delete old;
                } else {
                    delete next; // retain the working old stream rather than lose playback
                    engine_->postLog(QStringLiteral("warn"), QStringLiteral("Could not refresh the audio queue; continuing on the existing output stream."));
                }
            } else if (newFile && engine_->pumpTimer_ && engine_->pumpTimer_->isActive()) {
                QMetaObject::invokeMethod(engine_->pumpTimer_, "timeout", Qt::DirectConnection);
                engine_->pumpTimer_->start(10);
            }
            emit readyRead();
        }, Qt::QueuedConnection);
    }

    void setSampleRate(int rate) { sampleRate_ = rate; }
    int sampleRate() const { return sampleRate_; }

protected:
    qint64 readData(char *data, qint64 maxlen) override
    {
        qint64 written = 0;
        auto *out = reinterpret_cast<float *>(data);
        while (written < maxlen) {
            const qint64 space = maxlen - written;
            const size_t wanted = std::min<qint64>(space / (2 * qint64(sizeof(float))), qint64(kChunkFrames));
            if (wanted == 0)
                break;
            renderChunk(out + written / qint64(sizeof(float)), size_t(wanted));
            written += qint64(wanted) * 2 * qint64(sizeof(float));
        }
        return written;
    }

    qint64 writeData(const char *, qint64) override { return 0; }

private:
    void renderChunk(float *out, size_t frames)
    {
        Engine *e = engine_;
        EngineSnapshot &state = e->state_;
        QMutexLocker lock(&e->stateMutex_);
        syncRepeat();
        if (!module_.isOpen() || state.paused || state.loading) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }
        if (state.ended) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }

        // Fade in after a seek/load removes most clicks at chunk boundaries.
        if (rampFrames_ > 0)
            rampStep_ = float(1.0) / float(rampFrames_);
        float ramp = rampFrames_ > 0 ? std::min(1.0f, 1.0f - float(rampFrames_) * rampStep_) : 1.0f;

        size_t got = 0;
        int transitions = 0;
        while (got < frames) {
            const size_t count = module_.readFloatStereo(sampleRate_, frames-got, out+got*2);
            got += count;
            if (got == frames) break;
            if (!advanceSubsong()) { handleEnd(); break; }
            // Bound work for pathological/empty subsongs; continue next pull.
            if (++transitions >= 8) break;
        }
        float *buf = out;
        float peakL = 0.f, peakR = 0.f;
        const float gain = currentGain();
        for (size_t i = 0; i < got * 2; i += 2) {
            float l = buf[i] * gain, r = buf[i + 1] * gain;
            if (rampFrames_ > 0) {
                l *= ramp;
                r *= ramp;
                ramp = std::min(1.0f, ramp + rampStep_);
            }
            buf[i] = l;
            buf[i + 1] = r;
            peakL = std::max(peakL, std::abs(l));
            peakR = std::max(peakR, std::abs(r));
        }
        if (rampFrames_ > 0)
            rampFrames_ = size_t(std::max(qint64(0), qint64(rampFrames_) - qint64(got)));
        if (got < frames) {
            std::memset(out + got * 2, 0, (frames - got) * 2 * sizeof(float));
        }

        // Telemetry for the UI.
        state.position = module_.positionSeconds();
        state.order = module_.currentOrder();
        state.pattern = module_.currentPattern();
        state.row = module_.currentRow();
        state.speed = module_.currentSpeed();
        state.tempo = module_.currentTempo();
        state.playingChannels = module_.playingChannels();
        state.levelL = peakL;
        state.levelR = peakR;
        if (got > 0) {
            state.vu.resize(state.channels);
            for (int c = 0; c < state.channels; ++c)
                state.vu[c] = module_.channelVu(c);
        } else {
            state.vu.fill(0.f, state.channels);
            state.levelL = state.levelR = 0.f;
        }
    }

    float currentGain() const
    {
        const int muted = engine_->muted_.loadRelaxed();
        if (muted)
            return 0.f;
        const double v = qBound(0, engine_->volume_.loadRelaxed(), 100) / 100.0;
        return float(v * v);      // same perceptual taper as the Python engine
    }

    void startRamp()
    {
        const size_t frames = size_t(double(sampleRate_) * kSeekRampMs / 1000.0);
        rampFrames_ = frames;
        rampStep_ = frames ? 1.0f / float(frames) : 1.0f;
    }

    bool applyLoad(const Command &command)
    {
        Engine *e = engine_;
        // File IO, native parsing, metadata and seeking do not own stateMutex_.
        // A GUI snapshot / a newer click must never wait for a large file to open.
        if (command.loadEpoch != e->loadEpoch_.loadAcquire()) return false;
        module_.close(); // release the old sample bank outside the lock, before allocating another
        QString error;
        auto *lib = OpenMPTLib::instance(&error);
        OpenMPTModule next = lib ? lib->openFile(command.path, {}, &error) : OpenMPTModule();
        ModuleInfo info;
        int subsong = 0;
        bool validSubsong = true;
        if (next.isOpen() && command.loadEpoch == e->loadEpoch_.loadAcquire()) {
            next.setCtlText("play.at_end", QStringLiteral("stop"));
            next.setInterpolationLength(command.interpLen > 0 ? command.interpLen : 8);
            const int count = std::max(1, next.numSubsongs());
            validSubsong = command.index >= 0 && command.index < count;
            subsong = validSubsong ? command.index : 0;
            if (!next.selectSubsong(subsong)) {
                error = QStringLiteral("Cannot select subsong %1").arg(subsong);
                next.close();
            } else {
                // Read full metadata once, AFTER explicit subsong selection.
                info = next.info(command.path);
                const double initial = validSubsong ? command.position : 0.0;
                if (initial > 0.0 || subsong > 0) {
                    double target = std::max(0.0, initial);
                    if (info.durationValid() && !info.endless() && target >= info.duration)
                        target = std::max(0.0, info.duration - 0.05);
                    next.seekSeconds(target);
                }
            }
        }
        if (command.loadEpoch != e->loadEpoch_.loadAcquire()) return false;
        QMutexLocker lock(&e->stateMutex_);
        if (command.loadEpoch != e->loadEpoch_.loadAcquire()) return false;
        module_ = std::move(next);
        auto &state = e->state_;
        renderSerial_ = command.transportSerial;
        if (renderSerial_ == e->transportSerial_) state.seekPending = false;
        state.loading = false; state.loaded = module_.isOpen();
        state.path = command.path; state.songGeneration = command.generation;
        state.orders.clear(); state.paused = command.paused; state.ended = false;
        state.failed = !state.loaded; state.loadError.clear();
        state.vu.clear(); state.levelL = state.levelR = 0.f; state.position = 0.0;
        state.playing = state.loaded && !state.paused;
        state.info = std::move(info); state.channels = state.info.channels;
        state.numSubsongs = std::max(1, state.info.subsongs); state.subsong = subsong;
        state.numOrders = state.info.orders; state.duration = state.info.duration;
        state.durationValid = state.info.durationValid() || state.info.endless();
        state.interpolation = command.interpLen > 0 ? command.interpLen : 8;
        nativeRepeat_ = 2; syncRepeat(); endReported_ = false;
        if (!state.loaded) {
            state.loadError = error.isEmpty() ? QStringLiteral("Cannot open module") : error;
            e->postLog(QStringLiteral("error"), QStringLiteral("%1: %2").arg(QFileInfo(command.path).fileName(), state.loadError));
            emit e->loadReady();
            return true; // also remove queued audio from the failed file's predecessor
        }
        publishPosition(); startRamp();
        if (!validSubsong)
            e->postLog(QStringLiteral("warn"), QStringLiteral("Saved subsong is unavailable; starting the first subsong."));
        e->postLog(QStringLiteral("info"), QStringLiteral("Loaded %1 (%2, %3 ch)")
            .arg(QFileInfo(command.path).fileName(), state.info.format.toUpper()).arg(state.info.channels));
        emit e->loadReady();
        return true;
    }

    void applyCommand(const Command &command)
    {
        Engine *e = engine_;
        EngineSnapshot &state = e->state_;
        if (command.transportSerial) {
            renderSerial_ = command.transportSerial;
            if (renderSerial_ == e->transportSerial_)
                state.seekPending = false;
        }
        switch (command.kind) {
        case Command::Load:
            break; // prepared/published by applyLoad, never inside a PCM pull
        case Command::Pause:
            if (command.paused) {
                state.paused = true;
                state.playing = false;
                state.vu.fill(0.f, state.channels);
                state.levelL = state.levelR = 0.f;
            } else {
                if (state.ended) {       // replay a finished song from the top
                    state.ended = false;
                    endReported_ = false;
                    if (state.playAllSubsongs)
                        selectSubsong(0, ++e->songGeneration_, true);
                    else applySeek(0.0);
                }
                state.paused = false;
                state.playing = state.loaded && !state.ended && !state.failed;
                startRamp();
            }
            break;
        case Command::Seek:
            applySeek(command.position);
            break;
        case Command::SeekOrderRow:
            if (module_.isOpen()) {
                module_.seekOrderRow(command.order, command.row);
                publishPosition();
                state.ended = false;
                endReported_ = false;
                if (!state.paused)
                    state.playing = true;
                startRamp();
            }
            break;
        case Command::Subsong:
            if (module_.isOpen() && command.index >= 0 && command.index < state.numSubsongs)
                selectSubsong(command.index, command.generation, true);
            state.loading = false;
            break;
        case Command::AllSubsongs:
            playAll_ = command.index != 0;
            syncRepeat();
            if (state.playAllSubsongs && module_.isOpen())
                selectSubsong(0, command.generation, true);
            else
                state.songGeneration = command.generation;
            break;
        case Command::Interpolation:
            if (module_.isOpen() && command.interpLen > 0) {
                if (module_.setInterpolationLength(command.interpLen))
                    state.interpolation = command.interpLen;
            }
            break;
        case Command::Tempo:
            if (module_.isOpen())
                module_.setTempoFactor(command.factor);
            break;
        case Command::Unload:
            module_.close();
            state = EngineSnapshot{};
            syncRepeat();
            break;
        case Command::RequestSong:
        case Command::RequestPattern:
            break; // handled on the separate metadata worker
        case Command::Quit:
            module_.close();
            break;
        }
    }

    void publishPosition()
    {
        auto &state = engine_->state_;
        state.position = module_.positionSeconds();
        state.order = module_.currentOrder();
        state.pattern = module_.currentPattern();
        state.row = module_.currentRow();
        state.speed = module_.currentSpeed();
        state.tempo = module_.currentTempo();
    }

    void applySeek(double seconds)
    {
        EngineSnapshot &state = engine_->state_;
        if (!module_.isOpen())
            return;
        double target = std::max(0.0, seconds);
        if (state.durationValid && !state.info.endless() && target >= state.duration)
            target = std::max(0.0, state.duration - 0.05);
        module_.seekSeconds(target);
        publishPosition();
        state.ended = false;
        endReported_ = false;
        if (!state.paused)
            state.playing = true;
        startRamp();
    }

    void syncRepeat()
    {
        auto &state = engine_->state_;
        state.loop = engine_->loopTrack_.loadRelaxed() != 0;
        state.playAllSubsongs = playAll_;
        const int count = state.loop && (!state.playAllSubsongs || state.numSubsongs <= 1) ? -1 : 0;
        if (module_.isOpen() && nativeRepeat_ != count) {
            module_.setRepeatCount(count);
            nativeRepeat_ = count;
        }
    }

    bool selectSubsong(int index, quint64 generation, bool ramp)
    {
        auto &state = engine_->state_;
        if (!module_.selectSubsong(index)) {
            engine_->postLog(QStringLiteral("error"), QStringLiteral("Cannot select subsong %1").arg(index+1));
            return false;
        }
        state.subsong = index;
        state.numOrders = state.info.orders = module_.numOrders();
        state.duration = state.info.duration = module_.durationSeconds();
        state.durationValid = state.info.durationValid() || state.info.endless();
        state.orders.clear();
        state.songGeneration = generation;
        const auto previousRamp = rampFrames_;
        applySeek(0.0);
        if (!ramp) rampFrames_ = previousRamp;
        return true;
    }

    bool advanceSubsong()
    {
        auto &state = engine_->state_;
        // A user transport request wins over automatic EOF traversal. Do not
        // wait for the GUI to keep playing the remainder of this file.
        if (!state.playAllSubsongs || state.numSubsongs <= 1
            || renderSerial_ != engine_->transportSerial_) return false;
        int next = state.subsong + 1;
        if (next >= state.numSubsongs) {
            if (!state.loop) return false;
            next = 0;
        }
        if (!selectSubsong(next, ++engine_->songGeneration_, false)) return false;
        renderSerial_ = ++engine_->transportSerial_;
        return true;
    }

    void handleEnd()
    {
        EngineSnapshot &state = engine_->state_;
        state.ended = true;
        state.playing = false;
        state.position = state.durationValid ? state.duration : state.position;
        if (!endReported_) {
            endReported_ = true;
            Engine *e = engine_;
            const quint64 serial = renderSerial_;
            QMetaObject::invokeMethod(e, [e, serial] { e->handleFinished(serial); }, Qt::QueuedConnection);
        }
    }

    Engine *engine_ = nullptr;
    OpenMPTModule module_;
    QMutex cmdMutex_;
    QQueue<Command> commands_;
    bool wakePosted_ = false;
    int sampleRate_ = 44100;
    int nativeRepeat_ = 2;
    bool playAll_ = false;
    size_t rampFrames_ = 0;
    float rampStep_ = 1.0f;
    bool endReported_ = false;
    quint64 renderSerial_ = 0;
};

// Tracker formatting can involve thousands of native calls. Never run it
// on the output thread or under the playback snapshot lock.
class MetadataWorker : public QObject {
public:
    explicit MetadataWorker(Engine *owner) : owner_(owner) {}
    void request(const Command &command)
    {
        Engine *e = owner_;
        if (command.token != e->metadataToken_.loadRelaxed()) return;
        if (path_ != command.path || loadEpoch_ != command.loadEpoch || !module_.isOpen()) {
            path_ = command.path;
            loadEpoch_ = command.loadEpoch;
            module_.close();
            if (auto *lib = OpenMPTLib::instance())
                module_ = lib->openFile(path_, {{"load.skip_samples", "1"}}, nullptr);
        }
        subsong_ = command.subsong;
        generation_ = command.generation;
        if (module_.isOpen() && !module_.selectSubsong(subsong_)) module_.close();
        if (command.token != e->metadataToken_.loadRelaxed() || command.loadEpoch != e->loadEpoch_.loadAcquire()) return;
        switch (command.kind) {
        case Command::RequestSong: {
            SongDataReply reply;
            reply.token = command.token;
            reply.path = path_;
            reply.generation = command.generation;
            reply.subsong = command.subsong;
            if (module_.isOpen()) {
                const int orders = module_.numOrders();
                for (int i = 0; i < orders; ++i) {
                    const int pattern = module_.orderPattern(i);
                    reply.orders << pattern;
                    if (pattern >= 0 && !reply.rows.contains(pattern))
                        reply.rows.insert(pattern, module_.patternRows(pattern));
                }
                reply.channels = module_.numChannels();
                reply.numPatterns = module_.numPatterns();
            }
            SongDataReply captured = reply;
            const int subsong = command.subsong;
            QMetaObject::invokeMethod(e, [e, captured, subsong] {
                if (captured.token != e->metadataToken_.loadRelaxed()) return;
                {
                    QMutexLocker lock(&e->stateMutex_);
                    if (!e->state_.loaded || e->state_.loading || e->state_.path != captured.path
                        || e->state_.subsong != subsong || e->state_.songGeneration != captured.generation)
                        return;
                    e->state_.orders = captured.orders;
                }
                emit e->songDataReady(captured);
            }, Qt::QueuedConnection);
            break;
        }
        case Command::RequestPattern: {
            PatternDataReply reply;
            reply.token = command.token;
            reply.pattern = command.index;
            if (!module_.isOpen()) {
                reply.error = QStringLiteral("no module");
            } else {
                reply.rows = module_.patternRows(command.index);
                reply.channels = command.order > 0 ? command.order : module_.numChannels();
                reply.cells.resize(reply.rows);
                for (int row = 0; row < reply.rows; ++row) {
                    if (command.token != e->metadataToken_.loadRelaxed()) return;
                    auto &cells = reply.cells[row];
                    cells.resize(reply.channels);
                    for (int ch = 0; ch < reply.channels; ++ch) {
                        using CT = OpenMPTModule::CommandType;
                        auto take = [&](CT t, const char *fallback) {
                            QString value = module_.formatCommand(command.index, row, ch, t).trimmed();
                            if (value.isEmpty() || value.startsWith(QLatin1Char('.')))
                                return QString::fromLatin1(fallback);
                            return value;
                        };
                        PatternDataReply::Cell cell;
                        cell.note = take(CT::Note, "...");
                        cell.instrument = take(CT::Instrument, "");
                        QString volLetter = take(CT::VolColEffect, "");
                        QString volValue = take(CT::Volume, "");
                        if (volValue.isEmpty() && volLetter.isEmpty())
                            cell.volume.clear();
                        else
                            cell.volume = (volLetter.isEmpty() ? QStringLiteral("v") : volLetter)
                                          + (volValue.isEmpty() ? QStringLiteral("..") : volValue);
                        QString effLetter = take(CT::Effect, "");
                        QString effParam = take(CT::Parameter, "");
                        if (effLetter.isEmpty())
                            cell.effect.clear();
                        else
                            cell.effect = effLetter + (effParam.isEmpty() ? QStringLiteral("00") : effParam);
                        cells[ch] = cell;
                    }
                }
            }
            PatternDataReply captured = reply;
            QMetaObject::invokeMethod(e, [e, captured, epoch = command.loadEpoch] {
                if (captured.token == e->metadataToken_.loadRelaxed() && epoch == e->loadEpoch_.loadAcquire())
                    emit e->patternDataReady(captured);
            },
                                      Qt::QueuedConnection);
            break;
        }
        default: break;
        }
    }
private:
    Engine *owner_;
    QString path_;
    int subsong_ = 0;
    quint64 generation_ = 0;
    quint64 loadEpoch_ = 0;
    OpenMPTModule module_;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<SongDataReply>("SongDataReply");
    qRegisterMetaType<PatternDataReply>("PatternDataReply");
    audioThread_ = new QThread(this);
    audioThread_->setObjectName(QStringLiteral("Audio output"));
    audioContext_ = new QObject;
    device_ = new RenderDevice(this, audioContext_);
    audioContext_->moveToThread(audioThread_);
    connect(audioThread_, &QThread::finished, audioContext_, &QObject::deleteLater);
    audioThread_->start();
    metadataThread_ = new QThread(this);
    metadataThread_->setObjectName(QStringLiteral("Tracker metadata"));
    metadataWorker_ = new MetadataWorker(this);
    metadataWorker_->moveToThread(metadataThread_);
    connect(metadataThread_, &QThread::finished, metadataWorker_, &QObject::deleteLater);
    metadataThread_->start();
}

Engine::~Engine()
{
    stopOutput();
    metadataToken_.fetchAndAddRelaxed(1);
    metadataThread_->quit();
    metadataThread_->wait();
    audioThread_->quit();
    audioThread_->wait();
}

int Engine::interpolationLength(const QString &mode) { return ::interpolationLength(mode); }

QString Engine::interpolationName(int length)
{
    switch (length) {
    case 1: return QStringLiteral("off");
    case 2: return QStringLiteral("linear");
    case 4: return QStringLiteral("cubic");
    case 8: return QStringLiteral("sinc");
    default: break;
    }
    return QString();
}

QString Engine::interpolationLabel(const QString &mode)
{
    if (mode == QLatin1String("off"))
        return tr("no interpolation");
    if (mode.isEmpty())
        return QString();
    return mode;
}

void Engine::applySettings(const Settings &settings)
{
    settings_ = settings;
    setPlayAllSubsongs(settings.playAllSubsongs);
    volume_.storeRelaxed(settings.volume);
    muted_.storeRelaxed(settings.muted ? 1 : 0);
    loopTrack_.storeRelaxed(settings.loopTrack ? 1 : 0);
    // Inspect output ownership only on its thread (manual file changes can
    // replace the sink). Rate/buffer/backend changes still need a rebuild.
    const Settings config = settings;
    QMetaObject::invokeMethod(audioContext_, [this, config] {
        if (sink_ || silent_) { stopOutputOnAudioThread(); startOutputOnAudioThread(config); }
    }, Qt::BlockingQueuedConnection);
    if (device_)
        device_->post(Command{.kind = Command::Interpolation, .interpLen = interpolationLength(settings.interpolation)});
}

bool Engine::startOutput()
{
    bool ok = false;
    const Settings config = settings_;
    QMetaObject::invokeMethod(audioContext_, [this, config, &ok] {
        ok = startOutputOnAudioThread(config);
    }, Qt::BlockingQueuedConnection);
    return ok;
}

bool Engine::startOutputOnAudioThread(const Settings &config)
{
    if (sink_ || silent_)
        return sink_ != nullptr || silent_;

    const bool wantNull = config.backend == QLatin1String("null");
    QAudioDevice audioDevice;
    if (!wantNull)
        audioDevice = QMediaDevices::defaultAudioOutput();
    if (audioDevice.isNull()) {
        {
            QMutexLocker lock(&stateMutex_);
            silent_ = true;
            outputName_ = QStringLiteral("null");
            samplerate_ = config.samplerate > 0 ? config.samplerate : 44100;
        }
        device_->setSampleRate(samplerate_);
        // Null output uses the same dedicated thread, independent of the GUI.
        if (!pumpTimer_) {
            pumpTimer_ = new QTimer(audioContext_);
            pumpTimer_->setTimerType(Qt::PreciseTimer);
            connect(pumpTimer_, &QTimer::timeout, audioContext_, [this] {
                const int frames = std::max(16, samplerate_ / 100);       // ~10 ms
                scratch_.resize(frames * 2);
                device_->read(reinterpret_cast<char *>(scratch_.data()), frames * 2 * int(sizeof(float)));
            });
        }
        pumpTimer_->start(10);
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("Silent output (%1 Hz) — no audio device in use").arg(samplerate_));
        return true;
    }

    QAudioFormat format;
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    const int preferred = audioDevice.preferredFormat().sampleRate();
    int rate = config.samplerate > 0 ? config.samplerate : (preferred > 0 ? preferred : 44100);
    format.setSampleRate(rate);
    QAudioFormat chosen = format;
    if (audioDevice.isFormatSupported(chosen)) {
        // fine
    } else {
        chosen = audioDevice.preferredFormat();
        chosen.setSampleFormat(QAudioFormat::Float);
        if (!audioDevice.isFormatSupported(chosen))
            chosen = audioDevice.preferredFormat();
        rate = chosen.sampleRate();
    }
    if (config.samplerate > 0) {
        QAudioFormat requested = chosen;
        requested.setSampleRate(config.samplerate);
        if (audioDevice.isFormatSupported(requested)) {
            chosen = requested;
            rate = config.samplerate;
        } else {
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("Output rate %1 Hz unavailable, using %2 Hz")
                                .arg(config.samplerate).arg(rate));
        }
    }

    outputDevice_ = audioDevice;
    sink_ = new QAudioSink(audioDevice, chosen, audioContext_);
    const int bytesPerSecond = rate * 2 * int(sizeof(float));
    outputBufferBytes_ = std::max<qint64>(bytesPerSecond * config.bufferMs / 1000,
                                        qint64(rate * 2 * sizeof(float) * 25 / 1000));
    sink_->setBufferSize(outputBufferBytes_);
    {
        QMutexLocker lock(&stateMutex_);
        samplerate_ = rate;
        outputName_ = QStringLiteral("qtaudio");
        silent_ = false;
    }
    device_->setSampleRate(rate);
    sink_->start(device_);
    if (pumpTimer_)
        pumpTimer_->stop();
    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("Audio: %1 @ %2 Hz").arg(audioDevice.description(), QString::number(rate)));
    return true;
}

void Engine::stopOutput()
{
    QMetaObject::invokeMethod(audioContext_, [this] { stopOutputOnAudioThread(); },
                              Qt::BlockingQueuedConnection);
}

void Engine::stopOutputOnAudioThread()
{
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    if (pumpTimer_) {
        pumpTimer_->stop();
    }
    QMutexLocker lock(&stateMutex_);
    silent_ = false;
}

void Engine::playPath(const QString &path, double position, bool paused, int subsong, bool preserveBufferedTail)
{
    Command command;
    command.kind = Command::Load;
    command.preserveBufferedTail = preserveBufferedTail;
    command.loadEpoch = loadEpoch_.fetchAndAddOrdered(1) + 1;
    metadataToken_.fetchAndAddRelaxed(1);
    command.path = path;
    command.position = position;
    command.paused = paused;
    command.index = subsong;
    command.interpLen = interpolationLength(settings_.interpolation);
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
        state_.seekPending = false;
        command.generation = state_.songGeneration = ++songGeneration_;
        state_.orders.clear();
        state_.info = {}; state_.duration = 0.0; state_.durationValid = false;
        state_.channels = 0; state_.numSubsongs = 1; state_.vu.clear();
        state_.numOrders = 0;
        state_.subsong = subsong;
        state_.path = path;
        state_.loaded = false;
        state_.playing = !paused;
        state_.paused = paused;
        state_.ended = false;
        state_.failed = false;
        state_.position = position;
        state_.loading = true;
    }
    // Publish optimistic GUI state BEFORE making the command visible to the
    // renderer; otherwise a fast renderer can load it, then be marked loading
    // again by this thread and remain silent forever.
    // Opening an output device can itself block; do not put a blocking call
    // behind the asynchronous file load on the GUI thread's first playback.
    const Settings config = settings_;
    QMetaObject::invokeMethod(audioContext_, [this, config] {
        if (!sink_ && !silent_) startOutputOnAudioThread(config);
    }, Qt::QueuedConnection);
    device_->post(command);
}

void Engine::play()
{
    Command command;
    command.kind = Command::Pause;
    command.paused = false;
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
    }
    device_->post(command);
}

void Engine::pause()
{
    Command command;
    command.kind = Command::Pause;
    command.paused = true;
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
        state_.paused = true;
        state_.playing = false;
    }
    device_->post(command);
}

void Engine::togglePause()
{
    QMutexLocker lock(&stateMutex_);
    const bool running = state_.playing;
    lock.unlock();
    if (running)
        pause();
    else
        play();
}

void Engine::stop()
{
    Command command;
    command.kind = Command::Unload;
    command.loadEpoch = loadEpoch_.fetchAndAddOrdered(1) + 1;
    metadataToken_.fetchAndAddRelaxed(1);
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
        state_ = EngineSnapshot{};
        state_.playAllSubsongs = allSubsongsRequested_.loadRelaxed() != 0;
    }
    device_->post(command);
}

void Engine::seek(double seconds)
{
    Command command;
    command.kind = Command::Seek;
    command.position = std::max(0.0, seconds);
    {
        QMutexLocker lock(&stateMutex_);
        if (!state_.loaded && !state_.loading)
            return;
        if (state_.durationValid && command.position >= state_.duration)
            command.position = std::max(0.0, state_.duration - 0.05);
        command.transportSerial = ++transportSerial_;
        state_.seekTarget = command.position;
        state_.seekPending = true;
    }
    // Keep the output stream alive. Reopening it for each seek can leave pull
    // backends idle, introduces repeated prebuffering, and races backend drain
    // callbacks. Apply commands on the audio event loop between PCM pulls instead.
    device_->post(command);
}

void Engine::seekRelative(double delta)
{
    double position = 0.0;
    {
        QMutexLocker lock(&stateMutex_);
        position = state_.position;
        if (state_.loop && state_.durationValid && std::isfinite(state_.duration) && state_.duration > 0.0)
            position = std::fmod(position, state_.duration);
    }
    seek(position + delta);
}

void Engine::seekFraction(double fraction)
{
    double duration = 0.0, position = 0.0;
    bool valid = false;
    {
        QMutexLocker lock(&stateMutex_);
        duration = state_.duration;
        valid = state_.durationValid;
        position = state_.position;
    }
    if (valid && duration > 0.0 && duration < 1e18)
        seek(std::clamp(fraction, 0.0, 1.0) * duration);
    else
        seek(std::max(0.0, fraction) * std::max(position + 300.0, 300.0));
}

void Engine::seekOrderRow(int order, int row)
{
    Command command;
    command.kind = Command::SeekOrderRow;
    command.order = order;
    command.row = row;
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
    }
    device_->post(command);
}

void Engine::setPlayAllSubsongs(bool enabled)
{
    if (allSubsongsRequested_.fetchAndStoreRelaxed(enabled ? 1 : 0) == int(enabled)) return;
    Command command;
    command.kind = Command::AllSubsongs;
    command.index = enabled ? 1 : 0;
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
        command.generation = ++songGeneration_;
    }
    device_->post(command);
}

void Engine::setSubsong(int index)
{
    Command command;
    command.kind = Command::Subsong;
    command.index = index;
    {
        QMutexLocker lock(&stateMutex_);
        // During queued loads/changes validate on the worker, not against the
        // previous module/index. In steady state invalid/no-op requests must
        // not cancel a legitimate end notification or restart the current song.
        if (!state_.loading && state_.songGeneration == songGeneration_
            && (!state_.loaded || index < 0 || index >= state_.numSubsongs
                                || index == state_.subsong)) return;
        command.transportSerial = ++transportSerial_;
        command.generation = ++songGeneration_;
        state_.loading = state_.loaded || state_.loading;
    }
    device_->post(command);
}

void Engine::setTempoFactor(double factor)
{
    Command command;
    command.kind = Command::Tempo;
    command.factor = factor;
    device_->post(command);
}

void Engine::setInterpolation(const QString &mode)
{
    const int length = interpolationLength(mode);
    if (length < 0)
        return;
    settings_.interpolation = mode;
    Command command;
    command.kind = Command::Interpolation;
    command.interpLen = length;
    device_->post(command);
}

void Engine::setVolume(int percent)
{
    volume_.storeRelaxed(std::clamp(percent, 0, 100));
}

void Engine::setMuted(bool muted)
{
    muted_.storeRelaxed(muted ? 1 : 0);
}

void Engine::setLoopTrack(bool loop)
{
    loopTrack_.storeRelaxed(loop ? 1 : 0);
}

void Engine::requestSongData(int token)
{
    Command command;
    command.kind = Command::RequestSong;
    command.token = token;
    metadataToken_.storeRelaxed(token);
    const auto snap = snapshot();
    command.path = snap.path;
    command.subsong = snap.subsong;
    command.generation = snap.songGeneration;
    command.loadEpoch = loadEpoch_.loadAcquire();
    QMetaObject::invokeMethod(metadataWorker_, [this, command] {
        static_cast<MetadataWorker *>(metadataWorker_)->request(command);
    }, Qt::QueuedConnection);
}

void Engine::requestPattern(int token, int pattern, int channels)
{
    Command command;
    command.kind = Command::RequestPattern;
    command.token = token;
    command.index = pattern;
    command.order = channels;   // reused as "channel limit" (0 = all)
    const auto snap = snapshot();
    command.path = snap.path;
    command.subsong = snap.subsong;
    command.generation = snap.songGeneration;
    command.loadEpoch = loadEpoch_.loadAcquire();
    QMetaObject::invokeMethod(metadataWorker_, [this, command] {
        static_cast<MetadataWorker *>(metadataWorker_)->request(command);
    }, Qt::QueuedConnection);
}

EngineSnapshot Engine::snapshot() const
{
    QMutexLocker lock(&stateMutex_);
    EngineSnapshot copy = state_;
    return copy;
}

int Engine::samplerate() const
{
    QMutexLocker lock(&stateMutex_);
    return samplerate_;
}

bool Engine::isSilentBackend() const
{
    QMutexLocker lock(&stateMutex_);
    return silent_;
}

QString Engine::outputDescription() const
{
    QMutexLocker lock(&stateMutex_);
    QString name = silent_ ? QStringLiteral("null") : outputName_;
    QString rate = QString::number(samplerate_ / 1000.0, 'g', 4) + QStringLiteral("kHz");
    return QStringLiteral("%1 %2, buf %3 ms").arg(name, rate).arg(settings_.bufferMs);
}

void Engine::postLog(const QString &level, const QString &text)
{
    emit logMessage(level, text);
}

void Engine::handleFinished(quint64 transportSerial)
{
    {
        QMutexLocker lock(&stateMutex_);
        // A queued end notification belongs to the transport which produced
        // it, never to a later seek/restart/load (even of the same file).
        if (transportSerial != transportSerial_ || !state_.ended)
            return;
        state_.playing = false;
        state_.ended = true;
    }
    emit finished();
}
