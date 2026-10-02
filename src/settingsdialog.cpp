#include "settingsdialog.h"

#include "theme.h"
#include "themeeditor.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <type_traits>

namespace {

// Select the last item holding value (the sample rates list "device default"
// twice; the second one is shown), or the first item when none does.
void selectData(QComboBox *combo, const QVariant &value)
{
    for (int i = combo->count() - 1; i >= 0; --i) {
        if (combo->itemData(i) == value) {
            combo->setCurrentIndex(i);
            return;
        }
    }
    combo->setCurrentIndex(0);
}

}   // namespace

SettingsDialog::SettingsDialog(QWidget *parent, const Settings &settings, const IgnoreStore *ignored,
                               Unignore unignore)
    : QDialog(parent), ignoreStore_(ignored), unignore_(std::move(unignore))
{
    setWindowTitle(tr("Settings"));
    auto *root = new QVBoxLayout(this);

    // Each helper makes a row's widget, loads it from `settings` and registers
    // the writer collect() runs.
    using Items = QVector<QPair<QString, QVariant>>;
    auto addCombo = [&](QFormLayout *form, const QString &label, const Items &items, auto field) {
        auto *combo = new QComboBox(form->parentWidget());
        for (const auto &[text, value] : items)
            combo->addItem(text, value);
        selectData(combo, QVariant(settings.*field));
        form->addRow(label, combo);
        writers_ << [combo, field](Settings &out) {
            out.*field = combo->currentData().value<std::decay_t<decltype(out.*field)>>();
        };
        return combo;
    };
    auto addSpin = [&](QFormLayout *form, const QString &label, int min, int max, const QString &suffix,
                       int Settings::*field) {
        auto *spin = new QSpinBox(form->parentWidget());
        spin->setRange(min, max);
        if (!suffix.isEmpty())
            spin->setSuffix(suffix);
        spin->setValue(settings.*field);
        form->addRow(label, spin);
        writers_ << [spin, field](Settings &out) { out.*field = spin->value(); };
    };
    auto addCheck = [&](QFormLayout *form, const QString &text, bool Settings::*field) {
        auto *check = new QCheckBox(text, form->parentWidget());
        check->setChecked(settings.*field);
        form->addRow(check);
        writers_ << [check, field](Settings &out) { out.*field = check->isChecked(); };
    };

    auto *playback = new QGroupBox(tr("Playback"), this);
    auto *playForm = new QFormLayout(playback);
    addCombo(playForm, tr("Audio backend"),
             {{tr("Default output"), QStringLiteral("auto")},
              {tr("Silent (no audio device)"), QStringLiteral("null")}},
             &Settings::backend);
    Items rates = {{tr("Auto (device default)"), 0}};
    for (int rate : Settings::sampleRateChoices())
        rates.append({Settings::sampleRateLabel(rate), rate});
    addCombo(playForm, tr("Sample rate"), rates, &Settings::samplerate);
    addCombo(playForm, tr("Interpolation"),
             {{tr("No interpolation"), QStringLiteral("off")},
              {tr("Linear (2-tap)"), QStringLiteral("linear")},
              {tr("Cubic (4-tap)"), QStringLiteral("cubic")},
              {tr("Sinc (8-tap)"), QStringLiteral("sinc")}},
             &Settings::interpolation);
    addSpin(playForm, tr("Buffer"), 20, 5000, QStringLiteral(" ms"), &Settings::bufferMs);
    addSpin(playForm, tr("UI refresh"), 5, 120, QString(), &Settings::uiFps);
    root->addWidget(playback);

    auto *between = new QGroupBox(tr("Between runs"), this);
    auto *betweenForm = new QFormLayout(between);
    addCheck(betweenForm, tr("Analyze new files automatically"), &Settings::autoAnalyze);
    addCheck(betweenForm, tr("Keep duration/title details (analysis cache)"), &Settings::cacheAnalysis);
    addCheck(betweenForm, tr("Remember the subsong and playing position"), &Settings::rememberPosition);
    addCheck(betweenForm, tr("Record local listening stats"), &Settings::trackListeningStats);
    addCheck(betweenForm, tr("Smooth tracker scrolling"), &Settings::smoothTrackerScrolling);
    root->addWidget(between);

    auto *appearance = new QGroupBox(tr("Appearance"), this);
    auto *appearanceForm = new QFormLayout(appearance);
    themeCombo_ = new QComboBox(appearance);
    themeCombo_->setObjectName(QStringLiteral("themeCombo"));
    customThemes_ = Palette::cleanCustomThemes(settings.customThemes);
    appearanceForm->addRow(tr("Theme"), themeCombo_);
    auto *themeActions = new QHBoxLayout;
    newTheme_ = new QPushButton(tr("New theme…"), appearance);
    editTheme_ = new QPushButton(tr("Edit…"), appearance);
    deleteTheme_ = new QPushButton(tr("Delete"), appearance);
    newTheme_->setObjectName(QStringLiteral("newTheme"));
    editTheme_->setObjectName(QStringLiteral("editTheme"));
    deleteTheme_->setObjectName(QStringLiteral("deleteTheme"));
    themeActions->addWidget(newTheme_); themeActions->addWidget(editTheme_);
    themeActions->addWidget(deleteTheme_); themeActions->addStretch();
    appearanceForm->addRow(QString(), themeActions);
    connect(newTheme_, &QPushButton::clicked, this, [this] { editTheme(true); });
    connect(editTheme_, &QPushButton::clicked, this, [this] { editTheme(false); });
    connect(deleteTheme_, &QPushButton::clicked, this, &SettingsDialog::deleteTheme);
    connect(themeCombo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateThemeButtons);
    refreshThemes(settings.theme);
    addCombo(appearanceForm, tr("Window title"),
             {{tr("Track name"), QStringLiteral("track")},
              {tr("Module title"), QStringLiteral("title")},
              {tr("File name"), QStringLiteral("filename")},
              {tr("Nothing"), QStringLiteral("none")}},
             &Settings::windowTitleMode);
    root->addWidget(appearance);

    skipIgnoreConfirmation_ = new QCheckBox(tr("Don't ask again when ignoring songs"), this);
    skipIgnoreConfirmation_->setObjectName(QStringLiteral("skipIgnoreConfirmation"));
    skipIgnoreConfirmation_->setChecked(!settings.confirmIgnore);
    skipIgnoreConfirmation_->setToolTip(tr("Uncheck to show the ignore confirmation again."));
    writers_ << [this](Settings &out) { out.confirmIgnore = !skipIgnoreConfirmation_->isChecked(); };
    root->addWidget(skipIgnoreConfirmation_);

    auto *robust = new QHBoxLayout;
    robust->addWidget(new QLabel(tr("Ignored files:"), this));
    ignoredCountLabel_ = new QLabel(this);
    robust->addWidget(ignoredCountLabel_);
    auto *ignoredBtn = new QPushButton(tr("Manage ignored songs…"), this);
    connect(ignoredBtn, &QPushButton::clicked, this, [this] {
        IgnoreDialog dialog(this, ignoreStore_, unignore_);
        dialog.exec();
        refreshIgnoredCount();
    });
    robust->addWidget(ignoredBtn);
    robust->addStretch();
    root->addLayout(robust);

    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    refreshIgnoredCount();
    resize(480, 560);
}

