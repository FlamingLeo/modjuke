#include "theme.h"
#include "casefold.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iterator>

static void initArrowResources() { Q_INIT_RESOURCE(arrows); }

namespace {

// The built-in themes, in the column order of RoleEntry::hex.
struct ThemeEntry {
    const char *key;
    const char *label;
};
const ThemeEntry kThemes[] = {
    {"dark", QT_TRANSLATE_NOOP("QObject", "Dark")},
    {"light", QT_TRANSLATE_NOOP("QObject", "Light")},
    {"midnight", QT_TRANSLATE_NOOP("QObject", "Midnight")},
    {"high-contrast", QT_TRANSLATE_NOOP("QObject", "High contrast")},
    {"amber", QT_TRANSLATE_NOOP("QObject", "Amber CRT")},
};
constexpr int kThemeCount = int(std::size(kThemes));

// position in kThemes, -1 for anything else
int themeIndex(const QString &key)
{
    for (int i = 0; i < kThemeCount; ++i) {
        if (key == QLatin1String(kThemes[i].key))
            return i;
    }
    return -1;
}

struct RoleEntry {
    Palette::Role role;
    const char *name;
    const char *hex[kThemeCount];
};

// Values copied from modjuke/theme.py (dark == module defaults).
const RoleEntry kRoles[] = {
    {Palette::BG, "BG", {"#1b1d23", "#f2f3f6", "#0b0f1a", "#000000", "#120c02"}},
    {Palette::BG_PANEL, "BG_PANEL", {"#23262e", "#ffffff", "#111726", "#0d0d0d", "#1a1206"}},
    {Palette::BG_ALT, "BG_ALT", {"#2a2e38", "#e2e5ec", "#1a2333", "#2b2b2b", "#2a1d09"}},
    {Palette::BG_STRIPE, "BG_STRIPE", {"#272b35", "#f4f6f9", "#161e30", "#222222", "#231a0a"}},
    {Palette::BG_INPUT, "BG_INPUT", {"#15171c", "#eef1f6", "#070a12", "#000000", "#0b0700"}},
    {Palette::BUTTON_OFF_BG, "BUTTON_OFF_BG", {"#16181e", "#edeff4", "#10151f", "#000000", "#1c1305"}},
    {Palette::FG, "FG", {"#e7e9ee", "#1b1f27", "#dbe4f5", "#ffffff", "#ffcf7f"}},
    {Palette::FG_DIM, "FG_DIM", {"#939aab", "#5a6474", "#8493b0", "#d0d0d0", "#c08f43"}},
    {Palette::FG_FAINT, "FG_FAINT", {"#5c6370", "#8b94a5", "#4d5a75", "#9a9a9a", "#8a6430"}},
    {Palette::ACCENT, "ACCENT", {"#5aa9ff", "#1a6ef5", "#4f8cff", "#4fd2ff", "#ffb000"}},
    {Palette::ACCENT_DIM, "ACCENT_DIM", {"#2f5f96", "#b7d3fb", "#1f3f7a", "#005f8a", "#6f4700"}},
    {Palette::GREEN, "GREEN", {"#4ac97e", "#0f7a42", "#3fb87a", "#00e676", "#a8d05f"}},
    {Palette::AMBER, "AMBER", {"#e0b050", "#7a5300", "#d9a544", "#ffc400", "#ffb000"}},
    {Palette::RED, "RED", {"#e06c75", "#c62b39", "#e0606c", "#ff5252", "#ff7043"}},
    {Palette::PURPLE, "PURPLE", {"#b48ead", "#7b4fa8", "#a48ad4", "#d0a2ff", "#d9a441"}},
    {Palette::SEP, "SEP", {"#4d5567", "#b3bac7", "#2c3a55", "#8a8a8a", "#5c3f14"}},
    {Palette::TRACKER_BEAT, "TRACKER_BEAT", {"#20242c", "#e6eaf1", "#0f1523", "#141414", "#170f03"}},
    {Palette::TRACKER_PLAYING, "TRACKER_PLAYING", {"#2f5f96", "#b7d3fb", "#1f3f7a", "#005f8a", "#6f4700"}},
    {Palette::TRACKER_FAINT, "TRACKER_FAINT", {"#3f4757", "#aab3c2", "#2a3550", "#8a8a8a", "#6b4a1c"}},
    {Palette::TRACKER_DIM, "TRACKER_DIM", {"#a9b1c2", "#38414f", "#9aa8c4", "#dcdcdc", "#e0ad5e"}},
    {Palette::TRACKER_BRIGHT, "TRACKER_BRIGHT", {"#ffffff", "#11151c", "#ffffff", "#ffffff", "#ffe3a8"}},
    {Palette::ON_ACCENT, "ON_ACCENT", {"#ffffff", "#10305c", "#ffffff", "#ffffff", "#ffe8b0"}},
    {Palette::VU_BASELINE, "VU_BASELINE", {"#3a3f4b", "#c3cad6", "#1e2940", "#666666", "#4a3208"}},
    {Palette::SEEK_TRACK, "SEEK_TRACK", {"#31353f", "#c3cad6", "#1e2a42", "#4a4a4a", "#6b4a1c"}},
    {Palette::SEEK_TRACK_OFF, "SEEK_TRACK_OFF", {"#282c34", "#d8dde5", "#151d2e", "#333333", "#3d2907"}},
    {Palette::SEEK_FILL_OFF, "SEEK_FILL_OFF", {"#4a5160", "#b3bac7", "#2b3a56", "#666666", "#7a5518"}},
    {Palette::SEEK_KNOB, "SEEK_KNOB", {"#f2f4f8", "#ffffff", "#dfe7f6", "#ffffff", "#ffd98a"}},
    {Palette::SEEK_MARKER, "SEEK_MARKER", {"#6d7686", "#6b7484", "#5c6d8f", "#ffffff", "#c58e3a"}},
    {Palette::EFFECT_GLOBAL, "EFFECT_GLOBAL", {"#ff8b7b", "#800000", "#ff8f8f", "#ff7b7b", "#ff9a6b"}},
    {Palette::EFFECT_VOLUME, "EFFECT_VOLUME", {"#7bd88a", "#008000", "#6fd39a", "#4dff88", "#b9d97a"}},
    {Palette::EFFECT_PAN, "EFFECT_PAN", {"#5fd0d0", "#008080", "#5ccfe6", "#4dd8e6", "#7fd8c0"}},
    {Palette::EFFECT_PITCH, "EFFECT_PITCH", {"#d8c85f", "#808000", "#d9cc66", "#ffd24d", "#ffcf5c"}},
    {Palette::EFFECT_MISC, "EFFECT_MISC", {"#a8b2c4", "#808080", "#9aa8c4", "#cfcfcf", "#c9a86b"}},
};

// a custom theme entry's display name
QString themeName(const QJsonValue &entry)
{
    return entry.toObject().value(QStringLiteral("name")).toString();
}

}  // namespace

