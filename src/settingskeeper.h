#pragma once
// Saving the window's settings (config.json). The command line's values are
// for this session: a field still holding one is saved with the value it had
// before; a field the user changed in the app since is saved as usual. Also
// what the next start restores: the resume position and the window geometry.
#include "config.h"
#include "sessionoptions.h"

#include <QRect>

struct EngineSnapshot;

class SettingsKeeper {
public:
    explicit SettingsKeeper(Settings &settings) : settings_(settings) {}

    bool save() const { return save(settings_); }
    bool save(const Settings &s) const;
    bool adopt(const Settings &updated);    // saved first: a failed save changes nothing
    // Takes the command line's values for this session; returns the settings
    // as they were before.
    const Settings &beginSession(SessionOptions options);

    void rememberPlayback(const EngineSnapshot &snap);   // the resume position
    QRect windowGeometry() const;           // invalid when there is none
    void setWindowGeometry(const QRect &rect);

private:
    Settings persisted(const Settings &s) const;

    Settings &settings_;
    SessionOptions session_;                // what the command line set (not saved)
    Settings beforeSession_;                // the saved values it hides
};
