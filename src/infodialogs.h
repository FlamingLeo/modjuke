// Read-mostly dialogs: song info (samples / instruments), listening stats and
// about. (Ports of songinfo.py and statsview.py.)
#pragma once

#include "openmptapi.h"
#include "statsstore.h"

#include <QDialog>
#include <QLabel>
#include <QTreeWidget>

class SongInfoDialog : public QDialog {
    Q_OBJECT
public:
    SongInfoDialog(QWidget *parent, const ModuleInfo &info);
};

class StatsDialog : public QDialog {
    Q_OBJECT
public:
    StatsDialog(QWidget *parent, StatsStore *store);

signals:
    void statsReset();

private:
    void refresh();
    QTreeWidget *tree_ = nullptr;
    QLabel *totalLabel_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    StatsStore *store_;
};

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    AboutDialog(QWidget *parent, const QString &libopenmptVersion, bool libLoaded);
};
