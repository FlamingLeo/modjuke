// modjuke (Qt port) entry point.
//
// GUI:   modjuke [dir] [--track PATH] [--autoplay] [--volume N] [--theme K]
//                    [--speed F] [--interpolation m] [--backend auto|null]
// CLI:   modjuke --scan DIR [--order] [--analyze]     (prints the queue, exits)
//        modjuke --check                               (diagnostics, exits)
#include "analyzer.h"
#include "config.h"
#include "library.h"
#include "mainwindow.h"
#include "openmptapi.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

static QStringList defaultExtensions()
{
    return {QStringLiteral("669"), QStringLiteral("amf"), QStringLiteral("ams"),
            QStringLiteral("dbm"), QStringLiteral("dmf"), QStringLiteral("dsm"),
            QStringLiteral("dtm"), QStringLiteral("far"), QStringLiteral("gdm"),
            QStringLiteral("it"), QStringLiteral("j2b"), QStringLiteral("med"),
            QStringLiteral("mdl"), QStringLiteral("mod"), QStringLiteral("mptm"),
            QStringLiteral("mt2"), QStringLiteral("mtm"), QStringLiteral("noiser"),
            QStringLiteral("okta"), QStringLiteral("pt3"), QStringLiteral("s3m"),
            QStringLiteral("sfx"), QStringLiteral("stm"), QStringLiteral("stx"),
            QStringLiteral("ult"), QStringLiteral("wow"), QStringLiteral("xm")};
}

static QStringList supportedExtensions()
{
    OpenMPTLib *lib = OpenMPTLib::instance();
    if (lib) {
        const QStringList exts = lib->supportedExtensions();
        if (!exts.isEmpty())
            return exts;
    }
    return defaultExtensions();
}

static int runScan(const QString &root, bool ordered, bool analyze)
{
    QTextStream out(stdout);
    ScanResult result = scanLibrary(root, supportedExtensions());
    QVector<Track> tracks = result.tracks;
    if (analyze) {
        for (Track &track : tracks) {
            OpenMPTLib *lib = OpenMPTLib::instance();
            if (!lib)
                break;
            OpenMPTModule module = lib->openFile(track.path, {}, nullptr);
            if (!module.isOpen()) {
                track.broken = QStringLiteral("could not load module");
                track.analyzed = true;
                continue;
            }
            const ModuleInfo info = module.info(track.path);
            track.analyzed = true;
            track.duration = info.duration;
            track.fmt = info.format;
            track.channels = info.channels;
            track.subsongs = std::max(1, info.subsongs);
            track.title = info.title;
            if (!info.ok)
                track.broken = QStringLiteral("could not load module");
        }
    }
    if (ordered)
        tracks = orderTracks(tracks, OrderMode::Alphabetical, 0);
    for (const Track &track : tracks)
        out << track.durationText() << "\t" << track.fmt << "\t"
            << (track.broken.isEmpty() ? QString() : QStringLiteral("!broken\t"))
            << track.path << "\n";
    out << "# " << result.tracks.size() << " tracks, " << result.dirs << " folders"
        << (result.canceled ? ", canceled" : "") << "\n";
    for (const QString &error : result.errors)
        out << "! " << error << "\n";
    out.flush();
    return result.errors.isEmpty() ? 0 : 1;
}

static int runCheck()
{
    QTextStream out(stdout);
    out << "config dir: " << modjukeConfigDir() << "\n";
    QString error;
    OpenMPTLib *lib = OpenMPTLib::instance(&error);
    if (lib) {
        out << "libopenmpt: " << lib->versionString() << "\n";
        out << "formats: " << lib->supportedExtensions().size() << " extensions\n";
    } else {
        out << "libopenmpt: NOT FOUND (" << error << ")\n";
        return 1;
    }
    Settings settings = Settings::load();
    out << "theme: " << settings.theme << "\n";
    out << "backend: " << (settings.backend.isEmpty() ? QStringLiteral("auto") : settings.backend)
        << "\n";
    return 0;
}

