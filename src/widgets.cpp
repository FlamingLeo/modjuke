#include "widgets.h"

#include "theme.h"

#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QStyleOptionSlider>
#include <QStyleOptionButton>
#include <QStyle>
#include <QContextMenuEvent>

#include <algorithm>
#include <cmath>

StableLabelButton::StableLabelButton(const QStringList &labels, QWidget *parent)
    : QPushButton(labels.value(0), parent), labels_(labels)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize StableLabelButton::sizeHint() const
{
    ensurePolished();
    QStyleOptionButton option;
    initStyleOption(&option);
    QSize result;
    for (const QString &label : labels_) {
        option.text = label;
        const QSize contents = fontMetrics().size(Qt::TextShowMnemonic, label);
        option.rect.setSize(contents);
        for (bool checked : {false, true}) {
            option.state.setFlag(QStyle::State_On, checked);
            option.state.setFlag(QStyle::State_Off, !checked);
            result = result.expandedTo(style()->sizeFromContents(
                QStyle::CT_PushButton, &option, contents, this));
        }
    }
    return result;
}

// QStyleSheetStyle can leave native sub-page backgrounds around a narrow
// groove on Qt/Fusion. Paint only the rail/fill/knob, preserving QSlider's
// styled geometry and all existing click, drag and keyboard semantics.
void JumpSlider::paintEvent(QPaintEvent *event)
{
    if (!palette_ || orientation() != Qt::Horizontal) {
        QSlider::paintEvent(event);
        return;
    }
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const qreal radius = handle.width() / 2.0;
    const qreal y = height() / 2.0;
    const qreal left = groove.left() + radius, right = groove.right() - radius + 1;
    const qreal current = handle.left() + radius;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen((*palette_)[isEnabled() ? Palette::SEEK_TRACK : Palette::SEEK_TRACK_OFF], 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(left, y), QPointF(right, y));
    p.setPen(QPen((*palette_)[isEnabled() ? Palette::ACCENT : Palette::SEEK_FILL_OFF], 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(option.upsideDown ? right : left, y), QPointF(current, y));
    p.setPen(Qt::NoPen);
    p.setBrush((*palette_)[!isEnabled() ? Palette::SEEK_FILL_OFF : (hasFocus() ? Palette::ACCENT : Palette::SEEK_KNOB)]);
    p.drawEllipse(QPointF(current, y), radius, radius);
}

int JumpSlider::valueAt(const QPoint &point) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const bool horizontal = orientation() == Qt::Horizontal;
    const int length = horizontal ? handle.width() : handle.height();
    const int start = horizontal ? groove.x() : groove.y();
    const int span = (horizontal ? groove.width() : groove.height()) - length;
    const int coordinate = horizontal ? point.x() : point.y();
    return QStyle::sliderValueFromPosition(minimum(), maximum(),
        coordinate - dragOffset_ - start - length/2, std::max(0, span), option.upsideDown);
}

void JumpSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(event);
        return;
    }
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const QPoint point = event->position().toPoint();
    // Retain the grab offset when dragging an existing handle.
    dragOffset_ = handle.contains(point)
        ? (orientation() == Qt::Horizontal ? point.x()-handle.x()-handle.width()/2
                                          : point.y()-handle.y()-handle.height()/2)
        : 0;
    setFocus(Qt::MouseFocusReason);
    setSliderDown(true);
    setSliderPosition(valueAt(point));
    pressValue_ = sliderPosition();
    emit positionRequested(pressValue_);
    event->accept();
}

void JumpSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (isSliderDown() && (event->buttons() & Qt::LeftButton)) {
        setSliderPosition(valueAt(event->position().toPoint()));
        event->accept();
    } else {
        QSlider::mouseMoveEvent(event);
    }
}

void JumpSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && isSliderDown()) {
        setSliderPosition(valueAt(event->position().toPoint()));
        // Commit before sliderReleased lets the UI follow engine telemetry again.
        if (sliderPosition() != pressValue_)
            emit positionRequested(sliderPosition());
        setSliderDown(false);
        event->accept();
    } else {
        QSlider::mouseReleaseEvent(event);
    }
}

void JumpSlider::contextMenuEvent(QContextMenuEvent *event)
{
    event->accept();
}

// ---------------------------------------------------------------------------
// VuMeter
// ---------------------------------------------------------------------------

VuMeter::VuMeter(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(kBottom + 4);
}

