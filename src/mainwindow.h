// The player window: toolbar, source/order/filter bar, queue table, song info
// panel, tracker tab, transport, status bar. Port of modjuke/ui.py PlayerApp.
#pragma once

#include "analyzer.h"
#include "config.h"
#include "dialogs.h"
#include "engine.h"
#include "queue.h"
#include "shuffles.h"
#include "theme.h"
#include "widgets.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QVector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class TrackerView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // ---- CLI entry points (main.cpp) ----
    void openDirectory(const QString &path);
    void startupPlay(const QString &track, bool autoplay);
    void applyCliOverrides(const QHash<QString, QString> &overrides);   // volume/theme/speed/...
    Engine &engine() { return engine_; }
    const Settings &settings() const { return settings_; }

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void rescan();
    void analyzeNow();
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
    void stopPlayback();
    void toggleMute();
    void addCurrentFavorite();
    void ignoreCurrent();
    void ignorePath(const QString &path);
    void revealCurrent();
    void addToPlaylistMenu();
    void removeFromPlaylist();
    void resetLayout();
    void tick();
    void onFinished();
    void restartQueue();
    void showTrackerTab(bool show);

private:
    struct CliState { bool autoplay = false; QString track; };

    // ---- setup ----
    void buildToolbar();
    void buildQueueBar();
    void buildPages();
    void buildPlayerPage();
    void buildInfoPanel(class QWidget *parent);
    void buildTrackerPage();
    void buildTransport();
    void buildStatusBar();
    void bindShortcuts();
    void applyTheme();
    void refreshSourceCombo();
    void refreshQueueControls();

    // ---- behavior ----
    void playPath(const QString &path, double position = 0.0, bool paused = false, int subsong = 0,
                  bool preserveBufferedTail = false);
    void restartQueueImpl(bool preserveBufferedTail);
    void playSelected();
    bool confirmIgnorePath(const QString &path);
    bool changeIgnored(const QStringList &add, const QStringList &remove = {});
    QString currentPath() const;                       // playing (or empty)
    QString selectedPath() const;                      // queue caret/selection
    QStringList queueSelectedPaths() const;
    void syncTrackerSong();
    void revealPlayingInQueue();
    QStringList queueViewContext_;
    void flushStats();
    bool typing() const;

    int queueIndexOf(const QString &path) const;
    void status(const QString &text);
    void appendLog(const QString &level, const QString &text);
    void updateInfoPanel(const EngineSnapshot &snap);
    void updateTransport(const EngineSnapshot &snap);
    void updateWindowTitle(const EngineSnapshot &snap);
    void saveSession(bool force = false);
    void commitSettings();
    void applyOrderEdited(const QStringList &newOrder);
    void startShotDriver();
    bool snapShotForShots(ModuleInfo &outInfo);
    void moveSelectedRow(int delta);
    QueueFilter filterFromSettings() const;
    void syncFilterFromSettings();
    void applyQueueFilters();

    // ---- shuffle plans (shuffles.py port) ----
    QVector<Track> sourceTracks() const;                    // current collection, unfiltered
    QString shuffleSourceKey() const;
    void ensureShufflePlan(const QVector<Track> &source);   // restore; draw only if never drawn
    void drawShufflePlan(quint32 newSeed = 0, const QString &firstPath = QString(),
                         const QString &avoidFirst = QString());
    void migrateShuffleOrder();                             // settings.shuffle_paths -> store
    void onPlaylistRenamed(const QString &oldName, const QString &newName);
    void onPlaylistRemoved(const QString &name);

    Settings settings_;
    Engine engine_;
    ShuffleStore shuffles_;
    QString shuffleKey_;                       // source shufflePaths_ belongs to
    QStringList shufflePaths_;                 // drawn order for that source
    Analyzer analyzer_;
    PlaylistStore playlists_;
    IgnoreStore ignored_;
    StatsStore stats_;
    Palette palette_;

    QVector<Track> libraryTracks_;                     // scanned library (all formats)
    QVector<Track> orderedSource_; // invalidated by every full rebuild, reused for search-only edits
    QVector<Track> queueTracks_;                       // currently visible queue
    QString libraryRoot_;
    int libraryDirs_ = 0;
    int analyzedTotal_ = 0;
    bool analysisRestartPending_ = false;
    QHash<QString, Track> trackByPath_;

    QWidget *toolbarRow_ = nullptr;
    QWidget *queueBar_ = nullptr;

    // toolbar
    QPushButton *openButton_ = nullptr;
    QPushButton *rescanButton_ = nullptr;
    QPushButton *analyzeButton_ = nullptr;
    QLineEdit *dirField_ = nullptr;
    QLabel *countLabel_ = nullptr;

    // queue bar
    QComboBox *sourceCombo_ = nullptr;
    QComboBox *orderCombo_ = nullptr;
    QPushButton *shuffleBtn_ = nullptr;
    QPushButton *playlistsBtn_ = nullptr;
    QPushButton *filterBtn_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QLabel *filterLabel_ = nullptr;

    // pages
    QWidget *playerPage_ = nullptr;
    QWidget *trackerPage_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QTabBar *tabBar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QueueTableView *queueView_ = nullptr;
    QueueModel *queueModel_ = nullptr;
    QPushButton *addToPlaylistBtn_ = nullptr;
    QPushButton *removeFromPlaylistBtn_ = nullptr;

    // info panel
    QLabel *titleLabel_ = nullptr;
    QLabel *subtitleLabel_ = nullptr;
    QPushButton *favoriteButton_ = nullptr;
    QPushButton *ignoreButton_ = nullptr;
    QPushButton *songInfoButton_ = nullptr;
    QPushButton *revealButton_ = nullptr;
    QHash<QString, QLabel *> infoLabels_;
    VuMeter *vu_ = nullptr;
    QSpinBox *subsongSpin_ = nullptr;
    QLabel *subsongCountLabel_ = nullptr;
    QLabel *subsongNameLabel_ = nullptr;
    QCheckBox *allSubsongsCheck_ = nullptr;
    QLabel *loopLabel_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;

    // tracker page
    QLabel *trackerHeader_ = nullptr;
    QPushButton *followingButton_ = nullptr;
    TrackerView *tracker_ = nullptr;
    bool trackerSeen_ = false;
    int songToken_ = 0;
    QString songTokenKey_;

    // transport
    QWidget *transportBar_ = nullptr;
    QPushButton *prevButton_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QPushButton *nextButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QCheckBox *loopCheck_ = nullptr;
    QCheckBox *repeatCheck_ = nullptr;
    QPushButton *muteButton_ = nullptr;
    QSlider *volumeSlider_ = nullptr;
    QLabel *volumeLabel_ = nullptr;
    JumpSlider *seekSlider_ = nullptr;
    QLabel *timeLabel_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    QLabel *queuePosLabel_ = nullptr;
    bool transportCompact_ = false;

    // status bar
    QLabel *statusLabel_ = nullptr;
    QLabel *healthLabel_ = nullptr;
    QPushButton *resetButton_ = nullptr;

    QTimer *uiTimer_ = nullptr;
    QElapsedTimer sessionTimer_;
    QElapsedTimer statsTimer_;
    double statsAccum_ = 0.0;
    bool statsDirty_ = false;
    QString statsPath_;
    QPointer<SettingsDialog> settingsDialog_;
    bool seeking_ = false;
    bool closing_ = false;
    CliState cli_;
};
