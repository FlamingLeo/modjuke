#include "engine.h"

#include "audiooutput.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QQueue>
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
                Unload } kind = Unload;
    quint64 transportSerial = 0;    // 0: not a transport request
    quint64 generation = 0;
    quint64 loadEpoch = 0;          // 0: the current epoch when posted
    QString path;                   // Load
    double position = 0.0;          // Load, Seek
    double factor = 1.0;            // Tempo
    int order = 0, row = 0;         // SeekOrderRow
    int subsong = 0;                // Load, Subsong
    int interpLen = 0;              // Load, Interpolation
    bool paused = false;            // Load, Pause
    bool enabled = false;           // AllSubsongs
    bool preserveBufferedTail = false; // Load
};

// Render at most this many frames per lock acquisition (~23 ms at 44.1 kHz)
// so the GUI thread never waits long for the state lock.
constexpr size_t kChunkFrames = 1024;
constexpr int kSeekRampMs = 6;

// Seeking to or past the end would end the song at once; land just before it.
double clampSeekTarget(double seconds, bool bounded, double duration)
{
    const double target = std::max(0.0, seconds);
    return bounded && target >= duration ? std::max(0.0, duration - 0.05) : target;
}

}  // namespace

// ---------------------------------------------------------------------------
// RenderDevice: the live PCM source the audio output pulls from.
// Playback decoder calls stay on the audio thread. File preparation and
// commands run on its event loop; readData() does PCM work, not file IO.
// ---------------------------------------------------------------------------
class RenderDevice : public AudioSource {
public:
    RenderDevice(Engine *engine, QObject *parent)
        : AudioSource(parent), engine_(engine)
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

    void setFormat(int sampleRate, bool int16) override
    {
        int16Out_ = int16;
        sampleRate_ = sampleRate;
    }

    void post(Command command)
    {
        if (!command.loadEpoch) command.loadEpoch = engine_->loadEpoch_.loadAcquire();
        {
            QMutexLocker lock(&cmdMutex_);
            commands_.enqueue(std::move(command));
            if (wakePosted_) return;
            wakePosted_ = true;
        }
        // Do not wait for free space / the next backend pull to process Load.
        // Commands run on the owning audio event loop, OUTSIDE readData().
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }

protected:
    qint64 readData(char *data, qint64 maxlen) override
    {
        const qint64 frameBytes = 2 * qint64(int16Out_ ? sizeof(qint16) : sizeof(float));
        qint64 written = 0;
        while (written < maxlen) {
            const size_t wanted = std::min<qint64>((maxlen - written) / frameBytes, qint64(kChunkFrames));
            if (wanted == 0)
                break;
            float *pcm = reinterpret_cast<float *>(data + written);
            if (int16Out_) {
                conv_.resize(int(wanted * 2));
                pcm = conv_.data();
            }
            renderChunk(pcm, wanted);
            if (int16Out_) {
                auto *dst = reinterpret_cast<qint16 *>(data + written);
                for (size_t i = 0; i < wanted * 2; ++i)
                    dst[i] = qint16(std::lround(std::clamp(pcm[i], -1.0f, 1.0f) * 32767.0f));
            }
            written += qint64(wanted) * frameBytes;
        }
        return written;
    }

