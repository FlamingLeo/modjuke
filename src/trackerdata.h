// Tracker view data (order list, pattern cells) decoded on a dedicated
// metadata thread from a second, sample-less copy of the playing module.
#pragma once

#include "openmptapi.h"

#include <QAtomicInt>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

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

Q_DECLARE_METATYPE(SongDataReply)
Q_DECLARE_METATYPE(PatternDataReply)

// Lives on the metadata thread; call its request methods there (queued).
// Tracker formatting can involve thousands of native calls, so it never runs
// on the output thread or under the playback snapshot lock. A request whose
// token or load epoch is no longer current is dropped, also mid-pattern.
class MetadataWorker : public QObject {
    Q_OBJECT
public:
    MetadataWorker(const QAtomicInt *currentToken, const QAtomicInteger<quint64> *currentLoadEpoch);

    void requestSong(int token, const QString &path, int subsong, quint64 generation, quint64 loadEpoch);
    // channelLimit 0 = all channels of the module
    void requestPattern(int token, const QString &path, int subsong, quint64 loadEpoch,
                        int pattern, int channelLimit);

signals:
    void songReady(const SongDataReply &reply);
    void patternReady(const PatternDataReply &reply, quint64 loadEpoch);

private:
    bool prepare(int token, const QString &path, int subsong, quint64 loadEpoch);
    bool current(int token) const { return token == currentToken_->loadRelaxed(); }

    const QAtomicInt *currentToken_;
    const QAtomicInteger<quint64> *currentLoadEpoch_;
    QString path_;
    quint64 loadEpoch_ = 0;
    OpenMPTModule module_;
};
