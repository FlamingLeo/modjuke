#include "songactions.h"

#include "config.h"
#include "ignorestore.h"
#include "musiclibrary.h"
#include "playliststore.h"
#include "queuebuilder.h"
#include "reveal.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSet>

SongActions::SongActions(QWidget *window, const Settings &settings, PlaylistStore &playlists,
                         const IgnoreStore &ignored, const MusicLibrary &library, QueueBuilder &builder)
    : window_(window), settings_(settings), playlists_(playlists), ignored_(ignored),
      library_(library), builder_(builder)
{
}

bool SongActions::toggleFavorite(const QString &path)
{
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
            playlists_.addPaths(name, paths);
            emit playlistsChanged();
            emit status(tr("Added %1 songs to \"%2\"").arg(paths.size()).arg(name));
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
    if (!playlists_.create(name, paths, library_.root())) {
        emit status(playlists_.error);
        return;
    }
    emit playlistsChanged();
    emit status(tr("Created \"%1\" with %2 songs").arg(name).arg(paths.size()));
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
            playlists_.addPaths(name, paths);
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