const char *Palette::roleName(Role role)
{
    for (const RoleEntry &entry : kRoles) {
        if (entry.role == role)
            return entry.name;
    }
    return "";
}

Palette::Role Palette::roleFromName(const QString &name, bool *ok)
{
    for (const RoleEntry &entry : kRoles) {
        if (name == QLatin1String(entry.name)) {
            if (ok)
                *ok = true;
            return entry.role;
        }
    }
    if (ok)
        *ok = false;
    return BG;
}

QStringList Palette::builtinThemes()
{
    QStringList keys;
    for (const ThemeEntry &theme : kThemes)
        keys << QString::fromLatin1(theme.key);
    return keys;
}

QString Palette::themeLabel(const QString &key)
{
    const int index = themeIndex(key);
    return index < 0 ? key : QObject::tr(kThemes[index].label);
}

Palette Palette::builtin(const QString &themeKey)
{
    const int column = std::max(0, themeIndex(themeKey));   // unknown keys get dark
    Palette palette;
    for (const RoleEntry &entry : kRoles)
        palette[entry.role] = QColor(QString::fromLatin1(entry.hex[column]));
    return palette;
}

Palette Palette::resolve(const QString &themeKey, const QJsonObject &customThemes)
{
    // Built-in names win; otherwise look for "custom:<id>" or a stored name.
    if (themeIndex(themeKey) >= 0)
        return builtin(themeKey);
    QJsonObject definition;
    if (customThemes.contains(themeKey) && customThemes.value(themeKey).isObject()) {
        definition = customThemes.value(themeKey).toObject();
    } else {
        for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it) {
            if (themeName(it.value()) == themeKey) {
                definition = it.value().toObject();
                break;
            }
        }
    }
    if (definition.isEmpty())
        return builtin(QStringLiteral("dark"));
    Palette palette = builtin(QStringLiteral("dark"));
    const QJsonObject colors = definition.value(QStringLiteral("colours")).toObject();
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
        bool ok = false;
        const Role role = roleFromName(it.key(), &ok);
        const QColor color = QColor(it.value().toString());
        if (ok && color.isValid())
            palette[role] = color;
    }
    return palette;
}