void VuMeter::applyPalette(const Palette *palette)
{
    palette_ = palette;
    update();
}

void VuMeter::setChannels(int count)
{
    count = std::clamp(count, 0, 64);
    if (channels_.size() == count)
        return;
    channels_.resize(count);
    updateGeometry();
    update();
}

void VuMeter::updateValues(const QVector<float> &vu, float levelL, float levelR)
{
    // Instant attack, 180ms exponential release. Ballistics are independent
    // of the UI frame rate; missing/silent readings decay instead of flashing.
    const double dt = meterClock_.isValid() ? meterClock_.nsecsElapsed() / 1e9 : 0.0;
    meterClock_.start();
    const float decay = float(std::exp(-dt / 0.180));
    auto envelope = [decay](float previous, float incoming) {
        const float target = std::isfinite(incoming) ? std::clamp(incoming, 0.f, 1.f) : 0.f;
        const float result = std::max(target, previous * decay);
        return result < 0.0001f ? 0.f : result;
    };
    for (int i = 0; i < channels_.size(); ++i)
        channels_[i].level = envelope(channels_[i].level, vu.value(i));
    masterL_ = envelope(masterL_, levelL);
    masterR_ = envelope(masterR_, levelR);
    update();
}

// Geometry follows the Tk canvas: two stereo rails at the top (y 6 and 17,
// 8 px high), a baseline line at y 37, channel bars hanging from y 39 down
// to y 70.  Channel level uses a 0.6 gamma, the rails 0.5.
void VuMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const Palette fallback = Palette::builtin(QStringLiteral("dark"));
    const Palette &pal = palette_ ? *palette_ : fallback;

    auto barColor = [&](float frac) {
        if (frac >= 0.92f)
            return pal[Palette::RED];
        if (frac >= 0.75f)
            return pal[Palette::AMBER];
        return pal[Palette::GREEN];
    };

    const int width = qMax(10, this->width());
    p.fillRect(rect(), pal[Palette::BG_INPUT]);

    // ---- stereo rails ----
    for (int row = 0; row < 2; ++row) {
        const float v = row == 0 ? masterL_ : masterR_;
        const double frac = std::pow(double(std::clamp(v, 0.f, 1.f)), 0.5);
        const int top = 6 + row * kRailStep;
        p.fillRect(QRect(4, top, width - 8, kRailH), pal[Palette::BG_ALT]);        // trough
        if (frac > 0.002) {
            const int w = qMax(1, int(frac * (width - 8)));
            p.fillRect(QRect(4, top, w, kRailH), barColor(float(frac)));
        }
    }

    // ---- channel bars ----
    const int count = channels_.size();
    if (count > 0) {
        p.setPen(pal[Palette::VU_BASELINE]);
        p.drawLine(4, kBaseline - 1, width - 4, kBaseline - 1);
        const int barW = std::clamp((width - 10) / count, 3, 11);
        const int xBase = std::max(5, (width - count * barW) / 2);
        for (int i = 0; i < count; ++i) {
            const double frac = std::pow(double(channels_[i].level), 0.6);
            const int x = xBase + i * barW;
            const QRect trough(x, kBaseline + 1, barW - kBarGap, kBottom - kBaseline - 1);
            // Always paint the entire trough before overlaying the level.
            p.fillRect(trough, pal[Palette::BG_ALT]);
            if (frac <= 0.001)
                continue;
            const int top = kBaseline + 1 + int((1.0 - frac) * (kBottom - kBaseline - 1));
            p.fillRect(QRect(x, top, barW - kBarGap, kBottom - top), barColor(float(frac)));
        }
    }
}

// ---------------------------------------------------------------------------
// TrackerView
// ---------------------------------------------------------------------------

TrackerView::TrackerView(QWidget *parent) : QAbstractScrollArea(parent)
{
    monoFont_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont_.setStyleHint(QFont::Monospace);
    monoFont_.setFixedPitch(true);
    monoFont_.setPointSizeF(9.5);
    const QFontMetrics metrics(monoFont_);
    lineHeight_ = metrics.height() + 1;
    rowNumberWidth_ = metrics.horizontalAdvance(QLatin1Char('M')) * 4 + 6;
    headerHeight_ = metrics.height() + 6;
    scrollClock_.start();
    animationTimer_.setTimerType(Qt::PreciseTimer);
    animationTimer_.setInterval(16);
    connect(&animationTimer_, &QTimer::timeout, this, [this] {
        if (!gliding_ || !isVisible()) { animationTimer_.stop(); return; }
        advanceScroll(scrollClock_.elapsed());
        updateScrollBars();
        requestVisiblePatterns();
        viewport()->update();
        if (!gliding_) animationTimer_.stop();
    });
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setFont(monoFont_);
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        setFollowing(false);
        topRow_ = value;
        requestVisiblePatterns();
        viewport()->update();
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}

