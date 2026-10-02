#include "infopanel.h"

#include "library.h"
#include "widgets.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QLabel *dimLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("dimLabel"));
    return label;
}

QString interpolationText(int length)
{
    switch (length) {
    case 1: return InfoPanel::tr("No interpolation");
    case 2: return InfoPanel::tr("Linear (2-tap)");
    case 4: return InfoPanel::tr("Cubic (4-tap)");
    default: return InfoPanel::tr("Sinc (8-tap)");
    }
}

}   // namespace

InfoPanel::InfoPanel(bool playAllSubsongs, QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("infoPanel"));
    // a QWidget subclass paints its stylesheet background only with this
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 10);

    title_ = new QLabel(tr("Nothing playing"), this);
    title_->setObjectName(QStringLiteral("bigTitle"));
    title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true);
    layout->addWidget(title_);
    subtitle_ = dimLabel(tr("Double-click a module to play it"), this);
    subtitle_->setTextFormat(Qt::PlainText);
    subtitle_->setWordWrap(true);
    layout->addWidget(subtitle_);

    auto *actions = new QHBoxLayout;
    favorite_ = new QPushButton(QStringLiteral("☆"), this);   // ☆
    favorite_->setCheckable(true);
    favorite_->setToolTip(tr("Toggle favorite"));
    ignore_ = new QPushButton(tr("Ignore"), this);
    songInfo_ = new QPushButton(tr("Song info"), this);
    reveal_ = new QPushButton(tr("Show in folder"), this);
    for (QPushButton *b : {favorite_, ignore_, songInfo_, reveal_}) {
        b->setEnabled(false);
        actions->addWidget(b);
    }
    actions->addStretch();
    layout->addLayout(actions);
    connect(favorite_, &QPushButton::clicked, this, &InfoPanel::favoriteClicked);
    connect(ignore_, &QPushButton::clicked, this, &InfoPanel::ignoreClicked);
    connect(songInfo_, &QPushButton::clicked, this, &InfoPanel::songInfoClicked);
    connect(reveal_, &QPushButton::clicked, this, &InfoPanel::revealClicked);

    auto *grid = new QGridLayout;
    grid->setVerticalSpacing(1);
    const QString names[RowCount] = {tr("Format"), tr("Tracker"), tr("Artist"), tr("Module"), tr("Length"),
                                      tr("Position"), tr("Sequencer"), tr("Resampling"), tr("Output")};
    for (int r = 0; r < RowCount; ++r) {
        grid->addWidget(dimLabel(names[r], this), r, 0, Qt::AlignLeft);
        rows_[r] = new QLabel(QStringLiteral("-"), this);
        rows_[r]->setTextFormat(Qt::PlainText);
        rows_[r]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long artist is cut, not widening the panel
        grid->addWidget(rows_[r], r, 1, Qt::AlignLeft);
    }
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    layout->addWidget(dimLabel(tr("Channels"), this));
    vu_ = new VuMeter(this);
    layout->addWidget(vu_);

    auto *subs = new QHBoxLayout;
    subsong_ = new QSpinBox(this);
    subsong_->setObjectName(QStringLiteral("subsongSpin"));
    subsong_->setToolTip(tr("Choose a subsong (1 is the first). This restarts it and turns off Play all subsongs; pause is preserved."));
    subsong_->setRange(1, 1);
    subsong_->setKeyboardTracking(false);
    subsong_->setEnabled(false);
    subsongCount_ = dimLabel(tr("(single song)"), this);
    loop_ = dimLabel(QString(), this);
    subs->addWidget(dimLabel(tr("Subsong"), this));
    subs->addWidget(subsong_);
    subs->addWidget(subsongCount_);
    allSubsongs_ = new QCheckBox(tr("Play all subsongs"), this);
    allSubsongs_->setObjectName(QStringLiteral("allSubsongsCheck"));
    allSubsongs_->setChecked(playAllSubsongs);
    allSubsongs_->setToolTip(tr("Play every subsong in order, then advance to the next file if auto-advance is enabled. Enabling starts at the first subsong, preserving pause. Loop repeats the whole sequence. Time and seeking refer to the current subsong."));
    subs->addWidget(allSubsongs_);
    subs->addStretch();
    subs->addWidget(loop_);
    layout->addLayout(subs);
    subsongName_ = new QLabel(this);
    subsongName_->setTextFormat(Qt::PlainText);
    subsongName_->setWordWrap(true);
    subsongName_->hide();
    layout->addWidget(subsongName_);
    connect(allSubsongs_, &QCheckBox::toggled, this, &InfoPanel::playAllSubsongsToggled);
    connect(subsong_, &QSpinBox::valueChanged, this, [this](int value) {
        allSubsongs_->setChecked(false);
        emit subsongChosen(value - 1);
    });

    layout->addWidget(dimLabel(tr("Log"), this));
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(200);
    log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(log_, 1);
}

