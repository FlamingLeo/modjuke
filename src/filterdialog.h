// The queue filter dialog (port of filters.py FiltersDialog).
#pragma once

#include "library.h"

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

class FilterDialog : public QDialog {
    Q_OBJECT
public:
    FilterDialog(QWidget *parent, const QueueFilter &current, const QVector<Track> &tracks);
    QueueFilter criteria() const;

private slots:
    void clear();

private:
    void updatePreview();
    QVector<Track> tracks_;
    QList<QCheckBox *> formatChecks_;
    QLineEdit *minEdit_ = nullptr;
    QLineEdit *maxEdit_ = nullptr;
    QCheckBox *hideBroken_ = nullptr;
    QLabel *countLabel_ = nullptr;
    QPushButton *applyBtn_ = nullptr;
};
