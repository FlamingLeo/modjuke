// The playlist manager (port of playlists.py).
#pragma once

#include "playliststore.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <functional>

class PlaylistsDialog : public QDialog {
    Q_OBJECT
public:
    // queuePaths: the songs "Add selected songs" adds (asked when clicked)
    PlaylistsDialog(QWidget *parent, PlaylistStore *store, std::function<QStringList()> queuePaths);

signals:
    void loadRequested(const QString &name, bool savedOrder);   // make it the queue source
    void saveQueueRequested(const QString &name);
    void renamed(const QString &oldName, const QString &newName);
    void removed(const QString &name);

private:
    void refresh(const QString &selectName = QString());
    QString selectedName() const;
    void createEmpty();
    void addSelectedSongs();
    void addFiles();
    void saveAs();
    void renameSelected();
    void deleteSelected();
    void loadSelected();
    void exportSelected();
    void importM3u();
    void updateButtons();
    QString acceptName(const QString &text, const QString &emptyMessage);   // "" + note when invalid

    PlaylistStore *store_;
    std::function<QStringList()> queuePaths_;

    QTreeWidget *tree_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QPushButton *createBtn_ = nullptr;
    QPushButton *loadBtn_ = nullptr;
    QPushButton *renameBtn_ = nullptr;
    QPushButton *deleteBtn_ = nullptr;
    QPushButton *exportBtn_ = nullptr;
    QPushButton *addSelBtn_ = nullptr;
    QLabel *noteLabel_ = nullptr;
};
