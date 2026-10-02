#include "playlistsdialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QVBoxLayout>

PlaylistsDialog::PlaylistsDialog(QWidget *parent, PlaylistStore *store,
                                 std::function<QStringList()> queuePaths)
    : QDialog(parent), store_(store), queuePaths_(std::move(queuePaths))
{
    setWindowTitle(tr("Playlists"));
    auto *root = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Name"), this));
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("new playlist name"));
    top->addWidget(nameEdit_, 1);
    createBtn_ = new QPushButton(tr("Create empty playlist"), this);
    connect(createBtn_, &QPushButton::clicked, this, &PlaylistsDialog::createEmpty);
    connect(nameEdit_, &QLineEdit::returnPressed, this, &PlaylistsDialog::createEmpty);
    top->addWidget(createBtn_);
    root->addLayout(top);

    auto *saveAsBtn = new QPushButton(tr("Save current queue as\u2026"), this);
    connect(saveAsBtn, &QPushButton::clicked, this, &PlaylistsDialog::saveAs);
    root->addWidget(saveAsBtn, 0, Qt::AlignLeft);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Playlists"), tr("Tracks")});
    tree_->setRootIsDecorated(false);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &PlaylistsDialog::updateButtons);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { loadSelected(); });
    root->addWidget(tree_, 1);

    auto *row1 = new QHBoxLayout;
    loadBtn_ = new QPushButton(tr("Load"), this);
    connect(loadBtn_, &QPushButton::clicked, this, &PlaylistsDialog::loadSelected);
    renameBtn_ = new QPushButton(tr("Rename"), this);
    connect(renameBtn_, &QPushButton::clicked, this, &PlaylistsDialog::renameSelected);
    deleteBtn_ = new QPushButton(tr("Delete"), this);
    connect(deleteBtn_, &QPushButton::clicked, this, &PlaylistsDialog::deleteSelected);
    exportBtn_ = new QPushButton(tr("Export M3U\u2026"), this);
    connect(exportBtn_, &QPushButton::clicked, this, &PlaylistsDialog::exportSelected);
    for (QPushButton *b : {loadBtn_, renameBtn_, deleteBtn_, exportBtn_})
        row1->addWidget(b);
    row1->addStretch();
    root->addLayout(row1);

    auto *row2 = new QHBoxLayout;
    addSelBtn_ = new QPushButton(tr("Add selected songs"), this);
    connect(addSelBtn_, &QPushButton::clicked, this, &PlaylistsDialog::addSelectedSongs);
    auto *addFilesBtn = new QPushButton(tr("Add files\u2026"), this);
    connect(addFilesBtn, &QPushButton::clicked, this, &PlaylistsDialog::addFiles);
    auto *importBtn = new QPushButton(tr("Import M3U\u2026"), this);
    connect(importBtn, &QPushButton::clicked, this, &PlaylistsDialog::importM3u);
    row2->addWidget(addSelBtn_);
    row2->addWidget(addFilesBtn);
    row2->addWidget(importBtn);
    row2->addStretch();
    root->addLayout(row2);

    noteLabel_ = new QLabel(this);
    noteLabel_->setWordWrap(true);
    root->addWidget(noteLabel_);

    refresh();
    resize(580, 440);
}

void PlaylistsDialog::refresh(const QString &selectName)
{
    tree_->clear();
    for (const QString &name : store_->names()) {
        const Playlist *pl = store_->get(name);
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, name);
        item->setText(1, QString::number(pl ? pl->paths.size() : 0));
        item->setData(0, Qt::UserRole, name);
        if (!selectName.isEmpty() && name.compare(selectName, Qt::CaseInsensitive) == 0)
            tree_->setCurrentItem(item);
    }
    updateButtons();
}

QString PlaylistsDialog::selectedName() const
{
    const auto items = tree_->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
}

void PlaylistsDialog::updateButtons()
{
    const QString name = selectedName();
    const bool has = !name.isEmpty();
    const bool fav = PlaylistStore::isFavorites(name);
    for (QPushButton *b : {loadBtn_, renameBtn_, deleteBtn_, exportBtn_, addSelBtn_})
        b->setEnabled(has);
    renameBtn_->setEnabled(has && !fav);
    deleteBtn_->setText(fav ? tr("Clear Favorites") : tr("Delete"));
    if (!has)
        noteLabel_->setText(tr("Pick a playlist to load, rename, or add songs to."));
    else if (fav)
        noteLabel_->setText(tr("Favorites is permanent. Add songs with \u2606 in the player or "
                               "the queue's right-click menu."));
    else {
        const Playlist *pl = store_->get(name);
        noteLabel_->setText(tr("%1 tracks \u00b7 saved order %2")
                                .arg(pl ? pl->paths.size() : 0)
                                .arg(pl && !pl->root.isEmpty()
                                         ? tr("root %1").arg(pl->root)
                                         : tr("has no root folder")));
    }
}

QString PlaylistsDialog::acceptName(const QString &text, const QString &emptyMessage)
{
    QString err;
    const QString name = PlaylistStore::normalizeName(text, &err);
    if (name.isEmpty())
        noteLabel_->setText(err.isEmpty() ? emptyMessage : err);
    return name;
}

