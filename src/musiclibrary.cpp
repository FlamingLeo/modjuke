#include "musiclibrary.h"

#include "openmptapi.h"
#include "stores.h"

#include <QFileInfo>

#include <algorithm>

MusicLibrary::MusicLibrary(QObject *parent) : QObject(parent)
{
    connect(&analyzer_, &Analyzer::resultsReady, this, [this](const AnalysisResults &results) {
        if (restartPending_)
            return;   // a rescan superseded this job
        for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
            if (const int i = index_.value(it.key(), -1); i >= 0)
                tracks_[i].applyAnalysis(it.value());
        }
        analyzed_ = results.size();
        emit tracksChanged(analyzed_);
    });
    connect(&analyzer_, &Analyzer::finished, this, [this](const QString &error, bool canceled) {
        if (restartPending_) {
            restartPending_ = false;
            emit analysisSuperseded();
            return;
        }
        emit analysisFinished(error, canceled, analyzed_);
    });
    connect(&analyzer_, &Analyzer::progress, this, [this](int done, int total) {
        if (!restartPending_)
            emit analysisProgress(done, total);
    });
}

const Track *MusicLibrary::find(const QString &path) const
{
    const int i = index_.value(path, -1);
    return i >= 0 ? &tracks_[i] : nullptr;
}

QStringList MusicLibrary::rescan(const QString &root)
{
    if (analyzer_.isRunning()) {
        restartPending_ = true;
        analyzer_.cancel();
    }
    root_ = root;
    ScanResult result = scanLibrary(root_, libraryExtensions());
    tracks_ = std::move(result.tracks);
    dirs_ = result.dirs;
    index_.clear();
    index_.reserve(tracks_.size());
    AnalysisCache cache;
    cache.load();
    for (int i = 0; i < tracks_.size(); ++i) {
        Track &track = tracks_[i];
        // a path listed twice keeps its last place, as the lookup always did
        index_.insert(track.path, i);
        const QFileInfo info(track.path);
        CachedModule entry;
        if (cache.apply(info.exists() ? info.absoluteFilePath() : track.path, info.size(),
                        fileMTime(track.path), &entry))
            track.applyAnalysis(entry);
    }
    return result.errors;
}

int MusicLibrary::pendingCount() const
{
    return int(std::count_if(tracks_.cbegin(), tracks_.cend(), [](const Track &t) { return !t.analyzed; }));
}

bool MusicLibrary::analyze(bool useCache)
{
    QStringList pending;
    for (const Track &track : tracks_) {
        if (!track.analyzed)
            pending << track.path;
    }
    if (pending.isEmpty())
        return false;
    analyzed_ = 0;
    analyzer_.start(pending, useCache);
    return true;
}
