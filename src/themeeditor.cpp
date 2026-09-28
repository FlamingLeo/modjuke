#include "themeeditor.h"
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
using R = Palette;
struct RoleInfo { R::Role role; int group; const char *label; };
const char *groups[] = {QT_TRANSLATE_NOOP("ThemeEditorDialog", "Surfaces"),
    QT_TRANSLATE_NOOP("ThemeEditorDialog", "Text and accents"),
    QT_TRANSLATE_NOOP("ThemeEditorDialog", "Status and meters"),
    QT_TRANSLATE_NOOP("ThemeEditorDialog", "Tracker"),
    QT_TRANSLATE_NOOP("ThemeEditorDialog", "Seek bar"),
    QT_TRANSLATE_NOOP("ThemeEditorDialog", "Tracker effects")};
const RoleInfo roles[] = {
    {R::BG,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Window background")},
    {R::BG_PANEL,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Panel background")},
    {R::BG_ALT,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Alternate background")},
    {R::BG_STRIPE,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Striped rows")},
    {R::BG_INPUT,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Input background")},
    {R::BUTTON_OFF_BG,0,QT_TRANSLATE_NOOP("ThemeEditorDialog","Inactive buttons")},
    {R::FG,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Main text")},
    {R::FG_DIM,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Secondary text")},
    {R::FG_FAINT,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Faint text")},
    {R::ACCENT,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Accent")},
    {R::ACCENT_DIM,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Soft accent")},
    {R::ON_ACCENT,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Text on accent")},
    {R::SEP,1,QT_TRANSLATE_NOOP("ThemeEditorDialog","Separators")},
    {R::GREEN,2,QT_TRANSLATE_NOOP("ThemeEditorDialog","Success / low level")},
    {R::AMBER,2,QT_TRANSLATE_NOOP("ThemeEditorDialog","Warning / mid level")},
    {R::RED,2,QT_TRANSLATE_NOOP("ThemeEditorDialog","Error / peak level")},
    {R::PURPLE,2,QT_TRANSLATE_NOOP("ThemeEditorDialog","Information")},
    {R::VU_BASELINE,2,QT_TRANSLATE_NOOP("ThemeEditorDialog","Meter baseline")},
    {R::TRACKER_BEAT,3,QT_TRANSLATE_NOOP("ThemeEditorDialog","Beat background")},
    {R::TRACKER_PLAYING,3,QT_TRANSLATE_NOOP("ThemeEditorDialog","Playing row")},
    {R::TRACKER_FAINT,3,QT_TRANSLATE_NOOP("ThemeEditorDialog","Empty cells")},
    {R::TRACKER_DIM,3,QT_TRANSLATE_NOOP("ThemeEditorDialog","Dim cells")},
    {R::TRACKER_BRIGHT,3,QT_TRANSLATE_NOOP("ThemeEditorDialog","Notes")},
    {R::SEEK_TRACK,4,QT_TRANSLATE_NOOP("ThemeEditorDialog","Seek track")},
    {R::SEEK_TRACK_OFF,4,QT_TRANSLATE_NOOP("ThemeEditorDialog","Inactive seek track")},
    {R::SEEK_FILL_OFF,4,QT_TRANSLATE_NOOP("ThemeEditorDialog","Inactive seek fill")},
    {R::SEEK_KNOB,4,QT_TRANSLATE_NOOP("ThemeEditorDialog","Seek handle")},
    {R::SEEK_MARKER,4,QT_TRANSLATE_NOOP("ThemeEditorDialog","Seek marker")},
    {R::EFFECT_GLOBAL,5,QT_TRANSLATE_NOOP("ThemeEditorDialog","Global effects")},
    {R::EFFECT_VOLUME,5,QT_TRANSLATE_NOOP("ThemeEditorDialog","Volume effects")},
    {R::EFFECT_PAN,5,QT_TRANSLATE_NOOP("ThemeEditorDialog","Panning effects")},
    {R::EFFECT_PITCH,5,QT_TRANSLATE_NOOP("ThemeEditorDialog","Pitch effects")},
    {R::EFFECT_MISC,5,QT_TRANSLATE_NOOP("ThemeEditorDialog","Other effects")}
};
static_assert(std::size(roles) == Palette::NUM_ROLES);
bool validHex(const QString &s) {
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return s.size() == 7 && re.match(s).hasMatch();
}
double luminance(const QColor &c) {
    auto linear = [](double n) { return n <= .04045 ? n / 12.92 : std::pow((n + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}
}

ThemePreview::ThemePreview(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("themePreview"));
    setMinimumSize(420, 260);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Theme preview"));
    setAccessibleDescription(tr("Click a sample to select its color role, or use the arrow keys. All roles are also available in the color role list."));
}
void ThemePreview::setColors(const Palette &palette) { palette_ = palette; update(); }
void ThemePreview::setSelectedRole(Palette::Role role) { selected_ = role; update(); }

void ThemePreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette_[R::BG]);
    const qreal scale = std::min(width() / 520.0, height() / 400.0);
    transform_ = QTransform();
    transform_.translate((width() - 520 * scale) / 2, (height() - 400 * scale) / 2);
    transform_.scale(scale, scale);
    p.setTransform(transform_);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font = this->font(); font.setPixelSize(13); p.setFont(font);
    hits_.clear();
    auto box = [&](QRectF rect, R::Role role) {
        p.fillRect(rect, palette_[role]); hits_.append({rect, role});
    };
    auto text = [&](QRectF rect, R::Role role, const QString &s) {
        p.setPen(palette_[role]); p.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, s);
        // Only the glyph area selects text; the surrounding area selects its surface.
        const qreal w = std::min(rect.width(), QFontMetricsF(p.font()).horizontalAdvance(s));
        hits_.append({QRectF(rect.x(), rect.center().y()-9, w, 18), role});
    };
    box(QRectF(0,0,520,400), R::BG);
    box(QRectF(12,12,496,38), R::BG_PANEL);
    text(QRectF(24,12,160,38), R::ACCENT, tr("PLAYER"));
    text(QRectF(330,12,165,38), R::FG_DIM, tr("02 / 24 modules"));
    box(QRectF(12,60,496,28), R::BG_INPUT);
    text(QRectF(24,60,460,28), R::FG_FAINT, tr("Search modules…"));
    box(QRectF(12,98,496,25), R::BG_ALT);
    text(QRectF(24,98,320,25), R::FG_DIM, tr("Module"));
    text(QRectF(425,98,75,25), R::FG_DIM, tr("Length"));
    text(QRectF(24,123,365,25), R::FG, QStringLiteral("Night drive.mod"));
    text(QRectF(425,123,75,25), R::FG_DIM, QStringLiteral("03:24"));
    box(QRectF(12,148,496,25), R::BG_STRIPE);
    text(QRectF(24,148,365,25), R::FG, QStringLiteral("Last light.xm"));
    box(QRectF(12,173,496,25), R::ACCENT_DIM);
    text(QRectF(24,173,365,25), R::FG, QStringLiteral("Selected module.it"));
    box(QRectF(12,209,88,28), R::ACCENT);
    text(QRectF(28,209,70,28), R::ON_ACCENT, tr("Playing"));
    box(QRectF(110,209,90,28), R::BUTTON_OFF_BG);
    text(QRectF(127,209,72,28), R::FG_DIM, tr("Repeat"));
    box(QRectF(220,213,285,4), R::VU_BASELINE);
    box(QRectF(220,207,153,8), R::GREEN);
    box(QRectF(376,207,78,8), R::AMBER);
    box(QRectF(457,207,24,8), R::RED);
    text(QRectF(220,219,285,20), R::PURPLE, tr("Sample preview • no audio changes"));
    box(QRectF(12,250,496,6), R::SEEK_TRACK);
    box(QRectF(12,250,240,6), R::ACCENT);
    box(QRectF(240,245,9,16), R::SEEK_KNOB);
    box(QRectF(392,245,3,16), R::SEEK_MARKER);
    box(QRectF(12,269,496,4), R::SEEK_TRACK_OFF);
    box(QRectF(12,269,130,4), R::SEEK_FILL_OFF);
    box(QRectF(12,286,496,1), R::SEP);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(13); p.setFont(mono);
    box(QRectF(12,297,496,24), R::TRACKER_BEAT);
    box(QRectF(12,321,496,24), R::TRACKER_PLAYING);
    for (int row=0; row<3; ++row) {
        const int y=297+24*row;
        text(QRectF(24,y,35,24), R::TRACKER_DIM, QString::number(16+row));
        text(QRectF(68,y,53,24), R::TRACKER_BRIGHT, row==1 ? QStringLiteral("D#4") : QStringLiteral("C-4"));
        text(QRectF(130,y,45,24), R::TRACKER_FAINT, QStringLiteral("··"));
        text(QRectF(178,y,58,24), R::EFFECT_GLOBAL, QStringLiteral("F06"));
        text(QRectF(244,y,58,24), R::EFFECT_VOLUME, QStringLiteral("C40"));
        text(QRectF(310,y,58,24), R::EFFECT_PAN, QStringLiteral("880"));
        text(QRectF(376,y,58,24), R::EFFECT_PITCH, QStringLiteral("301"));
        text(QRectF(442,y,58,24), R::EFFECT_MISC, QStringLiteral("E91"));
    }
    p.setFont(font);
    text(QRectF(12,375,495,20), R::FG_DIM, tr("Click a sample to edit its color"));
    // A neutral selection guide stays visible even in intentionally low-contrast palettes.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(luminance(palette_[selected_]) > .4 ? Qt::black : Qt::white, 1, Qt::DashLine));
    for (const Hit &hit : hits_)
        if (hit.role == selected_) p.drawRect(hit.rect.adjusted(1,1,-1,-1));
}
void ThemePreview::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QPointF pt = transform_.inverted().map(event->position());
    for (auto it=hits_.crbegin(); it!=hits_.crend(); ++it)
        if (it->rect.contains(pt)) { emit roleSelected(it->role); return; }
}
void ThemePreview::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pt = transform_.inverted().map(event->position());
    for (auto it=hits_.crbegin(); it!=hits_.crend(); ++it)
        if (it->rect.contains(pt)) {
            setToolTip(QString::fromLatin1(R::roleName(it->role)) + QStringLiteral("  ") + palette_.name(it->role));
            return;
        }
}
void ThemePreview::keyPressEvent(QKeyEvent *event)
{
    int delta = 0;
    if (event->key()==Qt::Key_Right || event->key()==Qt::Key_Down) delta=1;
    if (event->key()==Qt::Key_Left || event->key()==Qt::Key_Up) delta=-1;
    if (delta) emit roleSelected(R::Role((int(selected_)+delta+R::NUM_ROLES)%R::NUM_ROLES));
    else QWidget::keyPressEvent(event);
}

ThemeEditorDialog::ThemeEditorDialog(QWidget *parent, const QString &id, const QString &name,
                                     const Palette &palette, const QJsonObject &existing)
    : QDialog(parent), id_(id), existing_(existing), initial_(palette), palette_(palette)
{
    setObjectName(QStringLiteral("themeEditor"));
    setWindowTitle(existing.contains(id) ? tr("Edit theme") : tr("New theme"));
    auto *root = new QVBoxLayout(this);
    auto *nameRow = new QHBoxLayout;
    auto *label = new QLabel(tr("Theme name"), this);
    name_ = new QLineEdit(name, this); name_->setObjectName(QStringLiteral("themeName"));
    name_->setPlaceholderText(tr("A unique name for your theme")); label->setBuddy(name_);
    nameRow->addWidget(label); nameRow->addWidget(name_,1); root->addLayout(nameRow);
    auto *hint = new QLabel(tr("Start with the selected palette. Select a color role or click the preview to change it."), this);
    hint->setWordWrap(true); root->addWidget(hint);
    auto *split = new QSplitter(this);
    roles_ = new QTreeWidget(split); roles_->setObjectName(QStringLiteral("themeRoles"));
    roles_->setAccessibleName(tr("Color roles"));
    roles_->setColumnCount(2); roles_->setHeaderLabels({tr("Color role"),tr("Hex")});
    roles_->setMinimumWidth(320); roles_->setIndentation(14);
    roles_->setSelectionMode(QAbstractItemView::SingleSelection);
    roles_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    roles_->header()->setStretchLastSection(false);
    roles_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    roles_->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    QTreeWidgetItem *groupItems[6];
    for (int i=0;i<6;++i) {
        groupItems[i]=new QTreeWidgetItem(roles_,{tr(groups[i])});
        groupItems[i]->setFlags(Qt::ItemIsEnabled);
        QFont f=roles_->font(); f.setBold(true); groupItems[i]->setFont(0,f);
        groupItems[i]->setFirstColumnSpanned(true);
    }
    for (const RoleInfo &info: roles) {
        auto *item=new QTreeWidgetItem(groupItems[info.group],{tr(info.label)});
        item->setData(0,Qt::UserRole,int(info.role));
        item->setToolTip(0,QString::fromLatin1(R::roleName(info.role)));
        items_[info.role]=item;
    }
    roles_->expandAll();
    auto *right=new QWidget(split); auto *layout=new QVBoxLayout(right); layout->setContentsMargins(8,0,0,0);
    roleLabel_=new QLabel(right); roleLabel_->setObjectName(QStringLiteral("selectedColorRole"));
    QFont bold=roleLabel_->font(); bold.setBold(true); roleLabel_->setFont(bold); layout->addWidget(roleLabel_);
    auto *colorRow=new QHBoxLayout;
    hex_=new QLineEdit(right); hex_->setObjectName(QStringLiteral("themeHex"));
    hex_->setAccessibleName(tr("Hex color")); hex_->setPlaceholderText(QStringLiteral("#RRGGBB"));
    hex_->setMaxLength(32); hex_->setMinimumWidth(90);
    auto *choose=new QPushButton(tr("Choose color…"),right); choose->setObjectName(QStringLiteral("chooseColor"));
    auto *reset=new QPushButton(tr("Reset color"),right); reset->setObjectName(QStringLiteral("resetColor"));
    reset->setToolTip(tr("Restore this color to its value when the editor opened."));
    colorRow->addWidget(hex_,1); colorRow->addWidget(choose); colorRow->addWidget(reset); layout->addLayout(colorRow);
    preview_=new ThemePreview(right); layout->addWidget(preview_,1);
    contrast_=new QLabel(right); contrast_->setObjectName(QStringLiteral("themeContrast"));
    contrast_->setWordWrap(true); contrast_->setMinimumHeight(2*contrast_->fontMetrics().height());
    layout->addWidget(contrast_);
    split->setSizes({340,560}); split->setCollapsible(0,false); split->setCollapsible(1,false);
    root->addWidget(split,1);
    error_=new QLabel(this); error_->setObjectName(QStringLiteral("themeValidation"));
    error_->setWordWrap(true); error_->setMinimumHeight(2*error_->fontMetrics().height()); root->addWidget(error_);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);
    auto *resetAll=buttons->addButton(tr("Reset all colors…"),QDialogButtonBox::ResetRole);
    resetAll->setObjectName(QStringLiteral("resetAllColors"));
    use_=buttons->addButton(tr("Use theme"),QDialogButtonBox::AcceptRole); use_->setObjectName(QStringLiteral("useTheme"));
    use_->setDefault(true); root->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&ThemeEditorDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(resetAll,&QPushButton::clicked,this,&ThemeEditorDialog::resetColors);
    connect(choose,&QPushButton::clicked,this,&ThemeEditorDialog::chooseColor);
    connect(reset,&QPushButton::clicked,this,[this] { palette_[role_]=initial_[role_]; selectRole(role_); refreshColors(); });
    connect(roles_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem *item) {
        if (item && item->data(0,Qt::UserRole).isValid()) selectRole(R::Role(item->data(0,Qt::UserRole).toInt()));
    });
    connect(preview_,&ThemePreview::roleSelected,this,&ThemeEditorDialog::selectRole);
    connect(name_,&QLineEdit::textChanged,this,&ThemeEditorDialog::validate);
    connect(hex_,&QLineEdit::textChanged,this,[this](const QString &value) {
        if (validHex(value)) { palette_[role_]=QColor(value); refreshColors(); }
        validate();
    });
    selectRole(R::BG); roles_->scrollToTop(); refreshColors(); name_->selectAll(); name_->setFocus();
    const QSize available=screen()->availableGeometry().size()-QSize(40,60);
    resize(QSize(980,700).boundedTo(available));
}

void ThemeEditorDialog::selectRole(Palette::Role role)
{
    role_=role;
    const QSignalBlocker block(roles_), hexBlock(hex_);
    roles_->setCurrentItem(items_[role]); roles_->scrollToItem(items_[role]);
    roleLabel_->setText(items_[role]->text(0)+QStringLiteral(" · ")+QString::fromLatin1(R::roleName(role)));
    hex_->setText(palette_.name(role)); preview_->setSelectedRole(role); validate();
}
void ThemeEditorDialog::refreshColors()
{
    for (int i=0;i<R::NUM_ROLES;++i) {
        const R::Role role=R::Role(i);
        QPixmap swatch(16,16); swatch.fill(palette_[role]);
        items_[i]->setIcon(0,QIcon(swatch)); items_[i]->setText(1,palette_.name(role));
    }
    preview_->setColors(palette_);
    const double a=luminance(palette_[R::FG]), b=luminance(palette_[R::BG]);
    const double ratio=(std::max(a,b)+.05)/(std::min(a,b)+.05);
    contrast_->setText((ratio<4.5 ? tr("Low main-text contrast: %1:1. Consider adjusting the text or background.")
                                : tr("Main-text contrast: %1:1. Preview changes stay inside this editor.")).arg(ratio,0,'f',1));
    validate();
}
QJsonObject ThemeEditorDialog::definition() const
{
    QJsonObject colors;
    for (int i=0;i<R::NUM_ROLES;++i) colors.insert(QString::fromLatin1(R::roleName(R::Role(i))),palette_.name(R::Role(i)));
    // This legacy spelling is the shared Python/Qt file-format key, not UI text.
    return {{QStringLiteral("name"),name_->text().trimmed()},{QStringLiteral("colours"),colors}};
}
void ThemeEditorDialog::validate()
{
    QString message;
    const QString name=name_->text().trimmed();
    if (!validHex(hex_->text())) message=tr("Enter a six-digit hex color, such as #5aa9ff.");
    else if (name.isEmpty() || name.size()>60) message=tr("Enter a theme name between 1 and 60 characters.");
    else if (!existing_.contains(id_) && existing_.size()>=100) message=tr("You can store up to 100 custom themes. Delete one in Settings first.");
    else {
        QJsonObject proposed=existing_; proposed.insert(id_,definition());
        const QJsonObject cleaned=R::cleanCustomThemes(proposed);
        if (!cleaned.contains(id_) || cleaned.size()!=proposed.size())
            message=tr("Use a unique name, not a built-in theme name or alias. Control characters and the custom: prefix are not allowed.");
    }
    valid_=message.isEmpty(); use_->setEnabled(valid_);
    error_->setText(valid_ ? tr("Changes apply only after you save Settings. Cancel discards this editor's changes.") : message);
}
void ThemeEditorDialog::accept() { validate(); if (valid_) QDialog::accept(); }
void ThemeEditorDialog::chooseColor()
{
    QColorDialog dialog(palette_[role_],this);
    dialog.setWindowTitle(tr("Choose color")); dialog.setOption(QColorDialog::DontUseNativeDialog);
    if (dialog.exec()==QDialog::Accepted) {
        palette_[role_]=dialog.currentColor().toRgb(); selectRole(role_); refreshColors();
    }
}
void ThemeEditorDialog::resetColors()
{
    if (QMessageBox::question(this,tr("Reset all colors"),
            tr("Restore all colors to their values when the editor opened? The theme name will be kept."),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
    palette_=initial_; selectRole(role_); refreshColors();
}
