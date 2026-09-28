// Isolated, draft-only visual editor for the shared 33-role theme format.
#pragma once
#include "theme.h"
#include <QDialog>
#include <QTreeWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVector>
#include <QTransform>

class ThemePreview : public QWidget {
    Q_OBJECT
public:
    explicit ThemePreview(QWidget *parent = nullptr);
    void setColors(const Palette &palette);
    void setSelectedRole(Palette::Role role);
    QSize sizeHint() const override { return QSize(520, 400); }
signals:
    void roleSelected(Palette::Role role);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    struct Hit { QRectF rect; Palette::Role role; };
    QVector<Hit> hits_;
    Palette palette_ = Palette::builtin(QStringLiteral("dark"));
    Palette::Role selected_ = Palette::BG;
    QTransform transform_;
};

class ThemeEditorDialog : public QDialog {
    Q_OBJECT
public:
    // id is a canonical custom ID. Existing entries, including id when editing,
    // are used to validate uniqueness without losing other definitions.
    ThemeEditorDialog(QWidget *parent, const QString &id, const QString &name,
                      const Palette &palette, const QJsonObject &existing);
    QJsonObject definition() const;
public slots:
    void accept() override;
private:
    void selectRole(Palette::Role role);
    void refreshColors();
    void validate();
    void chooseColor();
    void resetColors();
    QString id_;
    QJsonObject existing_;
    Palette initial_, palette_;
    Palette::Role role_ = Palette::BG;
    QTreeWidget *roles_;
    QTreeWidgetItem *items_[Palette::NUM_ROLES] = {};
    QLineEdit *name_, *hex_;
    QLabel *roleLabel_, *error_, *contrast_;
    QPushButton *use_;
    ThemePreview *preview_;
    bool valid_ = false;
};