void SettingsDialog::accept()
{
    // Failed writes leave the dialog and its theme drafts available for retry.
    if (!saveHandler_ || saveHandler_()) QDialog::accept();
}

void SettingsDialog::refreshThemes(const QString &selected)
{
    const QSignalBlocker block(themeCombo_);
    themeCombo_->clear();
    for (const QString &key : Palette::builtinThemes())
        themeCombo_->addItem(Palette::themeLabel(key), key);
    QStringList ids = customThemes_.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return Palette::customThemeName(customThemes_, a).toCaseFolded()
             < Palette::customThemeName(customThemes_, b).toCaseFolded();
    });
    for (const QString &id : ids)
        themeCombo_->addItem(tr("%1 (custom)").arg(Palette::customThemeName(customThemes_, id)), id);
    selectData(themeCombo_, Palette::normalizeTheme(selected, customThemes_));
    updateThemeButtons();
    const bool full = customThemes_.size() >= Palette::kMaxCustomThemes;
    newTheme_->setEnabled(!full);
    newTheme_->setToolTip(full ? tr("Delete a custom theme before creating another (limit: %1).")
                                     .arg(Palette::kMaxCustomThemes)
                               : tr("Copy the selected palette into a new custom theme."));
}

void SettingsDialog::updateThemeButtons()
{
    const bool custom = customThemes_.contains(themeCombo_->currentData().toString());
    editTheme_->setEnabled(custom);
    deleteTheme_->setEnabled(custom);
}

