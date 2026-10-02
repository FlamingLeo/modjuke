#include "settingskeeper.h"

#include "engine.h"
#include "theme.h"

#include <QRegularExpression>

#include <cmath>

bool SettingsKeeper::save(const Settings &s) const
{
    return persisted(s).save();
}

bool SettingsKeeper::adopt(const Settings &updated)
{
    // saved first: a failed save leaves settings_ (and the UI) as they were
    if (!save(updated))
        return false;
    settings_ = updated;
    return true;
}

Settings SettingsKeeper::persisted(const Settings &s) const
{
    Settings out = s;
    auto keep = [](auto &field, const auto &session, const auto &before) {
        if (session && field == *session)
            field = before;
    };
    keep(out.volume, session_.volume, beforeSession_.volume);
    keep(out.theme, session_.theme, beforeSession_.theme);
    keep(out.interpolation, session_.interpolation, beforeSession_.interpolation);
    keep(out.backend, session_.backend, beforeSession_.backend);
    return out;
}

const Settings &SettingsKeeper::beginSession(SessionOptions options)
{
    beforeSession_ = settings_;   // what persisted() writes back
    if (options.theme)            // a custom theme can be given by its name
        options.theme = Palette::normalizeTheme(*options.theme, settings_.customThemes);
    session_ = options;
    if (options.volume)
        settings_.volume = *options.volume;
    if (options.theme)
        settings_.theme = *options.theme;
    if (options.interpolation)
        settings_.interpolation = *options.interpolation;
    if (options.backend)
        settings_.backend = *options.backend;
    return beforeSession_;
}

void SettingsKeeper::rememberPlayback(const EngineSnapshot &snap)
{
    if (!snap.path.isEmpty() && snap.loaded && !snap.loading) {
        settings_.subsong = snap.subsong;
        settings_.lastPath = snap.path;
        settings_.lastPosition = snap.position;
        if (snap.loop && std::isfinite(snap.duration) && snap.duration > 0.0)
            settings_.lastPosition = std::fmod(snap.position, snap.duration);
        if (snap.ended)
            settings_.lastPosition = 0.0;   // a finished song resumes from its start
    } else if (snap.path.isEmpty()) {
        settings_.lastPath.clear();
        settings_.lastPosition = 0.0;
        settings_.subsong = 0;
    }
}

QRect SettingsKeeper::windowGeometry() const
{
    // Tk geometry: "+X" is a position (X may be negative: "+-1920" on a
    // monitor left of the primary one, which the old pattern rejected);
    // "-X" counts from the far edge (approximated as before)
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)x(\\d+)([+-])(-?\\d+)([+-])(-?\\d+)$"));
    const QRegularExpressionMatch m = re.match(settings_.windowGeometry);
    if (!m.hasMatch())
        return QRect();
    int x = m.captured(4).toInt(), y = m.captured(6).toInt();
    if (m.captured(3) == QLatin1String("-"))
        x = 100 - x;
    if (m.captured(5) == QLatin1String("-"))
        y = 100 - y;
    return QRect(x, y, m.captured(1).toInt(), m.captured(2).toInt());
}

void SettingsKeeper::setWindowGeometry(const QRect &rect)
{
    settings_.windowGeometry = QStringLiteral("%1x%2+%3+%4")
                                   .arg(rect.width()).arg(rect.height())
                                   .arg(rect.x()).arg(rect.y());
}
