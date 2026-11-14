#pragma once
// The library folder and the playlist songs outside it: their tracks with
// the cached analysis applied, and the background analysis that fills in
// the rest. No UI: the window
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
    const Track *find(const QString &path) const;   // a library or playlist song
    // Songs of all playlists. Those outside the folder are kept with their
    // cached analysis and analyzed along with the library.
    void setPlaylistPaths(const QStringList &paths);

    // Scan `root`; a running analysis is canceled and restarted afterwards
    // (analysisSuperseded). Returns the scan's problems.
    QStringList rescan(const QString &root);
    int pendingCount() const;               // tracks and playlist songs not analyzed yet
    bool analyze(bool useCache);            // false: nothing to analyze
    bool analyzing() const { return analyzer_.isRunning(); }
    void cancelAnalysis() { analyzer_.cancel(); }

signals:
    void tracksChanged(int analyzed);       // analysis results were applied
    void analysisProgress(int done, int total);
    void analysisFinished(const QString &error, bool canceled, int analyzed);
    void analysisSuperseded();              // stopped for a rescan; analyze again if wanted

private:
    // `cache`: the loaded analysis cache, or nullptr to load it if needed;
    // songs already kept stay as they are
    void updateOutside(AnalysisCache *cache);

    Analyzer analyzer_;
    QString root_;
    QVector<Track> tracks_;                 // scan order (all formats)
    QHash<QString, int> index_;             // path -> index in tracks_
    QStringList playlistPaths_;
    QStringList outsideOrder_;              // playlist songs outside the folder, in playlist order
    QHash<QString, Track> outside_;         // path -> its track
    int dirs_ = 0;
    int analyzed_ = 0;                      // results of the current analysis
    bool restartPending_ = false;           // a rescan canceled the running analysis
};