void SettingsDialog::editTheme(bool create)
{
    const QString selected = themeCombo_->currentData().toString();
    if (create ? customThemes_.size() >= Palette::kMaxCustomThemes : !customThemes_.contains(selected))
        return;
    const QString id = create ? Palette::newCustomThemeId() : selected;
    const QString name = create ? Palette::suggestCustomThemeName(customThemes_)
                                : Palette::customThemeName(customThemes_, selected);
    ThemeEditorDialog editor(this, id, name, Palette::resolve(selected, customThemes_), customThemes_);
    if (editor.exec() != QDialog::Accepted)
        return;
    bool ok = false;
    const QJsonObject updated = Palette::withCustomTheme(customThemes_, id, editor.definition(), &ok);
    if (!ok)
        return;
    customThemes_ = updated;
    refreshThemes(id);
}

void SettingsDialog::deleteTheme()
{
    const QString id = themeCombo_->currentData().toString();
    if (!customThemes_.contains(id)) return;
    QMessageBox question(QMessageBox::Question, tr("Delete theme"),
        tr("Delete “%1”? This takes effect only when you save Settings.")
            .arg(Palette::customThemeName(customThemes_, id)),
        QMessageBox::Yes | QMessageBox::No, this);
    question.setTextFormat(Qt::PlainText); question.setDefaultButton(QMessageBox::No);
    if (question.exec()!=QMessageBox::Yes) return;
    customThemes_.remove(id); refreshThemes(QStringLiteral("dark"));
}

void SettingsDialog::refreshIgnoredCount()
{
    ignoredCountLabel_->setText(ignoreStore_ ? QString::number(ignoreStore_->count())
                                             : QStringLiteral("0"));
}

void SettingsDialog::setConfirmIgnore(bool confirm)
{
    skipIgnoreConfirmation_->setChecked(!confirm);
}

void SettingsDialog::collect(Settings &out) const
{
    for (const auto &write : writers_)
        write(out);
    out.theme = themeCombo_->currentData().toString();
    out.customThemes = customThemes_;
}

IgnoreDialog::IgnoreDialog(QWidget *parent, const IgnoreStore *store, SettingsDialog::Unignore unignore)
    : QDialog(parent), store_(store), unignore_(std::move(unignore))
{
    setWindowTitle(tr("Ignored songs"));
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Ignored files are skipped by scans, playlists, and imports."),
                               this));
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    root->addWidget(tree_, 1);
    auto *buttons = new QHBoxLayout;
    auto *removeBtn = new QPushButton(tr("Remove selected"), this);
    connect(removeBtn, &QPushButton::clicked, this, &IgnoreDialog::removeSelected);
    buttons->addWidget(removeBtn);
    auto *clearBtn = new QPushButton(tr("Unignore everything"), this);
    connect(clearBtn, &QPushButton::clicked, this, &IgnoreDialog::clearAll);
    buttons->addWidget(clearBtn);
    buttons->addStretch();
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);
    countLabel_ = new QLabel(this);
    root->addWidget(countLabel_);
    refresh();
    resize(560, 380);
}

void IgnoreDialog::refresh()
{
    tree_->clear();
    for (const QString &path : store_->paths()) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, path);
        item->setData(0, Qt::UserRole, path);
    }
    countLabel_->setText(tr("%1 files ignored").arg(store_->count()));
}

void IgnoreDialog::removeSelected()
{
    QStringList paths;
    for (QTreeWidgetItem *item : tree_->selectedItems())
        paths << item->data(0, Qt::UserRole).toString();
    if (!paths.isEmpty() && unignore_) {
        unignore_(paths);
        refresh();
    }
}

void IgnoreDialog::clearAll()
{
    if (unignore_)
        unignore_(store_->paths());
    refresh();
}
