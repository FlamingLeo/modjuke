#pragma once
#include <QObject>
#include <QString>
#include <functional>

// Asynchronous file-manager selection. No shell expansion or GUI-thread waits.
// selected is false only when selection isn't supported and a folder fallback opened.
void revealFile(const QString &path, QObject *context,
                std::function<void(bool ok, bool selected, const QString &message)> done);
