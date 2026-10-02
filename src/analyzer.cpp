#include "analyzer.h"

#include "library.h"
#include "openmptapi.h"

#include <QFileInfo>
#include <QThread>
#include <algorithm>

CachedModule analyzeModule(const OpenMPTLib &lib, const QString &path)
{
    CachedModule entry;
    entry.size = QFileInfo(path).size();
    entry.mtime = fileMTime(path);
    // none of the fields needs the sample data
    const OpenMPTModule module = lib.openFile(path, {{"load.skip_samples", "1"}}, nullptr);
    if (!module.isOpen()) {
        entry.broken = QStringLiteral("could not load module");
        return entry;
    }
    const ModuleInfo meta = module.summary(path);
    entry.duration = meta.duration;
    entry.fmt = meta.format;
    entry.channels = meta.channels;
    entry.subsongs = std::max(1, meta.subsongs);
    entry.title = meta.title;
    return entry;
}

Analyzer::Analyzer(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<AnalysisResults>("AnalysisResults");
}

Analyzer::~Analyzer()
{
    cancel_ = true;
    if (thread_) {
        thread_->requestInterruption();
        if (!thread_->wait(3000))
            thread_->wait();   // the worker only blocks on file I/O; let it land
    }
}

void Analyzer::start(const QStringList &paths, bool useCache)
{
    if (thread_)
        return;
    cancel_ = false;
    error_.clear();
    // error_ is read only after the thread has finished
    thread_ = QThread::create([this, paths, useCache] { error_ = run(paths, useCache); });
    thread_->setParent(this);
    connect(thread_, &QThread::finished, this, [this] {
        thread_->deleteLater();
        thread_ = nullptr;
        emit finished(error_, cancel_.load());
    });
    thread_->start();
}

// The heavy lifting runs here on thread_; the only GUI contact is queued
// signal emission.
QString Analyzer::run(const QStringList &paths, bool useCache)
{
    QString libError;
    const OpenMPTLib *lib = OpenMPTLib::instance(&libError);
    if (!lib)
        return libError;
    AnalysisCache cache;
    AnalysisResults results;
    if (useCache) {
        cache.load();
        for (const QString &path : paths) {
            const QFileInfo info(path);
            CachedModule entry;
            if (cache.apply(info.absoluteFilePath(), info.size(), fileMTime(path), &entry))
                results.insert(path, entry);
        }
    }
    int done = 0;
    const int total = paths.size();
    bool dirty = false;
    for (const QString &path : paths) {
        ++done;
        if (cancel_ || QThread::currentThread()->isInterruptionRequested())
            break;
        if (results.contains(path))
            continue;
        const CachedModule entry = analyzeModule(*lib, path);
        cache.remember(QFileInfo(path).absoluteFilePath(), entry);
        dirty = true;
        results.insert(path, entry);
        // a long first run over a big library: a crash or kill keeps most
        // of the work instead of none
        if (useCache && done % 500 == 0 && cache.save())
            dirty = false;
        if (done % 8 == 0 || done == total)
            emit progress(done, total);
    }
    if (dirty && useCache)
        cache.save();
    emit resultsReady(results);
    return QString();
}
