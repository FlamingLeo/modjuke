// modjuke (Qt port) entry point.
//
// GUI:   modjuke [dir] [--track PATH] [--autoplay] [--volume N] [--theme K]
//                    [--speed F] [--interpolation m] [--backend auto|null]
// CLI:   modjuke --scan DIR [--order] [--analyze]     (prints the queue, exits)
//        modjuke --check                               (diagnostics, exits)
#include "analyzer.h"
#include "casefold.h"
#include "config.h"
#include "library.h"
#include "mainwindow.h"
#include "openmptapi.h"
#include "theme.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

// The Windows build is a GUI program and has no console of its own. With
// arguments (--check, --scan, --version, an invalid option) the output goes
// to the console it was started from, unless it is redirected to a file or
// pipe already. cmd doesn't wait for GUI programs, so it appears after the
// prompt; redirect it (modjuke --check > check.txt) for scripts.
static void attachParentConsole()
{
    auto usable = [](DWORD id) {
        const HANDLE h = GetStdHandle(id);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    const bool out = usable(STD_OUTPUT_HANDLE);
    const bool err = usable(STD_ERROR_HANDLE);
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    if (!out)
        std::freopen("CONOUT$", "w", stdout);
    if (!err)
        std::freopen("CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);   // QTextStream writes UTF-8
}
#endif

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
        QString loadError;
        if (!OpenMPTLib::instance(&loadError)) {
            // an unanalyzed listing with exit 0 looked like a successful run
            QTextStream(stderr) << "libopenmpt: NOT FOUND (" << loadError << ")\n";
            return 1;
        }
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
        out << "libopenmpt: " << lib->versionString() << " (" << lib->libraryPath() << ")\n";
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
#ifdef _WIN32
    if (argc > 1)
        attachParentConsole();
#endif
    // headless subcommands run without a display
    auto isArg = [argc, argv](const char *name) {
        for (int i = 1; i < argc; ++i)
            if (qstrcmp(argv[i], name) == 0)
                return true;
        return false;
    };
    auto hasPrefix = [argc, argv](const char *prefix) {
        for (int i = 1; i < argc; ++i)
            if (qstrncmp(argv[i], prefix, qstrlen(prefix)) == 0)
                return true;
        return false;
    };
    const bool headless = isArg("--scan") || hasPrefix("--scan=") || isArg("--check")
                          || isArg("--version") || isArg("-v");
    if (headless) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("modjuke"));
        QCoreApplication::setApplicationVersion(QStringLiteral("1.0-qt"));
        if (isArg("--version") || isArg("-v")) {
            QTextStream(stdout) << QStringLiteral("modjuke %1\n").arg(QCoreApplication::applicationVersion());
            return 0;
        }
        // arguments() also gets non-ASCII paths right on Windows (argv is in
        // the ANSI code page there)
        const QStringList args = QCoreApplication::arguments().mid(1);
        if (isArg("--check"))
            return runCheck();
        // --scan DIR [--order] [--analyze]   (also --scan=DIR)
        QString root;
        int idx = args.indexOf(QStringLiteral("--scan"));
        for (int i = 0; i < args.size(); ++i) {
            if (args.at(i).startsWith(QLatin1String("--scan="))) {
                root = args.at(i).mid(7);
                idx = i;
            }
        }
        if (root.isEmpty() && idx + 1 < args.size() && !args.at(idx + 1).startsWith(QLatin1Char('-')))
            root = args.at(idx + 1);
        // a typo like --analyse silently skipped the analysis
        for (int i = 0; i < args.size(); ++i) {
            const QString &a = args.at(i);
            if (i == idx || a == QLatin1String("--order") || a == QLatin1String("--analyze")
                || (i == idx + 1 && a == root))
                continue;
            QTextStream(stderr) << "modjuke: unknown option for --scan: " << a << "\n"
                                << "usage: modjuke --scan DIR [--order] [--analyze]\n";
            return 2;
        }
        if (root.isEmpty())
            root = Settings::load().lastDirectory;   // no DIR: the last library folder
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

    // Check every value before anything is applied or saved: "--volume 70%"
    // used to become 0, "--backend pulse" or a mistyped theme were stored.
    auto fail = [](const QString &message) {
        QTextStream(stderr) << "modjuke: " << message << "\n";
        return 2;
    };
    QHash<QString, QString> overrides;
    if (parser.isSet(volumeOption)) {
        bool ok = false;
        const int volume = parser.value(volumeOption).trimmed().toInt(&ok);
        if (!ok || volume < 0 || volume > 100)
            return fail(QStringLiteral("--volume takes a number from 0 to 100"));
        overrides.insert(QStringLiteral("volume"), QString::number(volume));
    }
    if (parser.isSet(themeOption)) {
        const QString theme = parser.value(themeOption);
        const QString folded = caseFold(theme.trimmed());
        if (Palette::normalizeTheme(theme, Settings::load().customThemes) == QLatin1String("dark")
            && folded != QLatin1String("dark") && folded != QLatin1String("default"))
            return fail(QStringLiteral("unknown theme \"%1\" (built in: %2, or a custom theme's name)")
                            .arg(theme, Palette::builtinThemes().join(QStringLiteral(", "))));
        overrides.insert(QStringLiteral("theme"), theme);
    }
    if (parser.isSet(speedOption)) {
        bool ok = false;
        const double speed = parser.value(speedOption).trimmed().toDouble(&ok);
        if (!ok || !(speed >= 0.05 && speed <= 20.0))
            return fail(QStringLiteral("--speed takes a factor from 0.05 to 20"));
        overrides.insert(QStringLiteral("speed"), parser.value(speedOption).trimmed());
    }
    if (parser.isSet(interpOption)) {
        const QString mode = parser.value(interpOption).trimmed().toLower();
        if (Engine::interpolationLength(mode) <= 0)
            return fail(QStringLiteral("--interpolation takes off, linear, cubic or sinc"));
        // stored by name ("8" would be reset to sinc by the settings loader anyway)
        overrides.insert(QStringLiteral("interpolation"),
                         Engine::interpolationName(Engine::interpolationLength(mode)));
    }
    if (parser.isSet(backendOption)) {
        const QString backend = parser.value(backendOption).trimmed().toLower();
        if (backend != QLatin1String("auto") && backend != QLatin1String("null"))
            return fail(QStringLiteral("--backend takes auto or null"));
        overrides.insert(QStringLiteral("backend"), backend);
    }
    QString dir = parser.value(dirOption);
    if (dir.isEmpty() && !parser.positionalArguments().isEmpty())
        dir = parser.positionalArguments().first();
    // a mistyped or file path would replace the remembered library folder
    if (!dir.isEmpty() && !QFileInfo(dir).isDir())
        return fail(QFileInfo(dir).exists()
                        ? QStringLiteral("%1 is not a folder (use --track to play a file)").arg(dir)
                        : QStringLiteral("folder not found: %1").arg(dir));

    MainWindow window;
    window.applyCliOverrides(overrides);
    if (!dir.isEmpty())
        window.openDirectory(dir);

    window.show();

    window.startupPlay(parser.value(trackOption), parser.isSet(autoplayOption));
    return app.exec();
}