    qint64 writeData(const char *, qint64) override { return 0; }

private:
    void drain()
    {
        QQueue<Command> pending;
        { QMutexLocker lock(&cmdMutex_); pending.swap(commands_); wakePosted_ = false; }
        bool newFile = false, discardOldAudio = false;
        for (const auto &c : std::as_const(pending)) {
            // All commands are posted from the GUI thread, which bumps
            // loadEpoch_ before it posts a Load/Unload. Queued transport for
            // the old file therefore carries an older epoch and is dropped;
            // global mixer/policy changes apply, in order, to the new decoder.
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
        // it owns the snapshot lock.
        if (newFile)
            engine_->output_->refresh(discardOldAudio);
        emit readyRead();
    }

    void renderChunk(float *out, size_t frames)
    {
        Engine *e = engine_;
        EngineSnapshot &state = e->state_;
        QMutexLocker lock(&e->stateMutex_);
        syncRepeat();
        if (!module_.isOpen() || state.paused || state.loading || state.ended) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }

        // Fade in after a seek/load removes most clicks at chunk boundaries.
        // The ramp continues across pulls (a backend may pull fewer frames
        // than the ramp is long), so its length is fixed when it starts.
        const float rampStep = rampTotal_ > 0 ? 1.0f / float(rampTotal_) : 1.0f;
        float ramp = rampFrames_ > 0 && rampTotal_ > 0
                         ? std::clamp(1.0f - float(rampFrames_) / float(rampTotal_), 0.0f, 1.0f)
                         : 1.0f;

        size_t got = 0;
        int transitions = 0;
        while (got < frames) {
            got += module_.readFloatStereo(sampleRate_, frames - got, out + got * 2);
            if (got == frames) break;
            if (!state.playAllSubsongs || state.numSubsongs <= 1) { handleEnd(); break; }
            // A transport command (pause/play/seek) is still pending: let it
            // apply first instead of ending the file here; the next pull
            // traverses to the next subsong (or ends) as usual.
            if (renderSerial_ != e->transportSerial_) break;
            if (!advanceSubsong()) { handleEnd(); break; }
            // Bound work for pathological/empty subsongs; continue next pull.
            if (++transitions >= 8) break;
        }
        float peakL = 0.f, peakR = 0.f;
        const float gain = currentGain();
        for (size_t i = 0; i < got * 2; i += 2) {
            float l = out[i] * gain, r = out[i + 1] * gain;
            if (rampFrames_ > 0) {
                l *= ramp;
                r *= ramp;
                ramp = std::min(1.0f, ramp + rampStep);
            }
            out[i] = l;
            out[i + 1] = r;
            peakL = std::max(peakL, std::abs(l));
            peakR = std::max(peakR, std::abs(r));
        }
        if (rampFrames_ > 0)
            rampFrames_ = size_t(std::max(qint64(0), qint64(rampFrames_) - qint64(got)));
        if (got < frames)
            std::memset(out + got * 2, 0, (frames - got) * 2 * sizeof(float));

        // Telemetry for the UI.
        publishPosition();
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
        rampFrames_ = rampTotal_ = size_t(double(sampleRate_) * kSeekRampMs / 1000.0);
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
            if (tempoFactor_ != 1.0)
                next.setTempoFactor(tempoFactor_);
            const int count = std::max(1, next.numSubsongs());
            validSubsong = command.subsong >= 0 && command.subsong < count;
            subsong = validSubsong ? command.subsong : 0;
            if (!next.selectSubsong(subsong)) {
                error = QStringLiteral("Cannot select subsong %1").arg(subsong);
                next.close();
            } else {
                // Read full metadata once, AFTER explicit subsong selection.
                info = next.info(command.path);
                const double initial = validSubsong ? command.position : 0.0;
                if (initial > 0.0 || subsong > 0)
                    next.seekSeconds(clampSeekTarget(initial, info.durationValid() && !info.endless(), info.duration));
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
        state.paused = command.paused; state.ended = false;
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
            break; // applyLoad: file IO must not hold the snapshot lock
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
                    else
                        seekTo(0.0, true);
                }
                state.paused = false;
                state.playing = state.loaded && !state.ended && !state.failed;
                startRamp();
            }
            break;
        case Command::Seek:
            seekTo(command.position, true);
            break;
        case Command::SeekOrderRow:
            if (module_.isOpen()) {
                module_.seekOrderRow(command.order, command.row);
                playheadMoved(true);
            }
            break;
        case Command::Subsong:
            if (module_.isOpen() && command.subsong >= 0 && command.subsong < state.numSubsongs)
                selectSubsong(command.subsong, command.generation, true);
            state.loading = false;
            break;
        case Command::AllSubsongs:
            playAll_ = command.enabled;
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
            tempoFactor_ = command.factor;   // also applied to later loads
            if (module_.isOpen())
                module_.setTempoFactor(command.factor);
            break;
        case Command::Unload:
            module_.close();
            state = EngineSnapshot{};
            syncRepeat();
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

    void seekTo(double seconds, bool ramp)
    {
        const EngineSnapshot &state = engine_->state_;
        if (!module_.isOpen())
            return;
        module_.seekSeconds(clampSeekTarget(seconds, state.durationValid && !state.info.endless(), state.duration));
        playheadMoved(ramp);
    }

    // After a jump: playing on from the new position, optionally fading in.
    void playheadMoved(bool ramp)
    {
        auto &state = engine_->state_;
        publishPosition();
        state.ended = false;
        endReported_ = false;
        if (!state.paused)
            state.playing = true;
        if (ramp)
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

    // ramp = false continues a fade-in already in progress (automatic traversal).
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
        state.songGeneration = generation;
        seekTo(0.0, ramp);
        return true;
    }

