#pragma once
// The right-hand panel of the player tab: what is playing, its module data,
// the channel meters, the subsong choice and the log. It only shows engine
// snapshots; what its controls do is up to the window (signals).
#include "engine.h"
#include "theme.h"

#include <QWidget>

#include <array>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class VuMeter;

class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(bool playAllSubsongs, QWidget *parent = nullptr);

    // what the snapshot doesn't know
    struct Context {
        bool queueEmpty = true;
        bool ignored = false;       // the playing song is on the ignore list
        bool favorite = false;
        QString output;             // output description
    };
    void refresh(const EngineSnapshot &snap, const Context &context);   // every UI tick
    void setFavorite(bool on);      // right after a toggle, before the next tick
    void appendLog(const QString &level, const QString &text);
    void applyPalette(const Palette *palette);

signals:
    void favoriteClicked();
    void ignoreClicked();
    void songInfoClicked();
    void revealClicked();
    void subsongChosen(int index);           // 0-based; Play all subsongs was turned off first
    void playAllSubsongsToggled(bool on);

private:
    enum Row { Format, Tracker, Artist, Module, Length, Position, Sequencer, Resampling, Output, RowCount };

    const Palette *palette_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *subtitle_ = nullptr;
    QPushButton *favorite_ = nullptr;
    QPushButton *ignore_ = nullptr;
    QPushButton *songInfo_ = nullptr;
    QPushButton *reveal_ = nullptr;
    std::array<QLabel *, RowCount> rows_{};
    VuMeter *vu_ = nullptr;
    QSpinBox *subsong_ = nullptr;
    QLabel *subsongCount_ = nullptr;
    QLabel *subsongName_ = nullptr;
    QCheckBox *allSubsongs_ = nullptr;
    QLabel *loop_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
};