void TrackerView::applyPalette(const Palette *palette)
{
    palette_ = palette;
    rowCache_.clear();
    viewport()->update();
}

void TrackerView::setFamily(const QString &family)
{
    if (family_ == family) return;
    family_ = family;
    rowCache_.clear();
    viewport()->update();
}

void TrackerView::setSong(const QString &path, const QVector<int> &orders,
                          const QHash<int, int> &rows, int channels, int numPatterns)
{
    path_ = path;
    orders_ = orders;
    rows_ = rows;
    channels_ = std::max(1, channels);
    numPatterns_ = numPatterns;
    patterns_.clear();
    rowCache_.clear();
    requestedPatterns_.clear();
    selectedPattern_ = -1;
    following_ = true;
    emit followingChanged(true);
    horizontalScrollBar()->setValue(0);
    rebuildLines();
    relayout();
    snapToPlaying();
    requestVisiblePatterns();
    viewport()->update();
}

void TrackerView::setPatternData(const PatternDataReply &reply)
{
    if (reply.error.isEmpty() && reply.pattern >= 0)
        patterns_.insert(reply.pattern, reply);
    rowCache_.clear();
    viewport()->update();
}

void TrackerView::clearSong()
{
    path_.clear(); orders_.clear(); rows_.clear(); patterns_.clear();
    rowCache_.clear(); animationTimer_.stop();
    requestedPatterns_.clear(); lines_.clear(); blockStart_.clear();
    channels_ = 0;
    currentOrder_ = currentRow_ = 0;
    playing_ = paused_ = false;
    topRow_ = 0.0;
    gliding_ = false; rowTime_ = lastTick_ = -1;
    selectedPattern_ = -1;
    updateScrollBars();
    viewport()->update();
}

void TrackerView::rebuildLines()
{
    lines_.clear(); blockStart_.clear();
    for (int i = 0; i < orders_.size(); ++i) {
        const int pattern = orders_[i];
        if (pattern < 0) continue;
        blockStart_.insert(i, lines_.size());
        const int rows = rows_.value(pattern, 0);
        if (rows <= 0) lines_.append(Line{i, 0, true});
        else for (int row = 0; row < rows; ++row) lines_.append(Line{i, row, false});
    }
}

int TrackerView::playingLine() const
{
    const auto it = blockStart_.constFind(currentOrder_);
    if (it == blockStart_.constEnd()) return -1;
    const int rows = rows_.value(orders_.value(currentOrder_, -1), 0);
    return it.value() + (rows > 0 ? std::clamp(currentRow_, 0, rows - 1) : 0);
}

double TrackerView::centeredTop(int line) const
{
    // Padding at the song's edges keeps even its first/last row centered.
    const double bodyRows = std::max(1.0, double(viewport()->height() - headerHeight_) / lineHeight_);
    return line - (bodyRows - 1.0) / 2.0;
}

void TrackerView::advanceScroll(qint64 now)
{
    if (!gliding_) return;
    const double fraction = std::clamp((now - glideStarted_) / glideDuration_, 0.0, 1.0);
    topRow_ = glideFrom_ + (glideTarget_ - glideFrom_) * fraction;
    if (fraction >= 1.0) gliding_ = false;
}

void TrackerView::snapToPlaying()
{
    gliding_ = false;
    animationTimer_.stop();
    rowTime_ = -1;
    rowInterval_ = 120.0;
    if (playingLine() >= 0) topRow_ = centeredTop(playingLine());
    updateScrollBars();
}