void InfoPanel::refresh(const EngineSnapshot &snap, const Context &context)
{
    const bool has = !snap.path.isEmpty();
    if (has) {
        title_->setText(snap.info.title.isEmpty() ? QFileInfo(snap.path).fileName() : snap.info.title);
        subtitle_->setText(snap.path);
    } else {
        title_->setText(tr("Nothing playing"));
        subtitle_->setText(context.queueEmpty ? tr("Open a folder to build the queue")
                                              : tr("Double-click a module to play it"));
    }
    favorite_->setEnabled(has);
    ignore_->setEnabled((snap.loaded || snap.loading) && has && !context.ignored);
    reveal_->setEnabled(has);
    songInfo_->setEnabled(snap.loaded);
    setFavorite(has && context.favorite);

    const ModuleInfo &meta = snap.info;
    const QString none = QStringLiteral("-");
    rows_[Format]->setText(snap.loaded ? (meta.formatLong.isEmpty() ? meta.format.toUpper() : meta.formatLong)
                                       : none);
    rows_[Tracker]->setText(meta.tracker.isEmpty() ? none : meta.tracker);
    rows_[Artist]->setText(meta.artist.isEmpty() ? none : meta.artist);
    QString moduleLine = none;
    if (snap.loaded) {
        moduleLine = tr("%1 ch, %2 ord, %3 pat, %4 smp")
                         .arg(meta.channels).arg(meta.orders).arg(meta.patterns).arg(meta.samples);
        if (meta.instruments > 0)
            moduleLine += tr(", %1 inst").arg(meta.instruments);
    }
    rows_[Module]->setText(moduleLine);
    rows_[Length]->setText(snap.durationValid ? formatTime(snap.duration)
                                              : (snap.loaded ? QStringLiteral("∞") : none));
    rows_[Position]->setText(snap.loaded ? tr("order %1, pattern %2, row %3")
                                               .arg(snap.order + 1).arg(snap.pattern).arg(snap.row)   // patterns 0-based, as in the tracker header
                                         : none);
    rows_[Sequencer]->setText(snap.loaded ? tr("speed %1, tempo %2").arg(snap.speed).arg(snap.tempo) : none);
    rows_[Resampling]->setText(interpolationText(snap.interpolation));
    rows_[Output]->setText(context.output);

    const QSignalBlocker spinBlocker(subsong_);
    const int subsongs = std::max(0, int(meta.subsongs));
    subsong_->setEnabled(subsongs > 1 && snap.loaded);
    // Leave the box alone while the user is typing in it, and only touch it
    // on a change: every tick's setValue reset the edit text.
    if (!subsong_->hasFocus()) {
        if (subsong_->maximum() != std::max(1, subsongs))
            subsong_->setRange(1, std::max(1, subsongs));
        if (subsong_->value() != snap.subsong + 1)
            subsong_->setValue(snap.subsong + 1);
    }
    subsongCount_->setText(subsongs > 1 ? tr("of %1").arg(subsongs) : tr("of 1"));
    const QString name = snap.loaded ? meta.subsongNames.value(snap.subsong).trimmed() : QString();
    subsongName_->setText(name);
    subsongName_->setVisible(!name.isEmpty());
    loop_->setText(snap.loop ? (snap.playAllSubsongs ? tr("loop all") : tr("looping")) : QString());

    // channels = the module's pattern channels; playingChannels counts mixer
    // voices (including virtual ones): never size the bars by that
    vu_->setChannels(snap.channels);
    vu_->updateValues(snap.vu, snap.levelL, snap.levelR);
}

void InfoPanel::setFavorite(bool on)
{
    const QSignalBlocker blocker(favorite_);
    favorite_->setText(on ? QStringLiteral("★") : QStringLiteral("☆"));
    favorite_->setChecked(on);
}

void InfoPanel::appendLog(const QString &level, const QString &text)
{
    Palette::Role role = Palette::FG_DIM;
    if (level == QLatin1String("warn"))
        role = Palette::AMBER;
    else if (level == QLatin1String("error"))
        role = Palette::RED;
    else if (level == QLatin1String("debug"))
        role = Palette::FG_FAINT;
    const QString color = palette_ ? palette_->name(role) : Palette::builtin(QStringLiteral("dark")).name(role);
    log_->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>")
                         .arg(color, level == QLatin1String("info")
                                         ? text.toHtmlEscaped()
                                         : QStringLiteral("[%1] %2").arg(level, text.toHtmlEscaped())));
}

void InfoPanel::applyPalette(const Palette *palette)
{
    palette_ = palette;
    vu_->applyPalette(palette);
}
