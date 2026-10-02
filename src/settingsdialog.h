// Settings and the ignored-files list it opens (ports of the ui.py settings
// dialog and ignoreview.py).
#pragma once

#include "config.h"
#include "ignorestore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVector>
#include <functional>

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // unignore: removes paths from the ignore list (the owner reports errors
    // and rebuilds its queue)
    using Unignore = std::function<bool(const QStringList &paths)>;
    SettingsDialog(QWidget *parent, const Settings &settings, const IgnoreStore *ignored,
                   Unignore unignore);
    // Copy the edited values back into `out` (applied by the caller).
    void collect(Settings &out) const;
    void setConfirmIgnore(bool confirm);
    void refreshIgnoredCount();
    void setSaveHandler(std::function<bool()> handler) { saveHandler_ = std::move(handler); }
public slots:
    void accept() override;
private:
    void refreshThemes(const QString &selected);
    void updateThemeButtons();
    void editTheme(bool create);
    void deleteTheme();

    // one per bound widget: copies its value into the Settings (collect())
    QVector<std::function<void(Settings &)>> writers_;
    QJsonObject customThemes_;
    QComboBox *themeCombo_ = nullptr;
    QPushButton *newTheme_, *editTheme_, *deleteTheme_;
    QCheckBox *skipIgnoreConfirmation_ = nullptr;
    QLabel *ignoredCountLabel_ = nullptr;
    const IgnoreStore *ignoreStore_ = nullptr;
    Unignore unignore_;
    std::function<bool()> saveHandler_;
};

class IgnoreDialog : public QDialog {
    Q_OBJECT
public:
    IgnoreDialog(QWidget *parent, const IgnoreStore *store, SettingsDialog::Unignore unignore);

private:
    void refresh();
    void removeSelected();
    void clearAll();
    QTreeWidget *tree_ = nullptr;
    QLabel *countLabel_ = nullptr;
    const IgnoreStore *store_;
    SettingsDialog::Unignore unignore_;
};
