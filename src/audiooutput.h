// Audio output on the audio thread: a QAudioSink pulling PCM from a live
// source, or, when no device can be used, a timer that pumps the source
// silently so position, end of song and the queue go on.
#pragma once

#include "config.h"

#include <QAudioDevice>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <functional>

class QAudioFormat;
class QAudioSink;
class QTimer;

// A never-ending stereo PCM stream: float at the given rate, or 16-bit for
// devices without float output.
class AudioSource : public QIODevice {
public:
    using QIODevice::QIODevice;
    virtual void setFormat(int sampleRate, bool int16) = 0;
};

class AudioOutput : public QObject {
public:
    using LogFunction = std::function<void(const QString &level, const QString &text)>;
    explicit AudioOutput(LogFunction log);

    // Audio thread only.
    void setSource(AudioSource *source);
    bool isRunning() const { return mode_ != Mode::Off; }
    void start(const Settings &config);      // no-op while running
    void stop();
    // After a file switch: let the new file start without waiting for the
    // next pull, and with discardQueued drop the old file's queued PCM.
    void refresh(bool discardQueued);

    // Any thread.
    QString description() const;             // "qtaudio 48kHz"

private:
    enum class Mode { Off, Sink, Pump };
    void startPump(const Settings &config, const QString &why);
    QAudioSink *openSink(const QAudioDevice &device, const QAudioFormat &format);
    void watchSink(QAudioSink *sink);
    void recover(const QString &why);
    void pump();
    void publish(const QString &name, int rate);

    LogFunction log_;
    AudioSource *source_ = nullptr;
    Mode mode_ = Mode::Off;
    QAudioSink *sink_ = nullptr;             // Mode::Sink
    QAudioDevice device_;
    qint64 bufferBytes_ = 0;
    QTimer *pumpTimer_ = nullptr;            // Mode::Pump
    QVector<float> scratch_;
    Settings config_;                        // for reopening after a device failure
    qint64 recoverWindowStart_ = 0;
    int recoverCount_ = 0;

    mutable QMutex publishedMutex_;          // name_/rate_ are written on the audio thread only
    QString name_;
    int rate_ = 44100;
};