int main(int argc, char **argv)
{
    // headless subcommands run without a display
    auto isArg = [argc, argv](const char *name) {
        for (int i = 1; i < argc; ++i)
            if (qstrcmp(argv[i], name) == 0)
                return true;
        return false;
    };
    const bool headless = isArg("--scan") || isArg("--check") || isArg("--version");
    if (headless) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("modjuke"));
        QCoreApplication::setApplicationVersion(QStringLiteral("1.0-qt"));
        if (isArg("--version")) {
            QTextStream(stdout) << QStringLiteral("modjuke %1\n").arg(QCoreApplication::applicationVersion());
            return 0;
        }
        QStringList args;
        for (int i = 1; i < argc; ++i)
            args << QString::fromLocal8Bit(argv[i]);
        if (isArg("--check"))
            return runCheck();
        // --scan DIR [--order] [--analyze]
        const int idx = args.indexOf(QStringLiteral("--scan"));
        QString root = idx + 1 < args.size() && !args.at(idx + 1).startsWith(QLatin1Char('-'))
            ? args.at(idx + 1)
            : Settings::load().lastDirectory;
        if (root.isEmpty()) {
            QTextStream(stderr) << "usage: modjuke --scan DIR [--order] [--analyze]\n";
            return 2;
        }
        return runScan(QFileInfo(root).absoluteFilePath(),
                       args.contains(QStringLiteral("--order")),
                       args.contains(QStringLiteral("--analyze")));
    }

    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/modjuke/icon.png")));
    QApplication::setApplicationName(QStringLiteral("modjuke"));
    QApplication::setApplicationVersion(QStringLiteral("1.0-qt"));
    QApplication::setOrganizationName(QStringLiteral("modjuke"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("modjuke - tracker music player"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dirOption(QStringLiteral("dir"), QObject::tr("Library folder."), QStringLiteral("path"));
    const QCommandLineOption trackOption(QStringLiteral("track"), QObject::tr("Play this file at start."), QStringLiteral("path"));
    const QCommandLineOption autoplayOption(QStringLiteral("autoplay"), QObject::tr("Start playing immediately."));
    const QCommandLineOption volumeOption(QStringLiteral("volume"), QObject::tr("Volume 0-100."), QStringLiteral("n"));
    const QCommandLineOption themeOption(QStringLiteral("theme"), QObject::tr("Theme key."), QStringLiteral("key"));
    const QCommandLineOption speedOption(QStringLiteral("speed"), QObject::tr("Playback speed factor."), QStringLiteral("f"));
    const QCommandLineOption interpOption(QStringLiteral("interpolation"), QObject::tr("off|linear|cubic|sinc."), QStringLiteral("mode"));
    const QCommandLineOption backendOption(QStringLiteral("backend"), QObject::tr("auto|null."), QStringLiteral("name"));
    for (const QCommandLineOption *opt : {&dirOption, &trackOption, &autoplayOption, &volumeOption,
                                          &themeOption, &speedOption, &interpOption, &backendOption})
        parser.addOption(*opt);
    parser.addPositionalArgument(QStringLiteral("dir"), QObject::tr("Library folder (positional)."));
    parser.process(app);

    MainWindow window;

    QHash<QString, QString> overrides;
    if (parser.isSet(volumeOption))
        overrides.insert(QStringLiteral("volume"), parser.value(volumeOption));
    if (parser.isSet(themeOption))
        overrides.insert(QStringLiteral("theme"), parser.value(themeOption));
    if (parser.isSet(speedOption))
        overrides.insert(QStringLiteral("speed"), parser.value(speedOption));
    if (parser.isSet(interpOption))
        overrides.insert(QStringLiteral("interpolation"), parser.value(interpOption));
    if (parser.isSet(backendOption))
        overrides.insert(QStringLiteral("backend"), parser.value(backendOption));
    window.applyCliOverrides(overrides);

    QString dir = parser.value(dirOption);
    if (dir.isEmpty() && !parser.positionalArguments().isEmpty())
        dir = parser.positionalArguments().first();
    if (!dir.isEmpty())
        window.openDirectory(dir);

    window.show();

    window.startupPlay(parser.value(trackOption), parser.isSet(autoplayOption));
    return app.exec();
}
