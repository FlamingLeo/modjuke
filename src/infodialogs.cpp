#include "infodialogs.h"

#include "library.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>
#include <algorithm>

SongInfoDialog::SongInfoDialog(QWidget *parent, const ModuleInfo &info) : QDialog(parent)
{
    setWindowTitle(tr("Song info - %1").arg(info.title.isEmpty() ? info.path : info.title));
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("<h3>%1</h3>").arg(info.title.toHtmlEscaped()), this));
    QString summary = tr("%1, %2 samples, %3 instruments")
                          .arg(info.formatLong.isEmpty() ? info.format : info.formatLong)
                          .arg(info.samples).arg(info.instruments);
    if (!info.tracker.isEmpty())
        summary += tr(" \u00b7 %1").arg(info.tracker);
    root->addWidget(new QLabel(summary, this));

    auto *tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Samples"), tr("Instruments")});
    tree->setRootIsDecorated(false);
    const int rows = std::max(info.sampleNames.size(), info.instrumentNames.size());
    for (int i = 0; i < rows; ++i) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, i < info.sampleNames.size()
                             ? QStringLiteral("%1. %2").arg(i + 1)
                                   .arg(info.sampleNames.at(i).trimmed())
                             : QString());
        item->setText(1, i < info.instrumentNames.size()
                             ? QStringLiteral("%1. %2").arg(i + 1)
                                   .arg(info.instrumentNames.at(i).trimmed())
                             : QString());
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(tree, 1);

    if (!info.message.isEmpty()) {
        auto *message = new QPlainTextEdit(info.message, this);
        message->setReadOnly(true);
        message->setMaximumHeight(140);
        root->addWidget(message);
    }
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    resize(640, 460);
}

StatsDialog::StatsDialog(QWidget *parent, StatsStore *store) : QDialog(parent), store_(store)
{
    setWindowTitle(tr("Listening stats"));
    auto *root = new QVBoxLayout(this);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Module"), tr("Plays"), tr("Time"), tr("Last played")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    root->addWidget(tree_, 1);
    totalLabel_ = new QLabel(this);
    root->addWidget(totalLabel_);
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);
    auto *buttons = new QHBoxLayout;
    auto *clearBtn = new QPushButton(tr("Clear stats"), this);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clear stats"), tr("Forget all listening history?"))
            == QMessageBox::Yes) {
            store_->resetAll();
            store_->save();
            refresh();
            if (store_->error.isEmpty())
                emit statsReset();
        }
    });
    buttons->addWidget(clearBtn);
    buttons->addStretch();
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);
    refresh();
    resize(660, 420);
}

void StatsDialog::refresh()
{
    tree_->clear();
    for (const ModuleStats &row : store_->mostPlayed(500)) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, row.title.isEmpty() ? QFileInfo(row.path).fileName() : row.title);
        item->setToolTip(0, row.path);
        item->setText(1, QString::number(row.plays));
        item->setText(2, formatTime(row.seconds));
        item->setText(3, row.lastPlayed > 0
                             ? QDateTime::fromSecsSinceEpoch(qint64(row.lastPlayed))
                                   .toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                             : QStringLiteral("-"));
    }
    totalLabel_->setText(tr("%1 modules, %2 total listening time")
                             .arg(store_->count()).arg(formatTime(store_->totalSeconds())));
    errorLabel_->setText(store_->error);
    errorLabel_->setVisible(!store_->error.isEmpty());
}

AboutDialog::AboutDialog(QWidget *parent, const QString &libopenmptVersion, bool libLoaded)
    : QDialog(parent)
{
    setWindowTitle(tr("About modjuke"));
    auto *root = new QVBoxLayout(this);
    auto *aboutLabel = new QLabel(tr("Made by FlamingLeo, 2026.<br/>"
                                     "Plays MOD, XM, IT, S3M and friends "
                                     "through libopenmpt.<br/>"
                                     "Project page: <a href=\"https://github.com/FlamingLeo/modjuke\">"
                                     "github.com/FlamingLeo/modjuke</a>"),
                                  this);
    aboutLabel->setTextFormat(Qt::RichText);
    aboutLabel->setOpenExternalLinks(true);
    aboutLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    aboutLabel->setWordWrap(true);
    root->addWidget(aboutLabel);
    const QString qtVersion = QString::fromLatin1(qVersion());
    const QString version = tr("modjuke %1 (MIT License)<br/>").arg(QStringLiteral(MODJUKE_VERSION));
    auto *libLabel = new QLabel(
        version + (libLoaded ? tr("libopenmpt %1<br/>Qt %2").arg(libopenmptVersion, qtVersion)
                             : tr("libopenmpt was not found - install libopenmpt0<br/>"
                                  "or set MODJUKE_LIBOPENMPT.<br/>Qt %1").arg(qtVersion)),
        this);
    libLabel->setTextFormat(Qt::RichText);
    libLabel->setWordWrap(true);
    root->addWidget(libLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    resize(430, 190);
}
