// Color palettes mirroring modjuke/theme.py (same roles, same 5 built-in
// themes, plus custom themes read from the settings file). Themes apply as a
// Qt stylesheet + a palette struct for the hand-painted widgets.
#pragma once

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct Palette {
    // role names follow theme.py COLOUR_NAMES
    enum Role {
        BG, BG_PANEL, BG_ALT, BG_STRIPE, BG_INPUT, BUTTON_OFF_BG,
        FG, FG_DIM, FG_FAINT,
        ACCENT, ACCENT_DIM, GREEN, AMBER, RED, PURPLE, SEP,
        TRACKER_BEAT, TRACKER_PLAYING, TRACKER_FAINT, TRACKER_DIM, TRACKER_BRIGHT,
        ON_ACCENT, VU_BASELINE, SEEK_TRACK, SEEK_TRACK_OFF, SEEK_FILL_OFF,
        SEEK_KNOB, SEEK_MARKER,
        EFFECT_GLOBAL, EFFECT_VOLUME, EFFECT_PAN, EFFECT_PITCH, EFFECT_MISC,
        NUM_ROLES
    };
    static const char *roleName(Role role);
    static Role roleFromName(const QString &name, bool *ok = nullptr);

    QColor colors[NUM_ROLES];
    QColor &operator[](Role role) { return colors[role]; }
    const QColor &operator[](Role role) const { return colors[role]; }

    // convenience: "#rrggbb" for stylesheets
    QString name(Role role) const { return colors[role].name(); }

    static Palette builtin(const QString &themeKey);
    static QStringList builtinThemes();               // dark, light, midnight, high-contrast, amber
    static QString themeLabel(const QString &key);
    // Resolve settings theme (built-in or "custom:<id>" / plain custom name).
    static Palette resolve(const QString &themeKey, const QJsonObject &customThemes);

    // mirrors modjuke/theme.py: keeps valid custom palettes (full colors merged
    // onto dark, hex lowercased) and drops the rest; caps at kMaxCustomThemes.
    static QJsonObject cleanCustomThemes(const QJsonObject &raw);

    // custom themes: "custom:<id>" -> {"name", "colours"}, at most this many
    static constexpr int kMaxCustomThemes = 100;
    static QString customThemeName(const QJsonObject &customThemes, const QString &id);
    // "My theme", or "My theme N" when taken (case-insensitive)
    static QString suggestCustomThemeName(const QJsonObject &customThemes);
    static QString newCustomThemeId();                // "custom:<uuid>"
    // customThemes with id set to definition, cleaned. *ok is false (and
    // customThemes comes back unchanged) when cleaning would drop any entry:
    // an invalid or clashing name.
    static QJsonObject withCustomTheme(const QJsonObject &customThemes, const QString &id,
                                       const QJsonObject &definition, bool *ok);
    // mirrors theme.normalise(): resolves aliases, case, and custom display
    // names to a canonical key ("dark" fallback).
    static QString normalizeTheme(const QString &name, const QJsonObject &customThemes);
    QString toStyleSheet() const;                     // Qt widgets stylesheet
};
