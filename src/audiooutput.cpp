#include "audiooutput.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QMediaDevices>
#include <QTimer>

#include <algorithm>

AudioOutput::AudioOutput(LogFunction log) : log_(std::move(log)) {}

void AudioOutput::setSource(AudioSource *source)
{
    source_ = source;
}

void AudioOutput::publish(const QString &name, int rate)
{
    QMutexLocker lock(&publishedMutex_);
    name_ = name;
    rate_ = rate;
}

QString AudioOutput::description() const
{
    QMutexLocker lock(&publishedMutex_);
    return name_ + QLatin1Char(' ') + QString::number(rate_ / 1000.0, 'g', 4) + QStringLiteral("kHz");
}

void AudioOutput::start(const Settings &config)
{
    if (mode_ != Mode::Off)
        return;
    config_ = config;

    QAudioDevice audioDevice;
    if (config.backend != QLatin1String("null"))
        audioDevice = QMediaDevices::defaultAudioOutput();
    if (audioDevice.isNull())
        return startPump(config, QString());

    // The renderer produces stereo float; 16-bit stereo is converted.
    // Anything else (other channel counts, 32-bit int) would be noise.
    const int preferredRate = audioDevice.preferredFormat().sampleRate() > 0
                                  ? audioDevice.preferredFormat().sampleRate() : 44100;
    const int wantRate = config.samplerate > 0 ? config.samplerate : preferredRate;
    QAudioFormat chosen;
    auto tryFormat = [&](int rate, QAudioFormat::SampleFormat sampleFormat) {
        QAudioFormat f;
        f.setChannelCount(2);
        f.setSampleFormat(sampleFormat);
        f.setSampleRate(rate);
        if (!audioDevice.isFormatSupported(f))
            return false;
        chosen = f;
        return true;
    };
    if (!tryFormat(wantRate, QAudioFormat::Float) && !tryFormat(preferredRate, QAudioFormat::Float)
        && !tryFormat(wantRate, QAudioFormat::Int16) && !tryFormat(preferredRate, QAudioFormat::Int16))
        return startPump(
            config, QStringLiteral("%1 offers no stereo float or 16-bit format.").arg(audioDevice.description()));
    const int rate = chosen.sampleRate();
    if (config.samplerate > 0 && rate != config.samplerate)
        log_(QStringLiteral("warn"),
             QStringLiteral("Output rate %1 Hz unavailable, using %2 Hz").arg(config.samplerate).arg(rate));
    const bool int16 = chosen.sampleFormat() == QAudioFormat::Int16;

    device_ = audioDevice;
    const qint64 bytesPerSecond = qint64(rate) * 2 * (int16 ? 2 : 4);
    bufferBytes_ = std::max<qint64>(bytesPerSecond * config.bufferMs / 1000, bytesPerSecond * 25 / 1000);
    publish(QStringLiteral("qtaudio"), rate);
    source_->setFormat(rate, int16);
    sink_ = openSink(audioDevice, chosen);
    if (!sink_)
        return startPump(
            config, QStringLiteral("Audio device %1 could not be opened.").arg(audioDevice.description()));
    mode_ = Mode::Sink;
    log_(QStringLiteral("info"),
         QStringLiteral("Audio: %1 @ %2 Hz%3").arg(audioDevice.description(), QString::number(rate),
                                                 int16 ? QStringLiteral(", 16-bit") : QString()));
}

void AudioOutput::startPump(const Settings &config, const QString &why)
{
    mode_ = Mode::Pump;
    publish(QStringLiteral("null"), config.samplerate > 0 ? config.samplerate : 44100);
    source_->setFormat(rate_, false);
    // Null output uses the same dedicated thread, independent of the GUI.
    if (!pumpTimer_) {
        pumpTimer_ = new QTimer(this);
        pumpTimer_->setTimerType(Qt::PreciseTimer);
        connect(pumpTimer_, &QTimer::timeout, this, &AudioOutput::pump);
    }
    pumpTimer_->start(10);
    log_(why.isEmpty() ? QStringLiteral("info") : QStringLiteral("warn"),
         (why.isEmpty() ? QString() : why + QStringLiteral(" "))
             + QStringLiteral("Silent output (%1 Hz) — no audio device in use").arg(rate_));
}

void AudioOutput::pump()
{
    const int frames = std::max(16, rate_ / 100);       // ~10 ms
    scratch_.resize(frames * 2);
    source_->read(reinterpret_cast<char *>(scratch_.data()), frames * 2 * int(sizeof(float)));
}

QAudioSink *AudioOutput::openSink(const QAudioDevice &device, const QAudioFormat &format)
{
    auto *sink = new QAudioSink(device, format, this);
    sink->setBufferSize(bufferBytes_);
    watchSink(sink);
    sink->start(source_);
    if (sink->error() != QAudio::NoError) {
        delete sink;
        return nullptr;
    }
    return sink;
}

void AudioOutput::watchSink(QAudioSink *sink)
{
    // A device that fails to open, or disappears mid-play on a backend that
    // doesn't move the stream, stops the sink with an error and no further
    // pulls: playback would freeze without ever reaching its end.
    connect(sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State st) {
        if (st != QAudio::StoppedState || sink != sink_ || sink->error() == QAudio::NoError)
            return;
        const QAudio::Error err = sink->error();
        QMetaObject::invokeMethod(this, [this, sink, err] {
            if (sink != sink_)
                return;   // already replaced or stopped
            sink_ = nullptr;
            mode_ = Mode::Off;
            sink->deleteLater();
            recover(err == QAudio::OpenError ? QStringLiteral("Audio device could not be opened.")
                                             : QStringLiteral("Audio device lost."));
        }, Qt::QueuedConnection);
    });
}

void AudioOutput::recover(const QString &why)
{
    // Retry on the current default device; after repeated failures within a
    // short time, keep playing silently so the position and queue go on.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - recoverWindowStart_ > 10000) {
        recoverWindowStart_ = now;
        recoverCount_ = 0;
    }
    if (++recoverCount_ > 3)
        return startPump(config_, why);
    log_(QStringLiteral("warn"), why + QStringLiteral(" Reopening the audio output."));
    start(config_);
}

void AudioOutput::stop()
{
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    if (pumpTimer_)
        pumpTimer_->stop();
    mode_ = Mode::Off;
}

void AudioOutput::refresh(bool discardQueued)
{
    if (mode_ == Mode::Sink && discardQueued) {
        // Open the replacement BEFORE closing the old stream: a close/open gap
        // can send PulseAudio devices into idle/suspend and cause multi-second
        // wake delays. Only one source can pull at a time on this event loop;
        // the old buffered PCM is discarded as soon as the replacement is ready.
        QAudioSink *next = openSink(device_, sink_->format());
        if (!next) {   // keep the working old stream rather than lose playback
            log_(QStringLiteral("warn"),
                 QStringLiteral("Could not refresh the audio queue; continuing on the existing output stream."));
            return;
        }
        QAudioSink *old = sink_;
        sink_ = next;
        old->reset();
        delete old;
    } else if (mode_ == Mode::Pump) {
        pump();
        pumpTimer_->start(10);
    }
}
