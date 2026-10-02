// Background library analysis: fills duration/format/title per track,
// using (and updating) the shared analysis cache. Port of library.py Analyzer.
#pragma once

#include "stores.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

class OpenMPTLib;

// (alias so the comma in QMap<...,...> does not break Q_DECLARE_METATYPE)
using AnalysisResults = QMap<QString, CachedModule>;
Q_DECLARE_METATYPE(AnalysisResults)

// One module file's library record, with the file's size and mtime; broken is
// set when libopenmpt cannot load it.
CachedModule analyzeModule(const OpenMPTLib &lib, const QString &path);

class Analyzer : public QObject {
    Q_OBJECT
public:
    explicit Analyzer(QObject *parent = nullptr);
    ~Analyzer() override;

    bool isRunning() const { return thread_ != nullptr; }

    // Analyze the given paths (those already in the cache resolve instantly).
    // Emits resultsReady() when done with every path's CachedModule.
    void start(const QStringList &paths, bool useCache);
    void cancel() { cancel_ = true; }

signals:
    void progress(int done, int total);
    void resultsReady(const AnalysisResults &results);
    // Emitted only after the worker thread has stopped; start() is safe again.
    void finished(const QString &error, bool canceled);

private:
    QString run(const QStringList &paths, bool useCache);   // on thread_; returns the error
    QThread *thread_ = nullptr;
    std::atomic_bool cancel_{false};
    QString error_;
};
