#include "trackerpage.h"

#include "widgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

TrackerPage::TrackerPage(Engine &engine, QWidget *parent) : QWidget(parent), engine_(engine)
{
    // a QWidget subclass paints its stylesheet background only with this
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 4, 10, 6);
    auto *header = new QHBoxLayout;
    header_ = new QLabel(tr("no module"), this);
    header_->setObjectName(QStringLiteral("headLabel"));
    following_ = new StableLabelButton({tr("Follow"), tr("Following")}, this);
    following_->setText(tr("Following"));
    following_->setCheckable(true);
    following_->setChecked(true);
    following_->setObjectName(QStringLiteral("miniButton"));
    auto *previousPattern = new QPushButton(QStringLiteral("◀"), this);
    auto *nextPattern = new QPushButton(QStringLiteral("▶"), this);
    for (QPushButton *b : {previousPattern, nextPattern})
        b->setObjectName(QStringLiteral("miniButton"));
    header->addWidget(header_, 1);
    header->addWidget(previousPattern);
    header->addWidget(nextPattern);
    header->addWidget(following_);
    layout->addLayout(header);

    view_ = new TrackerView(this);
    layout->addWidget(view_, 1);

    connect(following_, &QPushButton::toggled, view_, &TrackerView::setFollowing);
    connect(view_, &TrackerView::followingChanged, this, [this](bool following) {
        const QSignalBlocker blocker(following_);
        following_->setChecked(following);
        following_->setText(following ? tr("Following") : tr("Follow"));
    });
    connect(previousPattern, &QPushButton::clicked, this,
            [this] { view_->jumpPattern(TrackerView::PrevPattern); });
    connect(nextPattern, &QPushButton::clicked, this,
            [this] { view_->jumpPattern(TrackerView::NextPattern); });
    connect(view_, &TrackerView::rowClicked, this, [this](int order, int row) {
        engine_.seekOrderRow(order, row);
        engine_.play();
    });

    connect(&engine_, &Engine::songDataReady, this, [this](const SongDataReply &reply) {
        if (reply.token != token_ || reply.orders.isEmpty())
            return;
        view_->setFamily(engine_.snapshot().info.format.toLower());
        view_->setSong(reply.path, reply.orders, reply.rows, reply.channels, reply.numPatterns);
        const EngineSnapshot snap = engine_.snapshot();
        view_->setPlaying(snap.order, snap.row, snap.playing, snap.paused);
    });
    connect(view_, &TrackerView::patternNeeded, this, [this](int pattern) {
        engine_.requestPattern(token_, pattern, engine_.snapshot().channels);
    });
    connect(&engine_, &Engine::patternDataReady, this, [this](const PatternDataReply &reply) {
        if (reply.token == token_)
            view_->setPatternData(reply);
    });
}

void TrackerPage::sync(const EngineSnapshot &snap)
{
    if (snap.path.isEmpty()) {
        view_->clearSong();
        songKey_.clear();
        return;
    }
    const QString key = snap.path + QLatin1Char('|') + QString::number(snap.subsong)
                        + QLatin1Char('|') + QString::number(snap.songGeneration);
    if (!snap.loaded || snap.loading || key == songKey_)
        return;
    songKey_ = key;
    ++token_;
    engine_.requestSongData(token_);
}

void TrackerPage::refresh(const EngineSnapshot &snap)
{
    sync(snap);
    if (!snap.loaded || snap.loading) {
        header_->setText(snap.loading ? tr("Loading tracker…") : tr("No module loaded"));
        return;
    }
    const bool running = (snap.playing || snap.ended) && !snap.paused && !snap.failed;
    view_->setPlaying(snap.order, snap.row, running, snap.paused);
    header_->setText(tr("Order %1/%2, pattern %3, row %4, %5 ch")
                         .arg(snap.order + 1)
                         .arg(std::max(1, snap.numOrders))
                         .arg(snap.pattern)
                         .arg(snap.row)
                         .arg(snap.channels)
        + (snap.numSubsongs > 1 ? tr(", subsong %1/%2%3").arg(snap.subsong + 1).arg(snap.numSubsongs)
                                      .arg(snap.playAllSubsongs ? tr(" (all)") : QString())
                                : QString()));
}

void TrackerPage::songChanged()
{
    // the next sync() asks for the new song's data once it has loaded
    ++token_;
    songKey_.clear();
    view_->clearSong();
}

void TrackerPage::clear()
{
    view_->clearSong();
}

void TrackerPage::setSmoothScrolling(bool smooth)
{
    view_->setSmoothScrolling(smooth);
}

void TrackerPage::applyPalette(const Palette *palette)
{
    view_->applyPalette(palette);
}
