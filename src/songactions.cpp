#include "songactions.h"

#include "config.h"
#include "ignorestore.h"
#include "musiclibrary.h"
#include "playliststore.h"
#include "queuebuilder.h"
#include "reveal.h"
#include "songkeys.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>

int askAboutCopies(QWidget *parent, SongKeys &keys, const QString &listName,
                   const QStringList &existing, QStringList *adding)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    keys.prepareNow(existing + *adding);
    QApplication::restoreOverrideCursor();
    const QVector<QPair<QString, QString>> copies = copiesOf(existing, *adding, keys.keyOf());
    if (copies.isEmpty())
        return 0;
    const auto name = [](const QString &path) { return QFileInfo(path).fileName(); };
    QMessageBox box(QMessageBox::Question, QObject::tr("Duplicate songs"), QString(),
                    QMessageBox::NoButton, parent);
    if (adding->size() == 1) {
        box.setText(QObject::tr("\"%1\" is already in \"%2\" as \"%3\".")
                        .arg(name(copies[0].first), listName, name(copies[0].second)));
    } else {
        box.setText(QObject::tr("%n of these songs would be in \"%1\" twice, as different files.",
                                nullptr, int(copies.size()))
                        .arg(listName));
        QStringList lines, details;
        for (const auto &[path, copy] : copies) {
            if (lines.size() < 10)
                lines << QObject::tr("%1 (copy of %2)").arg(name(path), name(copy));
            details << QObject::tr("%1\n    copy of %2").arg(path, copy);
        }
        if (copies.size() > lines.size())
            lines << QObject::tr("and %n more", nullptr, int(copies.size() - lines.size()));
        box.setInformativeText(lines.join('\n'));
        box.setDetailedText(details.join('\n'));
    }
    QPushButton *add = box.addButton(QObject::tr("Add anyway"), QMessageBox::AcceptRole);
    QPushButton *skip = box.addButton(adding->size() == 1 ? QObject::tr("Don't add")
                                                          : QObject::tr("Skip copies"),
                                      QMessageBox::RejectRole);
    box.setDefaultButton(skip);
    box.setEscapeButton(skip);
    box.exec();
    if (box.clickedButton() == add)
        return 0;
    QSet<QString> left;
    for (const auto &pair : copies)
        left.insert(pair.first);
    adding->removeIf([&left](const QString &path) { return left.contains(path); });
    return int(left.size());
}

SongActions::SongActions(QWidget *window, const Settings &settings, PlaylistStore &playlists,
                         const IgnoreStore &ignored, const MusicLibrary &library, QueueBuilder &builder,
                         SongKeys &keys)
    : window_(window), settings_(settings), playlists_(playlists), ignored_(ignored),
      library_(library), builder_(builder), keys_(keys)
{
}

QStringList SongActions::pathsOf(const QString &playlist) const
{
    const Playlist *found = playlists_.get(playlist);
    return found ? found->paths : QStringList();
}

bool SongActions::toggleFavorite(const QString &path)
{
    if (!playlists_.isFavorite(path)) {
        const QString favorites = QString::fromLatin1(PlaylistStore::kFavoritesName);
        QStringList adding{path};
        if (askAboutCopies(window_, keys_, favorites, pathsOf(favorites), &adding) > 0)
            return false;
    }
    const bool on = playlists_.toggleFavorite(path);
    emit playlistsChanged();
    if (PlaylistStore::isFavorites(settings_.queueSource))
        emit queueChanged();
    return on;
}

void SongActions::showAddMenu(const QStringList &paths)
{
    if (paths.isEmpty()) {
        emit status(tr("Select songs in the queue first"));
        return;
    }
    QMenu menu(window_);
    for (const QString &name : playlists_.names()) {
        menu.addAction(name, [this, name, paths] {
            QStringList adding = paths;
            const int left = askAboutCopies(window_, keys_, name, pathsOf(name), &adding);
            playlists_.addPaths(name, adding);
            emit playlistsChanged();
            emit status(left > 0 ? tr("Added %1 songs to \"%2\", left out %3 copies")
                                       .arg(adding.size()).arg(name).arg(left)
                                 : tr("Added %1 songs to \"%2\"").arg(adding.size()).arg(name));
        });
    }
    menu.addSeparator();
    menu.addAction(tr("New playlist from selection\u2026"), [this, paths] { createPlaylist(paths); });
    menu.exec(QCursor::pos());
}

