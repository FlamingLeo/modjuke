// Screenshot driver, for README/documentation captures only (CMake option
// MODJUKE_SHOT_DRIVER, off by default). MODJUKE_SHOTS=<dir>[:<w>x<h>] captures
// the player tab, the tracker tab and every dialog as PNG files, then quits.
// Use it with a virtual display (xvfb-run).
#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QRegularExpression>
#include <QTabBar>
#include <QTimer>

void MainWindow::startShotDriver()
{
    const QString spec = QString::fromLocal8Bit(qgetenv("MODJUKE_SHOTS"));
    const QString dir = spec.split(QLatin1Char(':')).first();
    const QString sizePart =
        spec.contains(QLatin1Char(':')) ? spec.section(QLatin1Char(':'), 1) : QString();
    static const QRegularExpression sizeRe(QStringLiteral("(\\d+)x(\\d+)"));
    const QRegularExpressionMatch sizeMatch = sizeRe.match(sizePart);
    if (sizeMatch.hasMatch())
        resize(sizeMatch.captured(1).toInt(), sizeMatch.captured(2).toInt());
    else
        resize(1180, 720);
    QDir().mkpath(dir);

    auto *timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, [this, dir, timer, step = 0]() mutable {
        auto saveTab = [this, dir](int index, const QString &name) {
            tabBar_->setCurrentIndex(index);
            qApp->processEvents();
            qApp->processEvents();
            grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
        };
        auto captureDialog = [dir](QDialog *dialog, const QString &name) {
            dialog->setAttribute(Qt::WA_DontShowOnScreen);
            dialog->show();
            qApp->processEvents();
            dialog->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
            dialog->hide();
            dialog->deleteLater();
        };
        switch (step) {
        case 0:
            saveTab(0, QStringLiteral("player"));
            break;
        case 1:
            saveTab(1, QStringLiteral("tracker"));
            break;
        case 2:
            saveTab(0, QStringLiteral("player_returned"));
            break;
        case 3: {
            OpenMPTLib *lib = OpenMPTLib::instance();
            captureDialog(new SongInfoDialog(nullptr, engine_.snapshot().info), QStringLiteral("songinfo"));
            captureDialog(new StatsDialog(nullptr, &stats_.store()), QStringLiteral("stats"));
            captureDialog(new IgnoreDialog(nullptr, &ignored_, {}), QStringLiteral("ignored"));
            captureDialog(new FilterDialog(nullptr, builder_.filter(), library_.tracks()),
                          QStringLiteral("filter"));
            captureDialog(new SettingsDialog(nullptr, settings_, &ignored_, {}),
                          QStringLiteral("settings"));
            captureDialog(new PlaylistsDialog(nullptr, &playlists_, [this] { return queuePanel_->selectedPaths(); }),
                          QStringLiteral("playlists"));
            captureDialog(new AboutDialog(nullptr, lib ? lib->versionString() : QString(),
                                          lib != nullptr),
                          QStringLiteral("about"));
            break;
        }
        default:
            timer->stop();
            qApp->quit();
            return;
        }
        ++step;
    });
    timer->start(2500);
}