    // Play-all-subsongs traversal at the end of a subsong, with no transport
    // command pending (the caller checks both).
    bool advanceSubsong()
    {
        auto &state = engine_->state_;
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
    bool int16Out_ = false;              // devices without float output get stereo 16-bit
    QVector<float> conv_;                // float chunk before the 16-bit conversion
    int nativeRepeat_ = 2;
    bool playAll_ = false;
    size_t rampFrames_ = 0;              // left of the fade-in
    size_t rampTotal_ = 0;
    double tempoFactor_ = 1.0;
    bool endReported_ = false;
    quint64 renderSerial_ = 0;
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
    output_ = new AudioOutput([this](const QString &level, const QString &text) { postLog(level, text); });
    device_ = new RenderDevice(this, output_);
    output_->setSource(device_);
    output_->moveToThread(audioThread_);
    connect(audioThread_, &QThread::finished, output_, &QObject::deleteLater);
    audioThread_->start();

    metadataThread_ = new QThread(this);
    metadataThread_->setObjectName(QStringLiteral("Tracker metadata"));
    metadata_ = new MetadataWorker(&metadataToken_, &loadEpoch_);
    connect(metadata_, &MetadataWorker::songReady, this, [this](const SongDataReply &reply) {
        if (reply.token != metadataToken_.loadRelaxed()) return;
        {
            QMutexLocker lock(&stateMutex_);
            if (!state_.loaded || state_.loading || state_.path != reply.path
                || state_.subsong != reply.subsong || state_.songGeneration != reply.generation)
                return;
        }
        emit songDataReady(reply);
    });
    connect(metadata_, &MetadataWorker::patternReady, this, [this](const PatternDataReply &reply, quint64 epoch) {
        if (reply.token == metadataToken_.loadRelaxed() && epoch == loadEpoch_.loadAcquire())
            emit patternDataReady(reply);
    });
    metadata_->moveToThread(metadataThread_);
    connect(metadataThread_, &QThread::finished, metadata_, &QObject::deleteLater);
    metadataThread_->start();
}

Engine::~Engine()
{
    QMetaObject::invokeMethod(output_, [this] { output_->stop(); }, Qt::BlockingQueuedConnection);
    metadataToken_.fetchAndAddRelaxed(1);
    metadataThread_->quit();
    metadataThread_->wait();
    audioThread_->quit();
    audioThread_->wait();
}

int Engine::interpolationLength(const QString &mode)
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

void Engine::applySettings(const Settings &settings)
{
    settings_ = settings;
    setPlayAllSubsongs(settings.playAllSubsongs);
    setVolume(settings.volume);
    setMuted(settings.muted);
    setLoopTrack(settings.loopTrack);
    // Inspect output ownership only on its thread (manual file changes can
    // replace the sink). Rate/buffer/backend changes still need a rebuild.
    const Settings config = settings;
    QMetaObject::invokeMethod(output_, [this, config] {
        if (output_->isRunning()) { output_->stop(); output_->start(config); }
    }, Qt::BlockingQueuedConnection);
    Command command;
    command.kind = Command::Interpolation;
    command.interpLen = interpolationLength(settings.interpolation);
    device_->post(command);
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
    command.subsong = subsong;
    command.interpLen = interpolationLength(settings_.interpolation);
    {
        QMutexLocker lock(&stateMutex_);
        command.transportSerial = ++transportSerial_;
        state_.seekPending = false;
        command.generation = state_.songGeneration = ++songGeneration_;
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
    QMetaObject::invokeMethod(output_, [this, config] { output_->start(config); }, Qt::QueuedConnection);
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
    {
        QMutexLocker lock(&stateMutex_);
        if (!state_.loaded && !state_.loading)
            return;
        command.position = clampSeekTarget(seconds, state_.durationValid, state_.duration);
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
    command.enabled = enabled;
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
    command.subsong = index;
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
    metadataToken_.storeRelaxed(token);
    QString path;
    int subsong = 0;
    quint64 generation = 0;
    {
        QMutexLocker lock(&stateMutex_);
        path = state_.path;
        subsong = state_.subsong;
        generation = state_.songGeneration;
    }
    const quint64 epoch = loadEpoch_.loadAcquire();
    QMetaObject::invokeMethod(metadata_, [w = metadata_, token, path, subsong, generation, epoch] {
        w->requestSong(token, path, subsong, generation, epoch);
    }, Qt::QueuedConnection);
}

void Engine::requestPattern(int token, int pattern, int channels)
{
    QString path;
    int subsong = 0;
    {
        QMutexLocker lock(&stateMutex_);
        path = state_.path;
        subsong = state_.subsong;
    }
    const quint64 epoch = loadEpoch_.loadAcquire();
    QMetaObject::invokeMethod(metadata_, [w = metadata_, token, path, subsong, epoch, pattern, channels] {
        w->requestPattern(token, path, subsong, epoch, pattern, channels);
    }, Qt::QueuedConnection);
}

EngineSnapshot Engine::snapshot() const
{
    QMutexLocker lock(&stateMutex_);
    return state_;
}

QString Engine::outputDescription() const
{
    return QStringLiteral("%1, buf %2 ms").arg(output_->description(), QString::number(settings_.bufferMs));
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
    }
    emit finished();
}