void SongActions::createPlaylist(const QStringList &paths)
{
    bool ok = false;
    const QString base = QInputDialog::getText(window_, tr("New playlist"), tr("Playlist name:"),
                                               QLineEdit::Normal, QStringLiteral("Playlist"), &ok);
    if (!ok)
        return;
    QString err;
    const QString name = playlists_.uniqueName(base, QStringLiteral(" %1"), &err);
    if (name.isEmpty()) {
        emit status(err.isEmpty() ? tr("Invalid playlist name") : err);
        return;
    }
    QStringList adding = paths;
    askAboutCopies(window_, keys_, name, {}, &adding);
    if (!playlists_.create(name, adding, library_.root())) {
        emit status(playlists_.error);
        return;
    }
    emit playlistsChanged();
    emit status(tr("Created \"%1\" with %2 songs").arg(name).arg(adding.size()));
}

void SongActions::removeFromSource(const QStringList &paths)
{
    if (settings_.queueSource.isEmpty())
        return;
    if (paths.isEmpty()) {
        emit status(tr("Select songs to remove"));
        return;
    }
    playlists_.removePaths(settings_.queueSource, QSet<QString>(paths.begin(), paths.end()));
    emit queueChanged();
    emit status(tr("Removed %1 songs from \"%2\"").arg(paths.size()).arg(settings_.queueSource));
}

void SongActions::saveQueueAs(const QString &name, const QStringList &paths)
{
    const bool exists = playlists_.has(name);
    if (exists && QMessageBox::question(
                      window_, tr("Save queue as playlist"),
                      tr("Replace the songs of \"%1\" with the current queue (%2 songs)?")
                          .arg(name).arg(paths.size()))
                      != QMessageBox::Yes)
        return;
    const bool ok = exists ? playlists_.replace(name, paths)
                           : playlists_.create(name, paths, library_.root());
    if (!ok) {
        emit status(playlists_.error);
        return;
    }
    if (!exists)   // a new playlist opens in Saved order (README)
        builder_.setSource(name, true);
    emit playlistsChanged();
    emit queueChanged();
    emit status(tr("Saved the queue as \"%1\" (%2 songs)").arg(name).arg(paths.size()));
}

void SongActions::showContextMenu(const QPoint &globalPos, const QString &path,
                                  const QStringList &selected)
{
    // Directory headers are structural rows, not songs. Do not offer song
    // actions (especially Add to playlist) for them.
    if (path.isEmpty())
        return;
    QMenu menu(window_);
    menu.addAction(tr("Play now"), [this, path] { emit playRequested(path); });
    menu.addSeparator();
    menu.addAction(ignored_.contains(path) ? tr("Un-ignore this file") : tr("Ignore this file"),
                   [this, path] { emit ignoreToggled(path); });
    menu.addAction(tr("Toggle favorite"), [this, path] { toggleFavorite(path); });
    QMenu *addTo = menu.addMenu(tr("Add to playlist"));
    const QStringList paths = selected.isEmpty() ? QStringList{path} : selected;
    for (const QString &name : playlists_.names()) {
        if (PlaylistStore::isFavorites(name))
            continue;
        addTo->addAction(name, [this, name, paths] {
            QStringList adding = paths;
            askAboutCopies(window_, keys_, name, pathsOf(name), &adding);
            playlists_.addPaths(name, adding);
            emit playlistsChanged();
        });
    }
    addTo->addAction(tr("New playlist\u2026"), [this, paths] { createPlaylist(paths); });
    menu.addAction(tr("Copy path"), [path] { QApplication::clipboard()->setText(path); });
    menu.addSeparator();
    menu.addAction(tr("Show in folder"), [this, path] { reveal(path); });
    menu.exec(globalPos);
}

void SongActions::reveal(const QString &path)
{
    revealFile(path, window_, [this](bool, bool, const QString &message) { emit status(message); });
}
