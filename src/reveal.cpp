#include "reveal.h"
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif
#include <QDesktopServices>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

void revealFile(const QString &path, QObject *context,
                std::function<void(bool, bool, const QString &)> done)
{
    const QFileInfo file(path);
    if (!file.exists()) {
        done(false, false, QObject::tr("The file no longer exists: %1").arg(path));
        return;
    }
    const QString absolute = file.absoluteFilePath();
    auto fallback = [absolute, done] {
#if defined(Q_OS_WIN)
        const bool ok = QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(absolute)});
        done(ok, ok, ok ? QObject::tr("Selected file in Explorer") : QObject::tr("Could not open Explorer"));
#elif defined(Q_OS_MACOS)
        const bool ok = QProcess::startDetached("/usr/bin/open", {"-R", absolute});
        done(ok, ok, ok ? QObject::tr("Selected file in Finder") : QObject::tr("Could not open Finder"));
#else
        // Mint/Nemo and the other common managers accept the file itself.
        const QList<QPair<QString, QStringList>> managers = {
            {"nemo", {}}, {"nautilus", {"--select"}}, {"dolphin", {"--select"}},
            {"caja", {"--select"}}, {"thunar", {}}, {"pcmanfm-qt", {}}, {"pcmanfm", {}}
        };
        for (const auto &candidate : managers) {
            const QString program = QStandardPaths::findExecutable(candidate.first);
            if (!program.isEmpty() && QProcess::startDetached(program, candidate.second + QStringList{absolute})) {
                done(true, true, QObject::tr("Requested file selection in %1").arg(candidate.first));
                return;
            }
        }
        const bool ok = QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(absolute).absolutePath()));
        done(ok, false, ok ? QObject::tr("Opened folder (file selection is unavailable)")
                          : QObject::tr("Could not open a file manager"));
#endif
    };
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    auto message = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
        "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems");
    message.setArguments({QStringList{QUrl::fromLocalFile(absolute).toString(QUrl::FullyEncoded)}, QString()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
        [watcher, done, fallback] {
            const bool ok = watcher->reply().type() != QDBusMessage::ErrorMessage;
            watcher->deleteLater();
            if (ok) done(true, true, QObject::tr("Selected file in its folder"));
            else fallback();
        });
#else
    Q_UNUSED(context);
    fallback();
#endif
}
