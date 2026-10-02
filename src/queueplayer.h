#pragma once
// Plays the queue: a song by path or by queue position, the one before or
// after the playing song, the queue over again (Repeat queue), and on past a
// file that fails to load when the app chose it (a song the user picked keeps
// its error on screen). No UI: the window shows the playing row and the
// messages (signals).
#include <QObject>
#include <QString>

class Engine;
struct EngineSnapshot;
class IgnoreStore;
class ListeningStats;
class PlaybackQueue;
struct Settings;

class QueuePlayer : public QObject {
    Q_OBJECT
public:
    QueuePlayer(Engine &engine, PlaybackQueue &queue, ListeningStats &stats, const IgnoreStore &ignored,
                const Settings &settings, QObject *parent = nullptr);

    // skipDirection: the app chose this song (+1/-1: skip on that way if it
    // fails to load); 0 for a song the user picked, whose error stays shown
    void play(const QString &path, double position = 0.0, bool paused = false, int subsong = 0,
              bool preserveBufferedTail = false, int skipDirection = 0);
    void playQueued(int index, int direction, bool preserveBufferedTail = false);
    // `selected` is the queue's selected song: what plays when nothing
    // does, or where Previous and Next start when the playing song isn't known
    void playSelected(const QString &selected);
    void togglePlay(const QString &selected);
    void previous(const QString &selected);
    void next(const QString &selected);
    void restart(bool preserveBufferedTail = false);   // the queue from its start
    void stop();
    void songFinished();                    // Engine::finished: auto-advance
    void loadDone();                        // Engine::loadReady: skip a file that failed

    // The playing song (`snap`) was just ignored: stops it. The song after it
    // in the queue as it still is (with Repeat queue: from the start) plays
    // once the queue was rebuilt without it (playSuccessor).
    struct Successor {
        QString ignored;                    // the song that was playing
        QString path;                       // empty: none
        bool reshuffle = false;             // Repeat queue in shuffle: a new order instead
        bool paused = false;
    };
    Successor stopIgnored(const EngineSnapshot &snap);
    void playSuccessor(const Successor &next);

signals:
    void songStarted(const QString &path);
    void stopped();
    // Repeat queue in shuffle mode: draw a new order that doesn't open with
    // `finished` (empty: not in the queue) and rebuild the queue, before
    // this returns.
    void reshuffleNeeded(const QString &finished);
    void status(const QString &text);
    void logMessage(const QString &level, const QString &text);

private:
    Engine &engine_;
    PlaybackQueue &queue_;
    ListeningStats &stats_;
    const IgnoreStore &ignored_;
    const Settings &settings_;
    struct PendingLoad {                    // the load in flight, for skipping broken files
        quint64 generation = 0;             // its song generation (0: none)
        int direction = 0;                  // +1/-1: the app chose it; 0: the user did
    } pendingLoad_;
    int skipRun_ = 0;                       // broken files skipped in a row
};