void TrackerView::setPlaying(int order, int row, bool playing, bool paused)
{
    const qint64 now = scrollClock_.elapsed();
    const double oldTop = topRow_;
    const int previous = playingLine();
    const bool wasRunning = playing_ && !paused_;
    const bool moved = order != currentOrder_ || row != currentRow_;
    const bool timely = lastTick_ >= 0 && now - lastTick_ <= 500;
    if (moved) advanceScroll(now);
    currentOrder_ = order; currentRow_ = row;
    playing_ = playing; paused_ = paused;
    const int line = playingLine();
    if (following_ && line >= 0) {
        if (moved) {
            const double target = centeredTop(line);
            const bool adjacent = previous >= 0 && line > previous && line - previous <= 4;
            if (smoothScroll_ && playing && !paused && wasRunning && timely
                && adjacent && rowTime_ >= 0 && visibleRows() >= 4 && isVisible()) {
                glideFrom_ = topRow_; glideTarget_ = target; glideStarted_ = now;
                const double interval = std::clamp(double(now-rowTime_), 25.0, 500.0);
                // Smooth telemetry quantization rather than alternating glide
                // speeds on every observed row. Larger tempo changes adapt promptly.
                rowInterval_ = std::abs(interval-rowInterval_) > rowInterval_*.5
                    ? interval : rowInterval_*.75 + interval*.25;
                const double headroom = std::clamp(double(now-lastTick_), 16.0, 60.0);
                glideDuration_ = std::clamp(rowInterval_ + headroom, 25.0, 500.0);
                gliding_ = true;
            } else {
                gliding_ = false;
                topRow_ = target;
            }
            rowTime_ = playing && !paused ? now : -1;
        } else if (!playing || paused || !timely || !smoothScroll_) {
            snapToPlaying();
        } else if (rowTime_ < 0) rowTime_ = now;
    }
    lastTick_ = now;
    if (gliding_ && isVisible()) {
        if (!animationTimer_.isActive()) animationTimer_.start();
    } else animationTimer_.stop();
    if (oldTop != topRow_) {
        updateScrollBars();
        requestVisiblePatterns();
    }
    // No repaint for unchanged/paused telemetry. Manual views still highlight.
    if (moved || oldTop != topRow_) viewport()->update();
}

void TrackerView::setSmoothScrolling(bool smooth)
{
    if (smoothScroll_ == smooth) return;
    smoothScroll_ = smooth;
    if (following_) snapToPlaying();
    viewport()->update();
}

void TrackerView::scrollToPlaying()
{
    following_ = true;
    emit followingChanged(true);
    snapToPlaying();
    requestVisiblePatterns();
    viewport()->update();
}

void TrackerView::setFollowing(bool following)
{
    if (following_ == following) return;
    following_ = following;
    gliding_ = false; rowTime_ = -1;
    animationTimer_.stop();
    emit followingChanged(following);
    if (following) scrollToPlaying();
}

void TrackerView::focusVisible()
{
    if (following_) snapToPlaying();
    requestVisiblePatterns();
    viewport()->update();
}

int TrackerView::visibleRows() const
{
    return std::max(1, (viewport()->height() - headerHeight_) / lineHeight_);
}

void TrackerView::relayout()
{
    rowCache_.clear();
    const int cw = QFontMetrics(monoFont_).horizontalAdvance(QLatin1Char('M'));
    const int available = std::max(1, (viewport()->width() - 8) / cw - 4);
    // Same progressive layouts as Python: 3/2/3/3 fields with one-space gaps.
    const int widths[] = {17, 14, 9, 4};
    layout_ = Notes;
    for (int mode = Full; mode <= Notes; ++mode) {
        if (widths[mode] * std::max(1, channels_) <= available) {
            layout_ = Layout(mode); break;
        }
    }
    channelWidth_ = widths[layout_] * cw;
    shownChannels_ = std::min(std::max(1, channels_), std::max(1, available / widths[layout_]));
    updateScrollBars();
}

void TrackerView::updateScrollBars()
{
    // Setting a scrollbar is bookkeeping, never a user scroll. Without these
    // blockers truncating a fractional top row used to silently disable Follow.
    const QSignalBlocker vertical(verticalScrollBar());
    // Keep horizontal rangeChanged live: QAbstractScrollArea uses it to show
    // the channel-window scrollbar. It does not affect vertical Follow.
    verticalScrollBar()->setRange(lines_.isEmpty() ? 0 : int(std::floor(centeredTop(0))),
                                  lines_.isEmpty() ? 0 : int(std::ceil(centeredTop(lines_.size()-1))));
    verticalScrollBar()->setValue(qRound(topRow_));
    horizontalScrollBar()->setRange(0, std::max(0, channels_ - shownChannels_));
    horizontalScrollBar()->setSingleStep(1);
    horizontalScrollBar()->setPageStep(shownChannels_);
}

