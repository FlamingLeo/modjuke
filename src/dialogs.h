// The dialogs around the player: filter, playlists, settings, song info,
// stats, ignored files, and about. (Ports of filters.py/playlists.py/
// ui.py dialogs, songinfo.py, statsview.py, ignoreview.py.)
#pragma once

#include "config.h"
#include "library.h"
#include "openmptapi.h"
#include "stores.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <functional>

// ---- queue filter (FiltersDialog) ----
class FilterDialog : public QDialog {
    Q_OBJECT
public:
    FilterDialog(QWidget *parent, const QueueFilter &current, const QVector<Track> &tracks);
    QueueFilter criteria() const;

private slots:
    void clear();

private:
    void updatePreview();
    QVector<Track> tracks_;
    QList<QCheckBox *> formatChecks_;
    QLineEdit *minEdit_ = nullptr;
    QLineEdit *maxEdit_ = nullptr;
    QCheckBox *hideBroken_ = nullptr;
    QLabel *countLabel_ = nullptr;
    QPushButton *applyBtn_ = nullptr;
    QueueFilter base_;
};

// ---- playlist manager ----
class PlaylistsDialog : public QDialog {
    Q_OBJECT
public:
    PlaylistsDialog(QWidget *parent, PlaylistStore *store,
                    std::function<QStringList()> queuePaths,
                    std::function<void(const QString &, bool savedOrder)> loadAsSource,
                    std::function<void(const QString &)> saveQueueAs,
                    std::function<void(const QString &, const QString &)> onRenamed = {},
                    std::function<void(const QString &)> onDeleted = {});

private:
    void refresh(const QString &selectName = QString());
    QString selectedName() const;
    void createEmpty();
    void addSelectedSongs();
    void addFiles();
    void saveAs();
    void renameSelected();
    void deleteSelected();
    void loadSelected();
    void exportSelected();
    void importM3u();
    void updateButtons();

    PlaylistStore *store_;
    std::function<QStringList()> queuePaths_;
    std::function<void(const QString &, bool)> loadAsSource_;
    std::function<void(const QString &)> saveQueueAs_;
    std::function<void(const QString &, const QString &)> onRenamed_;
    std::function<void(const QString &)> onDeleted_;

    QTreeWidget *tree_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QPushButton *createBtn_ = nullptr;
    QPushButton *loadBtn_ = nullptr;
    QPushButton *renameBtn_ = nullptr;
    QPushButton *deleteBtn_ = nullptr;
    QPushButton *exportBtn_ = nullptr;
    QPushButton *addSelBtn_ = nullptr;
    QLabel *noteLabel_ = nullptr;
};

// ---- settings ----
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(QWidget *parent, const Settings &settings, IgnoreStore *ignored);
    // Copy the edited values back into `out` (applied by the caller).
    void collect(Settings &out) const;
    void setConfirmIgnore(bool confirm);
    void refreshIgnoredCount();
    void setSaveHandler(std::function<bool()> handler) { saveHandler_ = std::move(handler); }
public slots:
    void accept() override;
signals:
    void ignoredChanged();
private:

    QCheckBox *autoAnalyze_ = nullptr;
    QCheckBox *cacheAnalysis_ = nullptr;
    QCheckBox *rememberPos_ = nullptr;
    QCheckBox *trackStats_ = nullptr;
    QCheckBox *smoothTracker_ = nullptr;
    QCheckBox *skipIgnoreConfirmation_ = nullptr;
    void refreshThemes(const QString &selected);
    void editTheme(bool create);
    void deleteTheme();
    QJsonObject customThemes_;
    QPushButton *newTheme_, *editTheme_, *deleteTheme_;
    std::function<bool()> saveHandler_;
    QComboBox *themeCombo_ = nullptr;
    QComboBox *titleModeCombo_ = nullptr;
    QComboBox *backendCombo_ = nullptr;
    QComboBox *rateCombo_ = nullptr;
    QComboBox *interpCombo_ = nullptr;
    QSpinBox *bufferSpin_ = nullptr;
    QSpinBox *fpsSpin_ = nullptr;
    QLabel *ignoredCountLabel_ = nullptr;
    IgnoreStore *ignoreStore_ = nullptr;
    Settings base_;
};

// ---- song info (samples / instruments) ----
class SongInfoDialog : public QDialog {
    Q_OBJECT
public:
    SongInfoDialog(QWidget *parent, const ModuleInfo &info);
};

// ---- listening stats ----
class StatsDialog : public QDialog {
    Q_OBJECT
public:
    StatsDialog(QWidget *parent, StatsStore *store);

signals:
    void statsReset();

private:
    void refresh();
    QTreeWidget *tree_ = nullptr;
    QLabel *totalLabel_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    StatsStore *store_;
};

// ---- ignored files ----
class IgnoreDialog : public QDialog {
    Q_OBJECT
public:
    IgnoreDialog(QWidget *parent, IgnoreStore *store);

private:
    void refresh();
    void removeSelected();
    void clearAll();
    QTreeWidget *tree_ = nullptr;
    QLabel *countLabel_ = nullptr;
    IgnoreStore *store_;
};

// ---- about ----
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    AboutDialog(QWidget *parent, const QString &libopenmptVersion, bool libLoaded);
};
