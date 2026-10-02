#pragma once
// The library folder: its scanned tracks with the cached analysis applied,
// and the background analysis that fills in the rest. No UI: the window
// shows the progress and messages.
#include "analyzer.h"
#include "library.h"

#include <QHash>
#include <QObject>

class MusicLibrary : public QObject {
    Q_OBJECT
public:
    explicit MusicLibrary(QObject *parent = nullptr);

    QString root() const { return root_; }
    int dirCount() const { return dirs_; }
    const QVector<Track> &tracks() const { return tracks_; }
    const Track *find(const QString &path) const;

    // Scan `root`; a running analysis is canceled and restarted afterwards
    // (analysisSuperseded). Returns the scan's problems.
    QStringList rescan(const QString &root);
    int pendingCount() const;               // tracks not analyzed yet
    bool analyze(bool useCache);            // false: nothing to analyze
    bool analyzing() const { return analyzer_.isRunning(); }
    void cancelAnalysis() { analyzer_.cancel(); }

signals:
    void tracksChanged(int analyzed);       // analysis results were applied
    void analysisProgress(int done, int total);
    void analysisFinished(const QString &error, bool canceled, int analyzed);
    void analysisSuperseded();              // stopped for a rescan; analyze again if wanted

private:
    Analyzer analyzer_;
    QString root_;
    QVector<Track> tracks_;                 // scan order (all formats)
    QHash<QString, int> index_;             // path -> index in tracks_
    int dirs_ = 0;
    int analyzed_ = 0;                      // results of the current analysis
    bool restartPending_ = false;           // a rescan canceled the running analysis
};