static const char kQssTemplate[] = R"QSS(
QWidget { background: @BG@; color: @FG@; }
QMainWindow { background: @BG@; }
QWidget#infoPanel { background: @BG_PANEL@; }
QLabel { background: transparent; }
QLabel[role="dim"] { color: @FG_DIM@; }
QLabel[role="title"] { font-size: 15pt; font-weight: 600; }
QLabel[role="time"] { font-family: monospace; }
QPushButton { background: @BG_ALT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 4px 10px; }
QPushButton:hover { background: @BG_STRIPE@; border-color: @ACCENT@; }
QPushButton:pressed { background: @ACCENT_DIM@; color: @ON_ACCENT@; }
QPushButton:checked { background: @ACCENT_DIM@; border-color: @ACCENT@; color: @ON_ACCENT@; }
QPushButton:disabled { color: @FG_FAINT@; background: @BUTTON_OFF_BG@; border-color: @SEP@; }
QPushButton[role="accent"] { background: @ACCENT@; color: @ON_ACCENT@; border-color: @ACCENT@; }
QPushButton[role="accent"]:hover { background: @ACCENT_DIM@; }
QPushButton[role="accent"]:disabled { background: @BUTTON_OFF_BG@; color: @FG_FAINT@; border-color: @SEP@; }
QToolButton { background: @BG_ALT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 3px 8px; }
QToolButton:hover { border-color: @ACCENT@; }
QToolButton:disabled { color: @FG_FAINT@; background: @BUTTON_OFF_BG@; }
QToolButton::menu-indicator { width: 10px; height: 10px; }
QLineEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox { background: @BG_INPUT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 2px 4px; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; }
QPlainTextEdit#logView { color: @FG_DIM@; }
QComboBox { background: @BG_INPUT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 2px 6px; }
QComboBox:disabled { color: @FG_FAINT@; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox, QDoubleSpinBox { padding-right: 24px; min-height: 1.5em; }
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 21px; height: 0.85em; border-left: 1px solid @SEP@; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 21px; height: 0.85em; border-left: 1px solid @SEP@; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @BG_INPUT@; border-color: @ACCENT@; }
QComboBox QAbstractItemView { background: @BG_ALT@; color: @FG@; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; border: 1px solid @SEP@; }
QCheckBox { background: transparent; spacing: 5px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid @SEP@; border-radius: 3px; background: @BG_INPUT@; }
QCheckBox::indicator:checked { background: @ACCENT@; border-color: @ACCENT@; }
QTableView, QTreeView, QListWidget { background: @BG@; alternate-background-color: @BG_STRIPE@; color: @FG@; gridline-color: @SEP@; border: 1px solid @SEP@; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; }
QHeaderView::section { background: @BG_ALT@; color: @FG@; border: none; border-right: 1px solid @SEP@; border-bottom: 1px solid @SEP@; padding: 3px 6px; }
QTableCornerButton::section { background: @BG_ALT@; border: none; }
QScrollBar:vertical { background: @BG@; width: 12px; margin: 0; }
QScrollBar:horizontal { background: @BG@; height: 12px; margin: 0; }
QScrollBar::handle { background: @BG_ALT@; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: @SEP@; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QWidget#volumeGroup, QWidget#posGroup, QWidget#transportControls { background: transparent; }
QSlider { background: transparent; border: none; }
QSlider::groove:horizontal { border: none; height: 4px; background: @SEEK_TRACK@; border-radius: 2px; }
QSlider::sub-page:horizontal { border: none; height: 4px; background: @ACCENT@; border-radius: 2px; }
QSlider::handle:horizontal { border: none; background: @SEEK_KNOB@; width: 13px; height: 13px; margin: -5px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: @ACCENT@; }
QSlider:disabled::groove:horizontal { background: @SEEK_TRACK_OFF@; }
QSlider:disabled::sub-page:horizontal { background: @SEEK_FILL_OFF@; }
QSlider:disabled::handle:horizontal { background: @SEEK_FILL_OFF@; }
QSplitter::handle:horizontal { background: @BG_PANEL@; width: 3px; }
QMenu { background: @BG_ALT@; color: @FG@; border: 1px solid @SEP@; }
QMenu::item:selected { background: @ACCENT_DIM@; color: @ON_ACCENT@; }
QMenu::separator { height: 1px; background: @SEP@; margin: 4px 8px; }
QStatusBar { background: @BG@; color: @FG_DIM@; }
QStatusBar::item { border: none; }
QToolTip { background: @BG_ALT@; color: @FG@; border: 1px solid @SEP@; }
QDialog { background: @BG@; }
QGroupBox { border: 1px solid @SEP@; border-radius: 4px; margin-top: 8px; padding-top: 4px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; color: @FG_DIM@; }
)QSS";

