#include "theme.h"
#include "casefold.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>

static void initArrowResources() { Q_INIT_RESOURCE(arrows); }

namespace {

struct RoleEntry {
    Palette::Role role;
    const char *name;
    const char *dark;
    const char *light;
    const char *midnight;
    const char *contrast;
    const char *amber;
};

// Values copied from modjuke/theme.py (dark == module defaults).
const RoleEntry kRoles[] = {
    {Palette::BG, "BG", "#1b1d23", "#f2f3f6", "#0b0f1a", "#000000", "#120c02"},
    {Palette::BG_PANEL, "BG_PANEL", "#23262e", "#ffffff", "#111726", "#0d0d0d", "#1a1206"},
    {Palette::BG_ALT, "BG_ALT", "#2a2e38", "#e2e5ec", "#1a2333", "#2b2b2b", "#2a1d09"},
    {Palette::BG_STRIPE, "BG_STRIPE", "#272b35", "#f4f6f9", "#161e30", "#222222", "#231a0a"},
    {Palette::BG_INPUT, "BG_INPUT", "#15171c", "#eef1f6", "#070a12", "#000000", "#0b0700"},
    {Palette::BUTTON_OFF_BG, "BUTTON_OFF_BG", "#16181e", "#edeff4", "#10151f", "#000000", "#1c1305"},
    {Palette::FG, "FG", "#e7e9ee", "#1b1f27", "#dbe4f5", "#ffffff", "#ffcf7f"},
    {Palette::FG_DIM, "FG_DIM", "#939aab", "#5a6474", "#8493b0", "#d0d0d0", "#c08f43"},
    {Palette::FG_FAINT, "FG_FAINT", "#5c6370", "#8b94a5", "#4d5a75", "#9a9a9a", "#8a6430"},
    {Palette::ACCENT, "ACCENT", "#5aa9ff", "#1a6ef5", "#4f8cff", "#4fd2ff", "#ffb000"},
    {Palette::ACCENT_DIM, "ACCENT_DIM", "#2f5f96", "#b7d3fb", "#1f3f7a", "#005f8a", "#6f4700"},
    {Palette::GREEN, "GREEN", "#4ac97e", "#0f7a42", "#3fb87a", "#00e676", "#a8d05f"},
    {Palette::AMBER, "AMBER", "#e0b050", "#7a5300", "#d9a544", "#ffc400", "#ffb000"},
    {Palette::RED, "RED", "#e06c75", "#c62b39", "#e0606c", "#ff5252", "#ff7043"},
    {Palette::PURPLE, "PURPLE", "#b48ead", "#7b4fa8", "#a48ad4", "#d0a2ff", "#d9a441"},
    {Palette::SEP, "SEP", "#4d5567", "#b3bac7", "#2c3a55", "#8a8a8a", "#5c3f14"},
    {Palette::TRACKER_BEAT, "TRACKER_BEAT", "#20242c", "#e6eaf1", "#0f1523", "#141414", "#170f03"},
    {Palette::TRACKER_PLAYING, "TRACKER_PLAYING", "#2f5f96", "#b7d3fb", "#1f3f7a", "#005f8a", "#6f4700"},
    {Palette::TRACKER_FAINT, "TRACKER_FAINT", "#3f4757", "#aab3c2", "#2a3550", "#8a8a8a", "#6b4a1c"},
    {Palette::TRACKER_DIM, "TRACKER_DIM", "#a9b1c2", "#38414f", "#9aa8c4", "#dcdcdc", "#e0ad5e"},
    {Palette::TRACKER_BRIGHT, "TRACKER_BRIGHT", "#ffffff", "#11151c", "#ffffff", "#ffffff", "#ffe3a8"},
    {Palette::ON_ACCENT, "ON_ACCENT", "#ffffff", "#10305c", "#ffffff", "#ffffff", "#ffe8b0"},
    {Palette::VU_BASELINE, "VU_BASELINE", "#3a3f4b", "#c3cad6", "#1e2940", "#666666", "#4a3208"},
    {Palette::SEEK_TRACK, "SEEK_TRACK", "#31353f", "#c3cad6", "#1e2a42", "#4a4a4a", "#6b4a1c"},
    {Palette::SEEK_TRACK_OFF, "SEEK_TRACK_OFF", "#282c34", "#d8dde5", "#151d2e", "#333333", "#3d2907"},
    {Palette::SEEK_FILL_OFF, "SEEK_FILL_OFF", "#4a5160", "#b3bac7", "#2b3a56", "#666666", "#7a5518"},
    {Palette::SEEK_KNOB, "SEEK_KNOB", "#f2f4f8", "#ffffff", "#dfe7f6", "#ffffff", "#ffd98a"},
    {Palette::SEEK_MARKER, "SEEK_MARKER", "#6d7686", "#6b7484", "#5c6d8f", "#ffffff", "#c58e3a"},
    {Palette::EFFECT_GLOBAL, "EFFECT_GLOBAL", "#ff8b7b", "#800000", "#ff8f8f", "#ff7b7b", "#ff9a6b"},
    {Palette::EFFECT_VOLUME, "EFFECT_VOLUME", "#7bd88a", "#008000", "#6fd39a", "#4dff88", "#b9d97a"},
    {Palette::EFFECT_PAN, "EFFECT_PAN", "#5fd0d0", "#008080", "#5ccfe6", "#4dd8e6", "#7fd8c0"},
    {Palette::EFFECT_PITCH, "EFFECT_PITCH", "#d8c85f", "#808000", "#d9cc66", "#ffd24d", "#ffcf5c"},
    {Palette::EFFECT_MISC, "EFFECT_MISC", "#a8b2c4", "#808080", "#9aa8c4", "#cfcfcf", "#c9a86b"},
};

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
    return {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("midnight"),
            QStringLiteral("high-contrast"), QStringLiteral("amber")};
}

