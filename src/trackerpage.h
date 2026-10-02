#pragma once
// The tracker tab: a header with the position and the pattern buttons over
// the pattern view. It asks the engine for the song's patterns itself; a
// token per request keeps replies about an older song out.
#include "engine.h"
#include "theme.h"

#include <QWidget>

class QLabel;
class StableLabelButton;
class TrackerView;

class TrackerPage : public QWidget {
    Q_OBJECT
public:
    TrackerPage(Engine &engine, QWidget *parent = nullptr);

    void sync(const EngineSnapshot &snap);      // request the song data once it is loaded
    void refresh(const EngineSnapshot &snap);   // every UI tick: sync, playhead, header
    void songChanged();                         // another song or subsong was requested
    void clear();                               // nothing plays any more
    void setSmoothScrolling(bool smooth);
    void applyPalette(const Palette *palette);

private:
    Engine &engine_;
    QLabel *header_ = nullptr;
    StableLabelButton *following_ = nullptr;
    TrackerView *view_ = nullptr;
    int token_ = 0;          // of the newest request; replies with another one are stale
    QString songKey_;        // path|subsong|generation the song data was requested for
};