void TrackerView::requestVisiblePatterns()
{
    if (lines_.isEmpty()) return;
    const int first = std::max(0, int(std::floor(topRow_)) - 1);
    const int last = std::min(int(lines_.size()) - 1, int(std::ceil(topRow_)) + visibleRows() + 2);
    for (int i = first; i <= last; ++i) {
        const int pattern = orders_.value(lines_[i].orderIndex, -1);
        if (pattern >= 0 && !requestedPatterns_.contains(pattern)) {
            requestedPatterns_.insert(pattern);
            emit patternNeeded(pattern);
        }
    }
}

void TrackerView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
    if (following_) snapToPlaying();
    requestVisiblePatterns();
    viewport()->update();
}

void TrackerView::hideEvent(QHideEvent *event)
{
    animationTimer_.stop();
    if (following_) snapToPlaying();
    QAbstractScrollArea::hideEvent(event);
}

void TrackerView::showEvent(QShowEvent *event)
{
    QAbstractScrollArea::showEvent(event);
    if (following_) snapToPlaying();
    requestVisiblePatterns();
}

void TrackerView::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    // a sideways touchpad swipe has no vertical part: it scrolls channels
    // instead of switching Follow off and scrolling nowhere
    const bool vertical = event->angleDelta().y() != 0 || event->pixelDelta().y() != 0;
    if (!vertical && event->angleDelta().x() != 0) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                        - qRound(event->angleDelta().x() / 120.0));
    } else if (event->modifiers() & Qt::ShiftModifier) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(steps));
    } else if (vertical) {
        setFollowing(false);
        const double delta = event->pixelDelta().y() != 0 ? double(event->pixelDelta().y()) / lineHeight_ : steps * 3;
        topRow_ = std::clamp(topRow_ - delta, centeredTop(0), centeredTop(std::max(0, int(lines_.size())-1)));
        updateScrollBars(); requestVisiblePatterns(); viewport()->update();
    }
    event->accept();
}