QString Palette::toStyleSheet() const
{
    initArrowResources();
    QString qss = QString::fromLatin1(kQssTemplate);
    // Select black or white independently for each surface. Even custom
    // palettes get >=4.5:1 solid-arrow contrast; do not trust native/QSS inks.
    auto ink = [this](Role background) {
        auto linear = [](double x) { return x <= .04045 ? x/12.92 : std::pow((x+.055)/1.055,2.4); };
        const QColor c=colors[background];
        const double l=.2126*linear(c.redF())+.7152*linear(c.greenF())+.0722*linear(c.blueF());
        return l > .179 ? QStringLiteral("black") : QStringLiteral("white");
    };
    auto arrow = [&](const QString &selector, const QString &direction, Role bg, bool disabled=false) {
        qss += QStringLiteral("\n%1 { image: url(:/arrows/%2-%3%4.png); width: 10px; height: 10px; }")
            .arg(selector, direction, ink(bg), disabled ? QStringLiteral("-disabled") : QString());
    };
    for (const QString &widget : {QStringLiteral("QSpinBox"),QStringLiteral("QDoubleSpinBox")}) {
        for (const QString &direction : {QStringLiteral("up"),QStringLiteral("down")}) {
            const QString selector=widget+QStringLiteral("::")+direction+QStringLiteral("-arrow");
            arrow(selector,direction,BG_INPUT);
            arrow(selector+QStringLiteral(":disabled"),direction,BG_INPUT,true);
            arrow(selector+QStringLiteral(":off"),direction,BG_INPUT,true); // end of range
        }
    }
    arrow(QStringLiteral("QComboBox::down-arrow"),QStringLiteral("down"),BG_INPUT);
    arrow(QStringLiteral("QComboBox::down-arrow:disabled"),QStringLiteral("down"),BG_INPUT,true);
    for (const QString &control : {QStringLiteral("QToolButton::menu-indicator"),QStringLiteral("QToolButton::menu-arrow"),QStringLiteral("QPushButton::menu-indicator")}) {
        arrow(control,QStringLiteral("down"),BG_ALT);
        arrow(control+QStringLiteral(":hover"),QStringLiteral("down"),BG_STRIPE);
        arrow(control+QStringLiteral(":checked"),QStringLiteral("down"),ACCENT_DIM);
        arrow(control+QStringLiteral(":pressed"),QStringLiteral("down"),ACCENT_DIM);
        arrow(control+QStringLiteral(":disabled"),QStringLiteral("down"),BUTTON_OFF_BG,true);
    }
    arrow(QStringLiteral("QHeaderView::up-arrow"),QStringLiteral("up"),BG_ALT);
    arrow(QStringLiteral("QHeaderView::down-arrow"),QStringLiteral("down"),BG_ALT);
    arrow(QStringLiteral("QMenu::right-arrow"),QStringLiteral("right"),BG_ALT);
    arrow(QStringLiteral("QMenu::right-arrow:selected"),QStringLiteral("right"),ACCENT_DIM);
    arrow(QStringLiteral("QMenu::right-arrow:disabled"),QStringLiteral("right"),BG_ALT,true);
    arrow(QStringLiteral("QMenu::left-arrow"),QStringLiteral("left"),BG_ALT);
    arrow(QStringLiteral("QTreeView::branch:has-children:closed"),QStringLiteral("right"),BG);
    arrow(QStringLiteral("QTreeView::branch:has-children:open"),QStringLiteral("down"),BG);
    arrow(QStringLiteral("QTreeView::branch:has-children:closed:selected"),QStringLiteral("right"),ACCENT_DIM);
    arrow(QStringLiteral("QTreeView::branch:has-children:open:selected"),QStringLiteral("down"),ACCENT_DIM);
    // @ROLE@ placeholders, named after the palette roles
    for (const RoleEntry &entry : kRoles)
        qss.replace(QLatin1Char('@') + QLatin1String(entry.name) + QLatin1Char('@'), colors[entry.role].name());
    return qss;
}

