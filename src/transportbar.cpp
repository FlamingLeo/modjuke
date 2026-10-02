#include "transportbar.h"

#include "library.h"
#include "widgets.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>

#include <cmath>

TransportBar::TransportBar(bool loopTrack, bool loopQueue, bool muted, int volume, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("barPanel"));
    // a QWidget subclass paints its stylesheet background only with this
    setAttribute(Qt::WA_StyledBackground);
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(10, 6, 10, 8);
    grid_->setHorizontalSpacing(14);

    auto *buttons = new QWidget(this);
    buttons->setObjectName(QStringLiteral("transportControls"));
    auto *bl = new QHBoxLayout(buttons);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(3);
    auto *previous = new QPushButton(QStringLiteral("◀◀"), buttons);
    play_ = new QPushButton(QStringLiteral("▶"), buttons);
    auto *next = new QPushButton(QStringLiteral("▶▶"), buttons);
    auto *stop = new QPushButton(QStringLiteral("■"), buttons);
    for (QPushButton *b : {previous, play_, next, stop}) {
        b->setObjectName(QStringLiteral("transportButton"));
        b->setMinimumWidth(44);
        bl->addWidget(b);
    }
    loop_ = new QCheckBox(tr("Loop"), buttons);
    loop_->setChecked(loopTrack);
    repeat_ = new QCheckBox(tr("Repeat queue"), buttons);
    repeat_->setToolTip(tr("Repeat queue, draw new order in shuffle (R)"));
    repeat_->setChecked(loopQueue);
    bl->addSpacing(8);
    bl->addWidget(loop_);
    bl->addWidget(repeat_);

    auto *volumeGroup = new QWidget(this);
    volumeGroup->setObjectName(QStringLiteral("volumeGroup"));
    auto *vl = new QHBoxLayout(volumeGroup);
    vl->setContentsMargins(0, 0, 0, 0);
    mute_ = new StableLabelButton({tr("Mute"), tr("Unmute")}, volumeGroup);
    mute_->setText(muted ? tr("Unmute") : tr("Mute"));
    mute_->setObjectName(QStringLiteral("muteButton"));
    mute_->setCheckable(true);
    mute_->setChecked(muted);
    volume_ = new JumpSlider(Qt::Horizontal, volumeGroup);
    volume_->setRange(0, 100);
    volume_->setPageStep(5);
    volume_->setValue(std::clamp(volume, 0, 100));
    volume_->setFixedWidth(120);
    volumeLabel_ = new QLabel(QStringLiteral("%1%").arg(volume), volumeGroup);
    volumeLabel_->setObjectName(QStringLiteral("dimLabel"));
    volumeLabel_->setMinimumWidth(38);
    vl->addWidget(mute_);
    vl->addWidget(volume_);
    vl->addWidget(volumeLabel_);

    position_ = new QWidget(this);
    position_->setObjectName(QStringLiteral("posGroup"));
    auto *pl = new QHBoxLayout(position_);
    pl->setContentsMargins(0, 0, 0, 0);
    time_ = new QLabel(QStringLiteral("0:00"), position_);
    time_->setObjectName(QStringLiteral("timeLabel"));
    duration_ = new QLabel(QStringLiteral("0:00"), position_);
    duration_->setObjectName(QStringLiteral("timeLabel"));
    queuePosition_ = new QLabel(QStringLiteral("0/0"), position_);
    queuePosition_->setObjectName(QStringLiteral("dimLabel"));
    seek_ = new JumpSlider(Qt::Horizontal, position_);
    seek_->setRange(0, 1000);
    seek_->setMinimumWidth(90);
    pl->addWidget(time_);
    pl->addWidget(seek_, 1);
    pl->addWidget(duration_);
    pl->addWidget(queuePosition_);

    grid_->addWidget(buttons, 0, 0);
    grid_->addWidget(volumeGroup, 0, 1);
    grid_->addWidget(position_, 0, 2);
    grid_->setColumnStretch(2, 1);

    connect(previous, &QPushButton::clicked, this, &TransportBar::previousClicked);
    connect(play_, &QPushButton::clicked, this, &TransportBar::playClicked);
    connect(next, &QPushButton::clicked, this, &TransportBar::nextClicked);
    connect(stop, &QPushButton::clicked, this, &TransportBar::stopClicked);
    connect(loop_, &QCheckBox::toggled, this, &TransportBar::loopToggled);
    connect(repeat_, &QCheckBox::toggled, this, &TransportBar::repeatToggled);
    connect(mute_, &QPushButton::toggled, this, [this](bool on) {
        mute_->setText(on ? tr("Unmute") : tr("Mute"));
        emit muteToggled(on);
    });
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        volumeLabel_->setText(QStringLiteral("%1%").arg(value));
        emit volumeChanged(value);
    });
    connect(volume_, &QSlider::sliderReleased, this, &TransportBar::volumeCommitted);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] { seeking_ = false; });
    connect(seek_, &QSlider::sliderMoved, this, [this](int value) {
        if (songDuration_ > 0.0)
            showTime(songDuration_ * value / 1000.0);
    });
    connect(seek_, &JumpSlider::positionRequested, this, [this](int value) {
        emit seekRequested(value / 1000.0);
        if (songDuration_ > 0.0)
            showTime(songDuration_ * value / 1000.0);
    });
    seek_->installEventFilter(this);   // right click -> jump to start
}