void PlaylistsDialog::createEmpty()
{
    const QString name = acceptName(nameEdit_->text(), tr("Give the playlist a name first."));
    if (name.isEmpty())
        return;
    const bool ok = store_->create(name, {});
    const QString message = store_->error;
    nameEdit_->clear();
    refresh(name);
    if (!ok)
        noteLabel_->setText(message);   // after refresh, which rewrites the note
}

void PlaylistsDialog::addSelectedSongs()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const QStringList paths = queuePaths_();
    if (paths.isEmpty()) {
        noteLabel_->setText(tr("The queue is empty."));
        return;
    }
    const QString message = store_->addPaths(name, paths)
                                ? tr("Added %1 songs to \"%2\".").arg(paths.size()).arg(name)
                                : store_->error;
    refresh(name);
    noteLabel_->setText(message);
}

void PlaylistsDialog::addFiles()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add files to %1").arg(name), QString(),
        tr("Module files (*.mod *.xm *.it *.s3m *.mtm *.669 *.ult *.stm *.far *.gdm *.med);;"
           "All files (*)"));
    if (paths.isEmpty())
        return;
    const QString message = store_->addPaths(name, paths)
                                ? tr("Added %1 files to \"%2\".").arg(paths.size()).arg(name)
                                : store_->error;
    refresh(name);
    noteLabel_->setText(message);
}

void PlaylistsDialog::saveAs()
{
    QString suggested = nameEdit_->text();
    if (suggested.trimmed().isEmpty()) {
        bool ok = false;
        suggested = QInputDialog::getText(this, tr("Save queue as playlist"), tr("Playlist name:"),
                                          QLineEdit::Normal, QStringLiteral("Playlist"), &ok);
        if (!ok)
            return;
    }
    const QString name = acceptName(suggested, tr("Invalid playlist name."));
    if (name.isEmpty())
        return;
    emit saveQueueRequested(name);
    refresh(name);
}

void PlaylistsDialog::renameSelected()
{
    const QString oldName = selectedName();
    if (oldName.isEmpty())
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Rename playlist"), tr("New name:"),
                                               QLineEdit::Normal, oldName, &ok);
    if (!ok)
        return;
    const QString name = acceptName(text, tr("Invalid playlist name."));
    if (name.isEmpty())
        return;
    if (!store_->rename(oldName, name)) {
        noteLabel_->setText(store_->error);
        return;
    }
    emit renamed(oldName, name);
    refresh(name);
}

void PlaylistsDialog::deleteSelected()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    if (PlaylistStore::isFavorites(name)) {
        if (QMessageBox::question(this, tr("Clear Favorites"),
                tr("Remove all songs from Favorites? Your music files will not be deleted."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        store_->clearFavorites();
        refresh(name);
        return;
    }
    if (QMessageBox::question(this, tr("Delete playlist"),
                              tr("Delete \"%1\"? Songs are only removed from the playlist.")
                                  .arg(name))
        != QMessageBox::Yes)
        return;
    if (!store_->remove(name)) {
        noteLabel_->setText(store_->error);
        return;
    }
    emit removed(name);
    refresh();
}

void PlaylistsDialog::loadSelected()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    emit loadRequested(name, false);   // Load keeps the order choice (README)
    accept();
}

void PlaylistsDialog::exportSelected()
{
    const QString name = selectedName();
    const Playlist *pl = store_->get(name);
    if (!pl)
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export playlist"),
                                                      name + QStringLiteral(".m3u"),
                                                      tr("M3U playlist (*.m3u)"));
    if (path.isEmpty())
        return;
    const int written = writeM3u(pl->paths, path);
    if (written < 0)
        noteLabel_->setText(tr("Could not write %1").arg(QFileInfo(path).fileName()));
    else
        noteLabel_->setText(tr("Exported %1 of %2 entries to %3")
                                .arg(written).arg(pl->paths.size()).arg(QFileInfo(path).fileName()));
}

void PlaylistsDialog::importM3u()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import M3U"), QString(),
                                                      tr("M3U playlist (*.m3u *.m3u8);;All files (*)"));
    if (path.isEmpty())
        return;
    int skipped = 0;
    const QStringList entries = readM3u(path, &skipped);
    if (entries.isEmpty()) {
        noteLabel_->setText(tr("That M3U had no readable files."));
        return;
    }
    const QString base = QFileInfo(path).completeBaseName();
    QString name = store_->uniqueName(base, QStringLiteral(" (%1)"));
    if (name.isEmpty())
        name = store_->uniqueName(QStringLiteral("Imported"), QStringLiteral(" (%1)"));
    const bool ok = !name.isEmpty() && store_->create(name, entries, QFileInfo(path).absolutePath());
    const QString message = ok ? tr("Imported %1 tracks (%2 missing skipped) as \"%3\"")
                                     .arg(entries.size()).arg(skipped).arg(name)
                               : store_->error;
    refresh(name);
    noteLabel_->setText(message);
    if (ok)
        emit loadRequested(name, true);   // an imported M3U opens in Saved order (README)
}
