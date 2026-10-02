#include "analyzer.h"

#include "config.h"
#include "library.h"
#include "openmptapi.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <algorithm>

// The heavy lifting runs inside Worker::run on a secondary thread; the only
// GUI contact is queued signal emission.
class Analyzer::Worker : public QObject {
    Q_OBJECT
public:
    Worker(QStringList paths, bool useCache, Analyzer *parent)
        : paths_(std::move(paths)), useCache_(useCache), owner_(parent)
    {
    }

signals:
    void runFinished(const QString &error);

public slots:
    void run()
    {
        QString libError;
        OpenMPTLib *lib = OpenMPTLib::instance(&libError);
        if (!lib) {
            emit runFinished(libError);
            return;
        }
        AnalysisCache cache;
        QMap<QString, CachedModule> results;
        if (useCache_) {
            cache.load();
            for (const QString &path : paths_) {
                const QFileInfo info(path);
                CachedModule entry;
                if (cache.apply(info.absoluteFilePath(), info.size(),
                                fileMTime(path), &entry))
                    results.insert(path, entry);
            }
        }
        int done = 0;
        const int total = paths_.size();
        bool dirty = false;
        for (const QString &path : paths_) {
            ++done;
            if (owner_->cancel_ || QThread::currentThread()->isInterruptionRequested()) {
                break;
            }
            if (results.contains(path))
                continue;
            const QFileInfo info(path);
            CachedModule entry;
            entry.size = info.size();
            entry.mtime = fileMTime(path);
            OpenMPTModule module = lib->openFile(path, {{"load.skip_samples", "1"}}, nullptr);
            if (!module.isOpen()) {
                entry.broken = QStringLiteral("could not load module");
            } else {
                const ModuleInfo meta = module.info(path);
                entry.duration = meta.duration;
                entry.fmt = meta.format;
                entry.channels = meta.channels;
                entry.subsongs = std::max(1, meta.subsongs);
                entry.title = meta.title;
                if (!meta.ok && meta.title.isEmpty())
                    entry.broken = QStringLiteral("could not load module");
            }
            cache.remember(info.absoluteFilePath(), entry);
            dirty = true;
            results.insert(path, entry);
            // a long first run over a big library: a crash or kill keeps most
            // of the work instead of none
            if (useCache_ && done % 500 == 0 && cache.save())
                dirty = false;
            if (done % 8 == 0 || done == total)
                emit owner_->progress(done, total);
        }
        if (dirty && useCache_)
            cache.save();
        emit owner_->resultsReady(results);
        emit runFinished(QString());
    }

private:
    QStringList paths_;
    bool useCache_;
    Analyzer *owner_;
};

Analyzer::Analyzer(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<AnalysisResults>("AnalysisResults");
}

Analyzer::~Analyzer()
{
    cancel_ = true;
    if (thread_) {
        thread_->requestInterruption();
        thread_->quit();
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
    worker_ = new Worker(paths, useCache, this);
    thread_ = new QThread(this);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::started, worker_, &Worker::run);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::runFinished, this, [this](const QString &error) {
        error_ = error;
        thread_->quit();
    });
    connect(thread_, &QThread::finished, this, [this] {
        thread_->deleteLater();
        thread_ = nullptr;
        worker_ = nullptr;
        emit finished(error_, cancel_.load());
    });
    thread_->start();
}

#include "analyzer.moc"
