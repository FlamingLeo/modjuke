#pragma once
// The transport row: play controls, Loop / Repeat queue, mute and volume,
// the seek slider with the times and the queue position. It shows engine
// snapshots; what its controls do is up to the window (signals).
#include "engine.h"
#include "theme.h"

#include <QWidget>

class JumpSlider;
class QCheckBox;
class QGridLayout;
class QLabel;
class QPushButton;
class StableLabelButton;

class TransportBar : public QWidget {
    Q_OBJECT
public:
    TransportBar(bool loopTrack, bool loopQueue, bool muted, int volume, QWidget *parent = nullptr);

    void refresh(const EngineSnapshot &snap, int queueIndex, int queueSize);   // every UI tick
    void setCompact(bool compact);   // the position group on its own row
    void applyPalette(const Palette *palette);

    // keyboard shortcuts and the command line: like using the controls
    void toggleLoop();
    void toggleRepeat();
    void toggleMute();
    void setVolume(int percent);
    void stepVolume(int delta) { setVolume(volume() + delta); }
    int volume() const;

signals:
    void previousClicked();
    void playClicked();
    void nextClicked();
    void stopClicked();
    void loopToggled(bool on);
    void repeatToggled(bool on);
    void muteToggled(bool on);
    void volumeChanged(int percent);
    void volumeCommitted();             // the slider was let go
    void seekRequested(double fraction);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void showTime(double seconds);

    QGridLayout *grid_ = nullptr;
    QWidget *position_ = nullptr;       // times, seek slider, queue position
    QPushButton *play_ = nullptr;
    QCheckBox *loop_ = nullptr;
    QCheckBox *repeat_ = nullptr;
    StableLabelButton *mute_ = nullptr;
    JumpSlider *volume_ = nullptr;
    QLabel *volumeLabel_ = nullptr;
    JumpSlider *seek_ = nullptr;
    QLabel *time_ = nullptr;
    QLabel *duration_ = nullptr;
    QLabel *queuePosition_ = nullptr;
    bool seeking_ = false;              // the seek slider is held: don't move it
    bool compact_ = false;
    double songDuration_ = 0.0;         // of the last snapshot, 0 when unknown
};
