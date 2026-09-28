// Hand-painted widgets: the channel VU meter and the tracker pattern view.
#pragma once

#include "engine.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>
#include <QVector>
#include <QSet>
#include <QWidget>
#include <QSlider>
#include <QPushButton>
#include <QCache>
#include <QPixmap>
#include <QTimer>

class Palette;

// Text-only toggle button that reserves the styled space needed by every label.
// Measurements remain font/DPI/style-aware rather than fixing a pixel width.
class StableLabelButton : public QPushButton {
    Q_OBJECT
public:
    StableLabelButton(const QStringList &labels, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
private:
    QStringList labels_;
};

// Absolute left-click positioning without Qt's page-step/repeat behavior.
// A press requests immediately; a drag previews and requests again on release.
class JumpSlider : public QSlider {
    Q_OBJECT
public:
    using QSlider::QSlider;
    void applyPalette(const Palette *palette) { palette_ = palette; update(); }
signals:
    void positionRequested(int value);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
private:
    const Palette *palette_ = nullptr;
    int valueAt(const QPoint &point) const;
    int pressValue_ = 0;
    int dragOffset_ = 0;
};

// VuMeter: master L/R bars + one thin bar per module channel, fast attack / smooth release.
class VuMeter : public QWidget {
    Q_OBJECT
public:
    explicit VuMeter(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(330, 74); }
    QSize minimumSizeHint() const override { return QSize(60, 74); }
    void setChannels(int count);
    int channelCount() const { return channels_.size(); }
    float channelLevel(int index) const { return channels_.value(index).level; }
    float leftLevel() const { return masterL_; }
    void updateValues(const QVector<float> &vu, float levelL, float levelR);
    void applyPalette(const Palette *palette);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Channel { float level = 0.f; };
    QElapsedTimer meterClock_;
    static constexpr int kBaseline = 38;    // channel bars hang from here
    static constexpr int kBottom = 70;      // ... down to this line
    static constexpr int kBarGap = 2;
    static constexpr int kRailH = 8;
    static constexpr int kRailStep = 11;
    QVector<Channel> channels_;
    float masterL_ = 0.f, masterR_ = 0.f;
    const Palette *palette_ = nullptr;
};

// TrackerView: virtual pattern grid over the order list (tracker.py port).
class TrackerView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TrackerView(QWidget *parent = nullptr);

    void applyPalette(const Palette *palette);
    void setSong(const QString &path, const QVector<int> &orders, const QHash<int, int> &rows,
                 int channels, int numPatterns);
    void setFamily(const QString &formatLower);
    void setPatternData(const PatternDataReply &reply);
    void clearSong();
    void setPlaying(int order, int row, bool playing, bool paused);
    void setFollowing(bool following);
    bool following() const { return following_; }
    void scrollToPlaying();
    int selectedPattern() const;      // -1: follow the song
    void focusVisible();              // ensure the current line is on screen

    enum PatternJump { PrevPattern = -1, NextPattern = 1 };
    void jumpPattern(int direction);

signals:
    void rowClicked(int order, int row);
    void followingChanged(bool following);
    void patternNeeded(int pattern);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    friend class TestTracker;
    enum Layout { Full, NoInstrument, Shared, Notes };
    void requestVisiblePatterns();
    double centeredTop(int line) const;
    void advanceScroll(qint64 now);
    void snapToPlaying();
    // Rasterized rows avoid thousands of drawText/layout operations per frame.
    // Byte-costed LRU: bounded independently of song length.
    QCache<quint64, QPixmap> rowCache_{32 * 1024 * 1024};
    QSize cacheSize_;
    QFont cacheFont_;
    qreal cacheDpr_ = 0;
    int cacheChannel_ = -1;
    QTimer animationTimer_;
    double rowInterval_ = 120.0;
    QElapsedTimer scrollClock_;
    qint64 lastTick_ = -1, rowTime_ = -1, glideStarted_ = 0;
    double glideFrom_ = 0.0, glideTarget_ = 0.0, glideDuration_ = 120.0;
    bool gliding_ = false;
    Layout layout_ = Full;
    int shownChannels_ = 1;
    QSet<int> requestedPatterns_;
    struct Line {
        int orderIndex = 0;   // index into orders_
        int row = 0;          // row inside the pattern
        bool empty = false;   // placeholder for an empty pattern
    };

    void rebuildLines();
    void relayout();
    int visibleRows() const;
    int lineHeight() const { return lineHeight_; }
    void updateScrollBars();
    QColor effectColor(const QString &effect) const;
    int playingLine() const;

    QString path_;
    QVector<int> orders_;
    QHash<int, int> rows_;
    QHash<int, PatternDataReply> patterns_;   // fetched pattern cells
    QVector<Line> lines_;
    QHash<int, int> blockStart_;              // order index -> first line
    int channels_ = 0;
    int numPatterns_ = 0;
    int currentOrder_ = 0, currentRow_ = 0;
    bool playing_ = false, paused_ = false;
    bool following_ = true;
    double topRow_ = 0.0;                     // float for smooth scrolling
    bool smoothScroll_ = false;
    QString family_;                 // module format family for effect colors
public:
    void setSmoothScrolling(bool smooth);
    int lineHeight_ = 16, rowNumberWidth_ = 44, channelWidth_ = 130, headerHeight_ = 20;
    int selectedPattern_ = -1;
    const Palette *palette_ = nullptr;
    QFont monoFont_;
};