void TransportBar::refresh(const EngineSnapshot &snap, int queueIndex, int queueSize)
{
    songDuration_ = snap.durationValid ? snap.duration : 0.0;
    play_->setText(snap.playing && !snap.paused ? QStringLiteral("▮▮") : QStringLiteral("▶"));
    duration_->setText(snap.durationValid ? formatTime(snap.duration)
                                          : (snap.loaded ? QStringLiteral("∞") : QStringLiteral("0:00")));
    if (!seeking_) {
        double position = snap.seekPending ? snap.seekTarget : snap.position;
        if (snap.loop && !snap.seekPending && std::isfinite(snap.duration) && snap.duration > 0.0)
            position = std::fmod(position, snap.duration);
        const QSignalBlocker blocker(seek_);
        seek_->setValue(snap.durationValid ? int(position / snap.duration * 1000.0) : 0);
        showTime(position);
    }
    queuePosition_->setText(QStringLiteral("%1/%2").arg(queueIndex >= 0 ? queueIndex + 1 : 0).arg(queueSize));
    loop_->setToolTip(snap.playAllSubsongs
        ? tr("Repeat the whole subsong sequence; Next and Previous still change files.")
        : tr("Repeat the selected subsong."));
    seek_->setToolTip(tr("Position within the current subsong (%1 of %2)")
        .arg(snap.subsong + 1).arg(snap.numSubsongs));
}

void TransportBar::setCompact(bool compact)
{
    if (compact == compact_)
        return;
    compact_ = compact;
    grid_->removeWidget(position_);
    if (compact)
        grid_->addWidget(position_, 1, 0, 1, 3);
    else
        grid_->addWidget(position_, 0, 2);
}

void TransportBar::applyPalette(const Palette *palette)
{
    volume_->applyPalette(palette);
    seek_->applyPalette(palette);
}

void TransportBar::toggleLoop() { loop_->toggle(); }
void TransportBar::toggleRepeat() { repeat_->toggle(); }
void TransportBar::toggleMute() { mute_->toggle(); }
void TransportBar::setVolume(int percent) { volume_->setValue(percent); }
int TransportBar::volume() const { return volume_->value(); }

bool TransportBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == seek_ && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::RightButton) {
        emit seekRequested(0.0);
        seek_->setValue(0);
        showTime(0.0);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TransportBar::showTime(double seconds)
{
    time_->setText(formatTime(seconds));
}
