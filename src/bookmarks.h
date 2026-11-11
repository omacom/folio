#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <optional>
#include <vector>

// The bookmarks bar, as in a browser: bookmarks and folders of bookmarks,
// any depth, in the order they were put there. A bookmark's path is its
// URL: a folder, a file, or anything the path bar takes (~ for home, a
// file:// URI). Its name is its own, apart from the file's.
//
// Kept in ~/.config/<app>/bookmarks.json, which every window's process
// shares. Each change is made under a lock to whatever the file holds at
// that moment, so two windows never undo each other's changes, and is
// written atomically. Every item has an id stored in the file, so an id
// held by an open dialog still means the same item after another window's
// change. A file that can't be read, or isn't laid out as bookmarks, is
// left alone: nothing is saved over it until it's fixed or removed.
class Bookmarks : public QObject {
    Q_OBJECT
    // The tree, as maps: {id, type: "bookmark"|"folder", name, path, kind,
    // children}. `kind` is what the path is right now: "folder", "file" or
    // "missing" (bookmarks only).
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    // Every folder, depth first, for choosing where a bookmark goes; the bar
    // itself comes first, with an empty id.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY changed)
    // Why the file couldn't be read or written; empty when all is well.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

public:
    // `file` and `home` are overridable for tests.
    explicit Bookmarks(const QString &file = {}, const QString &home = {}, QObject *parent = nullptr);

    QVariantList items() const;
    QVariantList folders() const;
    QString problem() const { return m_problem; }

    // Each returns the new item's id, or empty when it couldn't be made.
    // `parentId` empty is the bar; `index` -1 is the end.
    Q_INVOKABLE QString add(const QString &parentId, const QString &name, const QString &path, int index = -1);
    Q_INVOKABLE QString addFolder(const QString &parentId, const QString &name, int index = -1);
    // Adds each local file or folder of a drop (file:// URIs), named after
    // it, all in one save; returns how many were added.
    Q_INVOKABLE int addUrls(const QString &parentId, const QList<QUrl> &urls, int index = -1);
    // Renames, points a bookmark elsewhere (a folder has no path) and, when
    // `parentId` isn't where it is, moves it to the end of that folder. All
    // or nothing: returns empty when done, or why not, to show as it is.
    // Partial: an empty name or path, or `parentId` left out (undefined in
    // QML), keeps what's there now, under the lock, so a field the user
    // didn't touch never undoes another window's change to it.
    Q_INVOKABLE QString edit(const QString &id, const QString &name, const QString &path,
                             const QVariant &parentId = QVariant());
    // Refuses to put a folder inside itself.
    Q_INVOKABLE bool move(const QString &id, const QString &parentId, int index = -1);
    Q_INVOKABLE bool remove(const QString &id);
    // The item as items() has it, plus its parentId; empty if it's gone.
    Q_INVOKABLE QVariantMap get(const QString &id) const;
    // Whether `id` is `ancestorId` or anywhere inside it; everything is
    // inside the bar ("").
    Q_INVOKABLE bool isWithin(const QString &id, const QString &ancestorId) const;
    // The first bookmark going to `path`, or empty.
    Q_INVOKABLE QString find(const QString &path) const;
    // How a path is stored: absolute, with home as ~. Anything that isn't a
    // local path ("recent:", "sftp://…") is kept as written.
    Q_INVOKABLE QString normalize(const QString &path) const;
    // Where a stored path goes: ~ expanded, ready for Backend.
    Q_INVOKABLE QString resolve(const QString &path) const;
    // A path as typed in a dialog, as the path bar takes it: a relative one
    // is inside `base` (the folder being shown, ~ allowed); anything else is
    // as written.
    Q_INVOKABLE QString absolute(const QString &path, const QString &base) const;
    // Tab in the editor's path: completes the last part to a folder name,
    // relative paths taken from `base` as absolute() takes them, so what's
    // completed is what's saved.
    Q_INVOKABLE QString completePath(const QString &text, const QString &base) const;
    // A name for a new bookmark: the file's or folder's own.
    Q_INVOKABLE QString defaultName(const QString &path) const;
    // Re-reads which bookmarks go to folders, files or nothing.
    Q_INVOKABLE void refresh();

signals:
    // Once per change saved, per change found on disk, and per failure to
    // read or save; never for a change that wasn't saved.
    void changed();

private:
    struct Node {
        QString id;
        bool folder = false;
        QString name;
        QString path;
        QString kind;
        std::vector<Node> children;
        // Keys this version doesn't know, written back as they were.
        QJsonObject extra;
    };
    using Tree = std::vector<Node>;
    // A change to a copy of the tree: empty when made, else why not.
    using Change = std::function<QString(Tree &)>;

    void load();
    bool sync();
    bool parse(const QByteArray &bytes, Tree &tree, QJsonObject &extra, QString &reason) const;
    QByteArray serialize(const Tree &tree) const;
    QString transact(const Change &change);
    // edit(), with no move when `parentId` is unset.
    QString editItem(const QString &id, const QString &name, const QString &path,
                     const std::optional<QString> &parentId);
    QVariantMap mapOf(const Node &node) const;
    static QString newId(const Tree &tree);
    static Node *findIn(Tree &tree, const QString &id, Tree **siblings = nullptr, Node **parent = nullptr);
    static const Node *findIn(const Tree &tree, const QString &id, const Node **parent = nullptr);
    static Tree *childrenIn(Tree &tree, const QString &parentId);
    static bool within(const Tree &tree, const QString &id, const QString &ancestorId);
    static QString insertIn(Tree &tree, const QString &parentId, std::vector<Node> nodes, int index);
    static QString moveIn(Tree &tree, const QString &id, const QString &parentId, int index);
    QString kindOf(const QString &path) const;
    bool refreshKinds(Tree &nodes);
    void watch();

    QString m_file;
    QString m_home;
    Tree m_root;
    // The file's own keys besides the bookmarks, kept for the same reason.
    QJsonObject m_extra;
    QString m_problem;
    // What was last read or written, to tell our own saves from others'.
    QByteArray m_bytes;
    bool m_loaded = false;
    // Whether the file was there at the last read: an empty file isn't no file.
    bool m_existed = false;
    // The file couldn't be read or isn't bookmarks: it's never saved over.
    bool m_broken = false;
    QFileSystemWatcher m_watcher;
};
