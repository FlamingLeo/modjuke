// The player window: the tab strip, the queue side (QueuePanel), song info
// panel, tracker tab, transport, status bar, and what connects them to the
// library, the queue and the engine. Port of modjuke/ui.py PlayerApp.
#pragma once

#include "config.h"
#include "dialogs.h"
#include "engine.h"
#include "ignorestore.h"
#include "infopanel.h"
#include "trackerpage.h"
#include "transportbar.h"
#include "listeningstats.h"
#include "musiclibrary.h"
#include "playbackqueue.h"
#include "playliststore.h"
#include "queuebuilder.h"
#include "queuepanel.h"
#include "queueplayer.h"
#include "sessionoptions.h"
#include "settingskeeper.h"
#include "shuffles.h"
#include "songactions.h"
#include "theme.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

class ElidedLabel;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // ---- CLI entry points (main.cpp) ----
    void openDirectory(const QString &path);
    void startupPlay(const QString &track, bool autoplay);
    void applySessionOptions(SessionOptions options);   // the command line, this session only
    Engine &engine() { return engine_; }
    const Settings &settings() const { return settings_; }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void chooseFolder();
    void rescan();
    void analyzeNow();
    void scanFolder(const QString &root);
    void showTrackCount();
    void rebuildQueue();
    void reshuffle();
    void openFilterDialog();
    void openPlaylistsDialog();
    void openSettingsDialog();
    bool applySettingsFromDialog(class SettingsDialog *dialog);
    void openAboutDialog();
    void openStatsDialog();
    void openSongInfoDialog();
    void togglePlay();
    void playPrevious();
    void playNext();
    void addCurrentFavorite();
    void ignoreCurrent();
    void ignorePath(const QString &path);
    void revealCurrent();
    QStringList actionPaths() const;   // the selected songs, or else the playing one
    void resetLayout();
    void tick();
    void showTrackerTab(bool show);

private:

    // ---- setup ----
    void buildPages();
    void buildPlayerPage();
    void buildTransport();
    void buildStatusBar();
    void connectQueuePanel();
    void bindShortcuts();
    void applyTheme();
    void refreshSourceCombo();
    void refreshQueueControls();

    // ---- behavior ----
    bool confirmIgnorePath(const QString &path);
    bool changeIgnored(const QStringList &add, const QStringList &remove = {});
    QString currentPath() const;                       // playing (or empty)
    QString anchorPath() const;                        // playing, or else the queue's selection
    QStringList queueViewContext_;
    bool typing() const;

    void applySettings(const Settings *previous);   // settings_ -> engine and UI
    void status(const QString &text);
    void appendLog(const QString &level, const QString &text);
    void updateLoadStatus(const EngineSnapshot &snap);   // the status bar while loading
    void updateWindowTitle(const EngineSnapshot &snap);
    void saveSession(bool force = false);
    void commitSettings();
    void applyOrderEdited(const QStringList &newOrder);
#ifdef MODJUKE_SHOT_DRIVER
    void startShotDriver();   // shotdriver.cpp (CMake option MODJUKE_SHOT_DRIVER)
#endif
    void moveSelectedRow(int delta);
    void applyQueueFilters();

    Settings settings_;
    SettingsKeeper keeper_{settings_};  // saves settings_ (command-line values stay session-only)
    Engine engine_;
    ShuffleStore shuffles_;
    MusicLibrary library_;              // the scanned folder and its analysis
    PlaylistStore playlists_;
    IgnoreStore ignored_;
    ListeningStats stats_;
    Palette palette_;

    PlaybackQueue queue_;               // the visible queue and its unfiltered order
    QueueBuilder builder_{settings_, library_, playlists_, ignored_, shuffles_};   // its source in play order
    QueuePlayer player_{engine_, queue_, stats_, ignored_, settings_};   // what plays next
    SongActions songActions_{this, settings_, playlists_, ignored_, library_, builder_};
    bool statusShowsLoading_ = false;   // the status bar shows "Loading ..."

    QueuePanel *queuePanel_ = nullptr;  // folder row, queue controls, queue table

    // pages
    QWidget *playerPage_ = nullptr;
    TrackerPage *trackerPage_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QTabBar *tabBar_ = nullptr;
    QStackedWidget *pages_ = nullptr;

    InfoPanel *infoPanel_ = nullptr;
    TransportBar *transport_ = nullptr;

    // status bar
    ElidedLabel *statusLabel_ = nullptr;
    QLabel *healthLabel_ = nullptr;
    QPushButton *resetButton_ = nullptr;

    QTimer *uiTimer_ = nullptr;
    QElapsedTimer sessionTimer_;
    QPointer<SettingsDialog> settingsDialog_;
};