void TrackerView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->position().y() >= headerHeight_) {
        const int line = int(std::floor(topRow_ + (event->position().y()-headerHeight_) / lineHeight_));
        if (line >= 0 && line < lines_.size() && !lines_[line].empty) {
            setFocus(Qt::MouseFocusReason);
            emit rowClicked(lines_[line].orderIndex, lines_[line].row);
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::mousePressEvent(event);
}

int TrackerView::selectedPattern() const
{
    return selectedPattern_;
}

void TrackerView::jumpPattern(int direction)
{
    if (orders_.isEmpty())
        return;
    int order = currentOrder_;
    if (direction > 0) {
        for (int step = 1; step <= orders_.size(); ++step) {
            order = (order + 1) % orders_.size();
            if (orders_.value(order, -1) >= 0)
                break;
        }
    } else {
        for (int step = 1; step <= orders_.size(); ++step) {
            order = (order - 1 + orders_.size()) % orders_.size();
            if (orders_.value(order, -1) >= 0)
                break;
        }
        if (order == currentOrder_) {
            // already at the start of this pattern: go to the previous one
            for (int step = 1; step <= orders_.size(); ++step) {
                order = (order - 1 + orders_.size()) % orders_.size();
                if (orders_.value(order, -1) >= 0)
                    break;
            }
        }
    }
    emit rowClicked(order, 0);
}

QColor TrackerView::effectColor(const QString &effect) const
{
    if (palette_ && !effect.isEmpty()) {
        const QChar letter = effect.at(0).toUpper();
        const QChar param = effect.size() > 1 ? effect.at(1).toUpper() : QChar();
        // The module format family decides which letter map applies.
        static const struct { const char *family; const char *global; const char *volume;
                             const char *pan; const char *pitch; const char *misc; } maps[] = {
            {"mod", "BDF", "7AC", "8", "01234", "569"},
            {"xm", "BDF", "ACGHLT", "8PY", "01234", "569KRZ\\"},
            {"s3m", "ABCT", "DIMNRVW", "PXY", "EFGHJU", "KLOQZ\\"},
        };
        int familyIndex = 0;
        if (family_ == QLatin1String("xm"))
            familyIndex = 1;
        else if (family_ == QLatin1String("s3m") || family_ == QLatin1String("it")
                 || family_ == QLatin1String("mptm"))
            familyIndex = 2;
        const auto &map = maps[familyIndex];
        auto categoryFor = [&](const char *set) { return QString::fromLatin1(set).contains(letter); };
        QString category;
        if (letter == QLatin1Char('E') || letter == QLatin1Char('X') || letter == QLatin1Char('S')) {
            // sub-command tables (same values as effects.py)
            static const struct { QChar param; const char *category; } sub[] = {
                {'0', "misc"}, {'1', "pitch"}, {'2', "pitch"}, {'3', "pitch"}, {'4', "pitch"},
                {'5', "pitch"}, {'6', "global"}, {'7', "volume"}, {'8', "pan"}, {'9', "misc"},
                {'A', "volume"}, {'B', "volume"}, {'C', "misc"}, {'D', "misc"}, {'E', "global"},
                {'F', "misc"},
            };
            for (const auto &entry : sub) {
                if (entry.param == param) {
                    category = QString::fromLatin1(entry.category);
                    break;
                }
            }
        }
        if (category.isEmpty()) {
            if (categoryFor(map.global)) category = QStringLiteral("global");
            else if (categoryFor(map.volume)) category = QStringLiteral("volume");
            else if (categoryFor(map.pan)) category = QStringLiteral("pan");
            else if (categoryFor(map.pitch)) category = QStringLiteral("pitch");
            else if (categoryFor(map.misc)) category = QStringLiteral("misc");
        }
        using R = Palette::Role;
        if (category == QLatin1String("global"))
            return (*palette_)[R::EFFECT_GLOBAL];
        if (category == QLatin1String("volume"))
            return (*palette_)[R::EFFECT_VOLUME];
        if (category == QLatin1String("pan"))
            return (*palette_)[R::EFFECT_PAN];
        if (category == QLatin1String("pitch"))
            return (*palette_)[R::EFFECT_PITCH];
        if (category == QLatin1String("misc"))
            return (*palette_)[R::EFFECT_MISC];
    }
    return palette_ ? (*palette_)[Palette::TRACKER_BRIGHT] : QColor(Qt::white);
}

void TrackerView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    const Palette fallback = Palette::builtin(QStringLiteral("dark"));
    const Palette &pal = palette_ ? *palette_ : fallback;
    p.fillRect(viewport()->rect(), pal[Palette::BG_INPUT]);
    p.setFont(monoFont_);
    const int cw = QFontMetrics(monoFont_).horizontalAdvance(QLatin1Char('M'));
    const int startChannel = horizontalScrollBar()->value();
    const int endChannel = std::min(channels_, startChannel + shownChannels_);
    // The body has its own clip, so a fractional top row never paints over
    // channel numbers (the old smooth-scroll/header truncation bug).
    p.save();
    p.setClipRect(QRect(0, headerHeight_, viewport()->width(), viewport()->height()-headerHeight_));
    if (lines_.isEmpty()) {
        p.setPen(pal[Palette::FG_FAINT]);
        p.drawText(viewport()->rect().adjusted(0, headerHeight_, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                   path_.isEmpty() ? tr("Nothing playing") : tr("Loading pattern data…"));
    }
    const int first = std::max(0, int(std::floor(topRow_)));
    const int last = std::min(int(lines_.size()), int(std::ceil(topRow_)) + visibleRows() + 2);
    const int playing = playingLine();
    const qreal dpr = viewport()->devicePixelRatioF();
    if (cacheSize_ != viewport()->size() || cacheDpr_ != dpr
        || cacheFont_ != monoFont_ || cacheChannel_ != startChannel) {
        rowCache_.clear(); cacheSize_ = viewport()->size(); cacheDpr_ = dpr;
        cacheFont_ = monoFont_; cacheChannel_ = startChannel;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int line = first; line < last; ++line) {
        const auto &l = lines_[line];
        const qreal screenY = headerHeight_ + (line-topRow_) * lineHeight_;
        const qreal y = 0;
        const bool highlight = line == playing && !path_.isEmpty();
        const quint64 key = quint64(line)*2 + (highlight ? 1 : 0);
        QPixmap *raster = rowCache_.object(key);
        QPixmap fresh;
        if (!raster) {
            fresh = QPixmap(qCeil(viewport()->width()*dpr), qCeil(lineHeight_*dpr));
            fresh.setDevicePixelRatio(dpr);
            fresh.fill(Qt::transparent);
            QPainter rowPainter(&fresh);
            rowPainter.setFont(monoFont_);
            auto &p = rowPainter;
            p.fillRect(QRectF(0, y, viewport()->width(), lineHeight_), highlight ? pal[Palette::TRACKER_PLAYING]
                       : (l.row % 4 == 0 ? pal[Palette::TRACKER_BEAT] : pal[Palette::BG_INPUT]));
            p.setPen(highlight ? pal[Palette::ON_ACCENT] : pal[Palette::FG_DIM]);
            p.drawText(QRectF(4, y, rowNumberWidth_-10, lineHeight_), Qt::AlignRight | Qt::AlignVCenter,
                       l.empty ? QStringLiteral("---") : QStringLiteral("%1").arg(l.row, 2, 10, QLatin1Char('0')));
            if (l.empty) {
                p.setPen(pal[Palette::FG_FAINT]);
                p.drawText(QRectF(rowNumberWidth_+2, y, 250, lineHeight_), Qt::AlignLeft | Qt::AlignVCenter,
                           tr("(empty pattern %1)").arg(orders_.value(l.orderIndex, -1)));
            } else {
                const auto cached = patterns_.constFind(orders_.value(l.orderIndex, -1));
                for (int ch = startChannel; ch < endChannel; ++ch) {
                    PatternDataReply::Cell cell;
                    if (cached != patterns_.constEnd() && l.row < cached->cells.size() && ch < cached->cells[l.row].size())
                        cell = cached->cells[l.row][ch];
                    int x = rowNumberWidth_ + (ch-startChannel) * channelWidth_;
                    const QColor faint = pal[highlight ? Palette::TRACKER_DIM : Palette::TRACKER_FAINT];
                    auto field = [&](QString text, int width, QColor color) {
                        if (text.isEmpty() || text == QLatin1String("...") || text == QLatin1String("..")) {
                            text = QString(width, QLatin1Char('.')); color = faint;
                        }
                        p.setPen(color);
                        p.drawText(QRectF(x, y, cw*width, lineHeight_), Qt::AlignLeft | Qt::AlignVCenter, text.left(width));
                        x += cw*(width+1);
                    };
                    field(cell.note, 3, pal[Palette::TRACKER_BRIGHT]);
                    if (layout_ == Full) field(cell.instrument, 2, pal[highlight ? Palette::TRACKER_BRIGHT : Palette::TRACKER_DIM]);
                    if (layout_ == Full || layout_ == NoInstrument) {
                        field(cell.volume, 3, pal[Palette::EFFECT_VOLUME]);
                        field(cell.effect, 3, effectColor(cell.effect));
                    } else if (layout_ == Shared) {
                        const bool effect = !cell.effect.isEmpty();
                        field(effect ? cell.effect : cell.volume, 3, effect ? effectColor(cell.effect) : pal[Palette::EFFECT_VOLUME]);
                    }
                    p.setPen(pal[Palette::SEP]);
                    p.drawLine(QPointF(x-cw, y), QPointF(x-cw, y+lineHeight_));
                }
            } // nonempty row
        } // row painter (finish before storing or drawing the pixmap)
        if (!raster) {
            const int cost = fresh.width()*fresh.height()*4;
            // Oversized single rows can still paint without exceeding the cache.
            if (cost <= rowCache_.maxCost()) {
                raster = new QPixmap(fresh);
                rowCache_.insert(key, raster, cost);
            }
        }
        p.drawPixmap(QPointF(0, screenY), raster ? *raster : fresh);
    }
    p.restore();
    // Fixed header is drawn last and contains only complete channel slots.
    p.fillRect(QRect(0, 0, viewport()->width(), headerHeight_), pal[Palette::BG_ALT]);
    p.setPen(pal[Palette::FG_DIM]);
    const int contentChars[] = {14, 11, 7, 3};
    for (int ch=startChannel; ch<endChannel; ++ch) {
        const int x = rowNumberWidth_ + (ch-startChannel)*channelWidth_;
        p.drawText(QRect(x, 0, contentChars[layout_]*cw, headerHeight_), Qt::AlignCenter,
                   QStringLiteral("%1").arg(ch+1, 2, 10, QLatin1Char('0')));
    }
    p.setPen(pal[Palette::SEP]);
    p.drawLine(0, headerHeight_-1, viewport()->width(), headerHeight_-1);
}