namespace {

// theme.py _ALIASES (keys pre-normalized, no spaces/underscores)
const struct {
    const char *alias;
    const char *theme;
} kAliases[] = {
    {"default", "dark"},
    {"contrast", "high-contrast"}, {"highcontrast", "high-contrast"}, {"hc", "high-contrast"},
    {"ambercrt", "amber"}, {"crt", "amber"},
    {"lighttheme", "light"},
};

QString aliasTheme(const QString &normalizedKey)
{
    for (const auto &entry : kAliases) {
        if (normalizedKey == QLatin1String(entry.alias))
            return QString::fromLatin1(entry.theme);
    }
    return normalizedKey;
}

// theme.normalise() without the custom names: lower case, "_" and " " as "-",
// then the aliases, matched without dashes first
QString canonicalThemeKey(const QString &text)
{
    QString key = text.toLower();
    key.replace(QLatin1Char('_'), QLatin1Char('-'));
    key.replace(QLatin1Char(' '), QLatin1Char('-'));
    QString noDash = key;
    noDash.remove(QLatin1Char('-'));
    const QString viaAlias = aliasTheme(noDash);
    return viaAlias != noDash ? viaAlias : aliasTheme(key);
}

bool validHexColor(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return value.toString().size() == 7 && hex.match(value.toString()).hasMatch();
}

// "#rrggbb" (lower case) from a stored color; the short "#rgb" form is
// expanded; anything else gives an empty string
QString cleanHexColor(const QJsonValue &value)
{
    if (validHexColor(value))
        return value.toString().toLower();
    if (!value.isString())
        return QString();
    static const QRegularExpression shortHex(QStringLiteral("^#[0-9a-fA-F]{3}$"));
    const QString text = value.toString();
    if (text.size() != 4 || !shortHex.match(text).hasMatch())
        return QString();
    QString out = QStringLiteral("#");
    for (int i = 1; i < 4; ++i)
        out += QString(2, text.at(i));
    return out.toLower();
}

}   // namespace