QString Palette::themeLabel(const QString &key)
{
    if (key == QLatin1String("dark"))
        return QObject::tr("Dark");
    if (key == QLatin1String("light"))
        return QObject::tr("Light");
    if (key == QLatin1String("midnight"))
        return QObject::tr("Midnight");
    if (key == QLatin1String("high-contrast"))
        return QObject::tr("High contrast");
    if (key == QLatin1String("amber"))
        return QObject::tr("Amber CRT");
    return key;
}

Palette Palette::builtin(const QString &themeKey)
{
    int column = 0;    // dark
    if (themeKey == QLatin1String("light"))
        column = 1;
    else if (themeKey == QLatin1String("midnight"))
        column = 2;
    else if (themeKey == QLatin1String("high-contrast"))
        column = 3;
    else if (themeKey == QLatin1String("amber"))
        column = 4;

    Palette palette;
    for (const RoleEntry &entry : kRoles) {
        const char *hex = column == 0   ? entry.dark
                          : column == 1 ? entry.light
                          : column == 2 ? entry.midnight
                          : column == 3 ? entry.contrast
                                        : entry.amber;
        palette[entry.role] = QColor(QString::fromLatin1(hex));
    }
    return palette;
}

Palette Palette::resolve(const QString &themeKey, const QJsonObject &customThemes)
{
    // Built-in names win; otherwise look for "custom:<id>" or a stored name.
    if (builtinThemes().contains(themeKey))
        return builtin(themeKey);
    QJsonObject definition;
    if (customThemes.contains(themeKey) && customThemes.value(themeKey).isObject()) {
        definition = customThemes.value(themeKey).toObject();
    } else {
        for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it) {
            if (it.value().toObject().value(QStringLiteral("name")).toString() == themeKey) {
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
QWidget#infoPanel { background: @PANEL@; }
QLabel { background: transparent; }
QLabel[role="dim"] { color: @DIM@; }
QLabel[role="title"] { font-size: 15pt; font-weight: 600; }
QLabel[role="time"] { font-family: monospace; }
QPushButton { background: @ALT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 4px 10px; }
QPushButton:hover { background: @STRIPE@; border-color: @ACCENT@; }
QPushButton:pressed { background: @ACCENT_DIM@; color: @ON_ACCENT@; }
QPushButton:checked { background: @ACCENT_DIM@; border-color: @ACCENT@; color: @ON_ACCENT@; }
QPushButton:disabled { color: @FAINT@; background: @BUTTON_OFF@; border-color: @SEP@; }
QPushButton[role="accent"] { background: @ACCENT@; color: @ON_ACCENT@; border-color: @ACCENT@; }
QPushButton[role="accent"]:hover { background: @ACCENT_DIM@; }
QPushButton[role="accent"]:disabled { background: @BUTTON_OFF@; color: @FAINT@; border-color: @SEP@; }
QToolButton { background: @ALT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 3px 8px; }
QToolButton:hover { border-color: @ACCENT@; }
QToolButton:disabled { color: @FAINT@; background: @BUTTON_OFF@; }
QToolButton::menu-indicator { width: 10px; height: 10px; }
QLineEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox { background: @INPUT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 2px 4px; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; }
QPlainTextEdit#logView { color: @DIM@; }
QComboBox { background: @INPUT@; color: @FG@; border: 1px solid @SEP@; border-radius: 4px; padding: 2px 6px; }
QComboBox:disabled { color: @FAINT@; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox, QDoubleSpinBox { padding-right: 24px; min-height: 1.5em; }
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 21px; height: 0.85em; border-left: 1px solid @SEP@; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 21px; height: 0.85em; border-left: 1px solid @SEP@; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @INPUT@; border-color: @ACCENT@; }
QComboBox QAbstractItemView { background: @ALT@; color: @FG@; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; border: 1px solid @SEP@; }
QCheckBox { background: transparent; spacing: 5px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid @SEP@; border-radius: 3px; background: @INPUT@; }
QCheckBox::indicator:checked { background: @ACCENT@; border-color: @ACCENT@; }
QTableView, QTreeView, QListWidget { background: @BG@; alternate-background-color: @STRIPE@; color: @FG@; gridline-color: @SEP@; border: 1px solid @SEP@; selection-background-color: @ACCENT_DIM@; selection-color: @ON_ACCENT@; }
QHeaderView::section { background: @ALT@; color: @FG@; border: none; border-right: 1px solid @SEP@; border-bottom: 1px solid @SEP@; padding: 3px 6px; }
QTableCornerButton::section { background: @ALT@; border: none; }
QScrollBar:vertical { background: @BG@; width: 12px; margin: 0; }
QScrollBar:horizontal { background: @BG@; height: 12px; margin: 0; }
QScrollBar::handle { background: @ALT@; border-radius: 5px; min-height: 24px; min-width: 24px; }
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
QSplitter::handle:horizontal { background: @PANEL@; width: 3px; }
QMenu { background: @ALT@; color: @FG@; border: 1px solid @SEP@; }
QMenu::item:selected { background: @ACCENT_DIM@; color: @ON_ACCENT@; }
QMenu::separator { height: 1px; background: @SEP@; margin: 4px 8px; }
QStatusBar { background: @BG@; color: @DIM@; }
QStatusBar::item { border: none; }
QToolTip { background: @ALT@; color: @FG@; border: 1px solid @SEP@; }
QDialog { background: @BG@; }
QGroupBox { border: 1px solid @SEP@; border-radius: 4px; margin-top: 8px; padding-top: 4px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; color: @DIM@; }
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
    auto color = [this](Role role) { return colors[role].name(); };
    qss.replace(QStringLiteral("@BG@"), color(BG));
    qss.replace(QStringLiteral("@PANEL@"), color(BG_PANEL));
    qss.replace(QStringLiteral("@ALT@"), color(BG_ALT));
    qss.replace(QStringLiteral("@STRIPE@"), color(BG_STRIPE));
    qss.replace(QStringLiteral("@INPUT@"), color(BG_INPUT));
    qss.replace(QStringLiteral("@BUTTON_OFF@"), color(BUTTON_OFF_BG));
    qss.replace(QStringLiteral("@FG@"), color(FG));
    qss.replace(QStringLiteral("@DIM@"), color(FG_DIM));
    qss.replace(QStringLiteral("@FAINT@"), color(FG_FAINT));
    qss.replace(QStringLiteral("@ACCENT@"), color(ACCENT));
    qss.replace(QStringLiteral("@ACCENT_DIM@"), color(ACCENT_DIM));
    qss.replace(QStringLiteral("@ON_ACCENT@"), color(ON_ACCENT));
    qss.replace(QStringLiteral("@SEP@"), color(SEP));
    qss.replace(QStringLiteral("@RED@"), color(RED));
    qss.replace(QStringLiteral("@SEEK_TRACK@"), color(SEEK_TRACK));
    qss.replace(QStringLiteral("@SEEK_TRACK_OFF@"), color(SEEK_TRACK_OFF));
    qss.replace(QStringLiteral("@SEEK_FILL_OFF@"), color(SEEK_FILL_OFF));
    qss.replace(QStringLiteral("@SEEK_KNOB@"), color(SEEK_KNOB));
    return qss;
}

namespace {

const char *const kThemeLabels[] = {"Dark", "Light", "Midnight", "High contrast", "Amber CRT"};

QString aliasTheme(const QString &normalizedKey)
{
    // theme.py _ALIASES (keys pre-normalized, no spaces/underscores)
    if (normalizedKey == QLatin1String("default"))
        return QStringLiteral("dark");
    if (normalizedKey == QLatin1String("contrast") || normalizedKey == QLatin1String("highcontrast")
        || normalizedKey == QLatin1String("hc"))
        return QStringLiteral("high-contrast");
    if (normalizedKey == QLatin1String("ambercrt") || normalizedKey == QLatin1String("crt"))
        return QStringLiteral("amber");
    if (normalizedKey == QLatin1String("lighttheme"))
        return QStringLiteral("light");
    return normalizedKey;
}

bool validHexColor(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return value.toString().size() == 7 && hex.match(value.toString()).hasMatch();
}

}   // namespace

QJsonObject Palette::cleanCustomThemes(const QJsonObject &raw)
{
    static const QRegularExpression idRe(QStringLiteral("^custom:[a-z0-9-]{1,64}$"));
    QSet<QString> reserved;
    for (const QString &name : builtinThemes())
        reserved.insert(caseFold(name));
    const QStringList themes = builtinThemes();
    for (const char *label : kThemeLabels)
        reserved.insert(caseFold(QString::fromLatin1(label)));
    for (const char *alias : {"default", "contrast", "highcontrast", "hc", "ambercrt", "crt", "lighttheme"})
        reserved.insert(QString::fromLatin1(alias));

    QJsonObject result;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
        if (result.size() >= 100)
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
        QString dashKey = name.toLower();
        dashKey.replace(QLatin1Char('_'), QLatin1Char('-'));
        dashKey.replace(QLatin1Char(' '), QLatin1Char('-'));
        QString noDash = dashKey;
        noDash.remove(QLatin1Char('-'));
        const QString alias = aliasTheme(noDash) == noDash ? aliasTheme(dashKey) : aliasTheme(noDash);
        if (themes.contains(alias))
            continue;   // shadows a built-in palette name
        bool bad = false;
        for (auto cit = colors.constBegin(); cit != colors.constEnd(); ++cit) {
            bool known = false;
            roleFromName(cit.key(), &known);
            if (known && !validHexColor(cit.value()))
                bad = true;
        }
        if (bad)
            continue;
        QJsonObject palette;
        const Palette dark = builtin(QStringLiteral("dark"));
        for (int role = BG; role < NUM_ROLES; ++role)
            palette.insert(QString::fromLatin1(roleName(Palette::Role(role))),
                           dark[Palette::Role(role)].name(QColor::HexRgb).toLower());
        for (auto cit = colors.constBegin(); cit != colors.constEnd(); ++cit) {
            bool known = false;
            const Role role = roleFromName(cit.key(), &known);
            if (known && validHexColor(cit.value()))
                palette.insert(cit.key(), cit.value().toString().toLower());
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
        const QString entryName = it.value().toObject().value(QStringLiteral("name")).toString();
        if (folded == caseFold(it.key()) || folded == caseFold(entryName))
            return it.key();
    }
    for (auto it = customThemes.constBegin(); it != customThemes.constEnd(); ++it) {
        const QString entryName = it.value().toObject().value(QStringLiteral("name")).toString();
        if (folded == caseFold(entryName + QStringLiteral(" (custom)")))
            return it.key();
    }
    QString key = text.toLower();
    key.replace(QLatin1Char('_'), QLatin1Char('-'));
    key.replace(QLatin1Char(' '), QLatin1Char('-'));
    QString noDash = key;
    noDash.remove(QLatin1Char('-'));
    const QString viaAlias = aliasTheme(noDash);
    if (viaAlias != noDash)
        key = viaAlias;
    else
        key = aliasTheme(key);
    return builtinThemes().contains(key) ? key : QStringLiteral("dark");
}