QJsonObject Palette::cleanCustomThemes(const QJsonObject &raw)
{
    static const QRegularExpression idRe(QStringLiteral("^custom:[a-z0-9-]{1,64}$"));
    QSet<QString> reserved;
    for (const ThemeEntry &theme : kThemes) {
        reserved.insert(caseFold(QString::fromLatin1(theme.key)));
        reserved.insert(caseFold(QString::fromLatin1(theme.label)));
    }
    for (const auto &entry : kAliases)
        reserved.insert(QString::fromLatin1(entry.alias));

    QJsonObject result;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
        if (result.size() >= kMaxCustomThemes)
            break;
        const QString &key = it.key();
        if (!idRe.match(key).hasMatch() || !it.value().isObject())
            continue;
        const QJsonObject entry = it.value().toObject();
        const QJsonValue nameValue = entry.value(QStringLiteral("name"));
        if (!nameValue.isString())
            continue;
        const QString name = nameValue.toString().trimmed();
        if (name.isEmpty() || name.size() > 60 || caseFold(name).startsWith(QLatin1String("custom:")))
            continue;
        bool control = false;
        for (const QChar &ch : name)
            control = control || ch.unicode() < 32;
        if (control || reserved.contains(caseFold(name)))
            continue;
        if (!entry.value(QStringLiteral("colours")).isObject())
            continue;
        const QJsonObject colors = entry.value(QStringLiteral("colours")).toObject();
        if (themeIndex(canonicalThemeKey(name)) >= 0)
            continue;   // shadows a built-in palette name
        // An invalid color only loses that role (it falls back to the dark
        // palette's value below); it used to drop the whole theme, which the
        // next save then deleted from config.json.
        QJsonObject palette;
        const Palette dark = builtin(QStringLiteral("dark"));
        for (int role = BG; role < NUM_ROLES; ++role)
            palette.insert(QString::fromLatin1(roleName(Palette::Role(role))),
                           dark[Palette::Role(role)].name(QColor::HexRgb).toLower());
        for (auto cit = colors.constBegin(); cit != colors.constEnd(); ++cit) {
            bool known = false;
            roleFromName(cit.key(), &known);
            const QString hex = known ? cleanHexColor(cit.value()) : QString();
            if (!hex.isEmpty())
                palette.insert(cit.key(), hex);
        }
        QJsonObject cleaned;
        cleaned.insert(QStringLiteral("name"), name);
        cleaned.insert(QStringLiteral("colours"), palette);
        result.insert(key, cleaned);
        reserved.insert(caseFold(name));
    }
    return result;
}

QString Palette::normalizeTheme(const QString &name, const QJsonObject &customThemes)
{
    const QString text = name.trimmed();
    if (text.isEmpty())
        return QStringLiteral("dark");
    const QString folded = caseFold(text);
    for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it) {
        if (folded == caseFold(it.key()) || folded == caseFold(themeName(it.value())))
            return it.key();
    }
    for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it) {
        if (folded == caseFold(themeName(it.value()) + QStringLiteral(" (custom)")))
            return it.key();
    }
    const QString key = canonicalThemeKey(text);
    return themeIndex(key) >= 0 ? key : QStringLiteral("dark");
}

QString Palette::customThemeName(const QJsonObject &customThemes, const QString &id)
{
    return themeName(customThemes.value(id));
}

QString Palette::suggestCustomThemeName(const QJsonObject &customThemes)
{
    QSet<QString> used;
    for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it)
        used.insert(themeName(it.value()).toCaseFolded());
    // translated in the context of the dialog that used to make the name
    QString name = QCoreApplication::translate("SettingsDialog", "My theme");
    for (int n = 2; used.contains(name.toCaseFolded()); ++n)
        name = QCoreApplication::translate("SettingsDialog", "My theme %1").arg(n);
    return name;
}

QString Palette::newCustomThemeId()
{
    return QStringLiteral("custom:") + QUuid::createUuid().toString(QUuid::Id128);
}

QJsonObject Palette::withCustomTheme(const QJsonObject &customThemes, const QString &id,
                                     const QJsonObject &definition, bool *ok)
{
    QJsonObject proposed = customThemes;
    proposed.insert(id, definition);
    const QJsonObject cleaned = cleanCustomThemes(proposed);
    // cleaning must keep every entry: a dropped one means a bad name or a clash
    *ok = cleaned.size() == proposed.size() && cleaned.contains(id);
    return *ok ? cleaned : customThemes;
}
