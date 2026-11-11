#include "bookmarks.h"
#include "backend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace {

// A local path is absolute, under home (~), or a file:// URI; anything else
// with a scheme ("recent:", "sftp://host/…") is a place of its own.
bool isLocal(const QString &path) {
    if (path.startsWith('/') || path == "~" || path.startsWith("~/") || path.startsWith("file://"))
        return true;
    // A scheme is letters (then letters, digits, + - .) before the first ':',
    // with no '/' in front of it: "recent:", "sftp://host". A colon later in
    // a relative path ("notes/report:2026") is part of a file name.
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    return !scheme.match(path).hasMatch();
}

} // namespace

Bookmarks::Bookmarks(const QString &file, const QString &home, QObject *parent)
    : QObject(parent), m_home(QDir::cleanPath(home.isEmpty() ? QDir::homePath() : home)) {
    m_file = file.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/"
              + QCoreApplication::organizationName() + "/bookmarks.json"
        : file;
    // Another process (a second window started from the launcher) saving
    // its bookmarks. Saves are atomic renames, which a watch on the file
    // alone loses, so the folder is watched too.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &Bookmarks::load);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Bookmarks::load);
    load();
}

QString Bookmarks::newId(const Tree &tree) {
    for (;;) {
        const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
        if (!findIn(tree, id))
            return id;
    }
}

void Bookmarks::watch() {
    // Made up front, so another process's first save is noticed too.
    const QString dir = QFileInfo(m_file).absolutePath();
    QDir().mkpath(dir);
    if (QFileInfo(dir).isDir() && !m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(m_file) && !m_watcher.files().contains(m_file))
        m_watcher.addPath(m_file);
}

void Bookmarks::load() {
    if (sync())
        emit changed();
}

bool Bookmarks::sync() {
    watch();
    QByteArray bytes;
    QFile file(m_file);
    const bool exists = file.open(QIODevice::ReadOnly);
    if (exists) {
        bytes = file.readAll();
    } else if (file.exists()) {
        const QString problem = tr("Couldn't read the bookmarks in %1.").arg(m_file);
        const bool news = !m_broken || m_problem != problem;
        m_problem = problem;
        m_broken = true;
        // Read afresh once it can be, whatever it then holds.
        m_loaded = false;
        return news;
    }
    // Our own save coming back, or another change in the folder.
    if (m_loaded && bytes == m_bytes && exists == m_existed)
        return false;
    m_loaded = true;
    m_bytes = bytes;
    m_existed = exists;
    Tree tree;
    QJsonObject extra;
    QString reason;
    // No file is no bookmarks yet; a file that's there but empty is a
    // file something went wrong with, kept as it is like any other.
    const bool empty = exists && bytes.trimmed().isEmpty();
    if (empty)
        reason = tr("the file is empty");
    if (empty || !parse(bytes, tree, extra, reason)) {
        // Left as it is, and never saved over: the bar stays empty and
        // says why until the file is fixed.
        m_root.clear();
        m_extra = {};
        m_broken = true;
        m_problem = tr("Couldn't read the bookmarks in %1: %2.").arg(m_file, reason);
        return true;
    }
    m_root = std::move(tree);
    m_extra = extra;
    m_broken = false;
    m_problem.clear();
    return true;
}

bool Bookmarks::parse(const QByteArray &bytes, Tree &tree, QJsonObject &extra, QString &reason) const {
    tree.clear();
    extra = {};
    if (bytes.trimmed().isEmpty())
        return true;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        reason = error.errorString();
        return false;
    }
    if (!doc.isObject()) {
        reason = tr("not a bookmarks file");
        return false;
    }
    extra = doc.object();
    const QJsonValue version = extra.take("version");
    if (!version.isUndefined() && !(version.isDouble() && version.toDouble() == 1)) {
        reason = tr("it's version %1, which this app doesn't know").arg(version.toVariant().toString());
        return false;
    }
    const QJsonValue bookmarks = extra.take("bookmarks");
    if (!bookmarks.isArray()) {
        reason = tr("it has no list of bookmarks");
        return false;
    }

    // Anything that isn't plainly a bookmark or a folder stops the whole
    // file being read: dropping it would lose it at the next save.
    QSet<QString> seen;
    std::function<bool(const QJsonArray &, Tree &, const QString &, const QString &)> read =
        [&](const QJsonArray &array, Tree &nodes, const QString &folder, const QString &position) {
            for (qsizetype i = 0; i < array.size(); ++i) {
                const QString entry = folder.isNull()
                    ? tr("entry %1").arg(i + 1)
                    : tr("entry %1 in “%2”").arg(QString::number(i + 1), folder);
                if (!array.at(i).isObject()) {
                    reason = tr("%1 isn't a bookmark or a folder").arg(entry);
                    return false;
                }
                QJsonObject o = array.at(i).toObject();
                const QJsonValue id = o.take("id");
                const QJsonValue name = o.take("name");
                const QJsonValue path = o.take("path");
                const QJsonValue children = o.take("children");
                if (!name.isString()) {
                    reason = tr("%1 has no name").arg(entry);
                    return false;
                }
                if (!id.isUndefined() && !id.isString()) {
                    reason = tr("%1 has an id that isn't text").arg(entry);
                    return false;
                }
                if (!path.isUndefined() && !children.isUndefined()) {
                    reason = tr("%1 is both a bookmark and a folder").arg(entry);
                    return false;
                }
                if (!children.isUndefined() && !children.isArray()) {
                    reason = tr("%1 has children that aren't a list").arg(entry);
                    return false;
                }
                if (children.isUndefined() && (!path.isString() || path.toString().isEmpty())) {
                    reason = tr("%1 has no path").arg(entry);
                    return false;
                }

                Node node;
                node.extra = o;
                node.name = name.toString();
                node.id = id.toString();
                const QString here = position + "/" + QString::number(i);
                // Given an id from where it is, so every window reading the
                // same file before it's next saved agrees on it; one copied
                // by hand along with its bookmark gets the same treatment.
                for (int salt = 0; node.id.isEmpty() || seen.contains(node.id); ++salt) {
                    const QString key = here + "\n" + node.name + "\n" + path.toString() + "\n" + QString::number(salt);
                    node.id = QString::fromLatin1(
                        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
                }
                seen.insert(node.id);
                if (children.isArray()) {
                    node.folder = true;
                    if (!read(children.toArray(), node.children, node.name, here))
                        return false;
                } else {
                    node.path = path.toString();
                    if (node.name.isEmpty())
                        node.name = defaultName(node.path);
                    node.kind = kindOf(node.path);
                }
                nodes.push_back(std::move(node));
            }
            return true;
        };
    if (!read(bookmarks.toArray(), tree, QString(), QString())) {
        tree.clear();
        extra = {};
        return false;
    }
    return true;
}

QByteArray Bookmarks::serialize(const Tree &tree) const {
    std::function<QJsonArray(const Tree &)> write = [&](const Tree &nodes) {
        QJsonArray array;
        for (const Node &node : nodes) {
            QJsonObject o = node.extra;
            o.insert("id", node.id);
            o.insert("name", node.name);
            if (node.folder)
                o.insert("children", write(node.children));
            else
                o.insert("path", node.path);
            array.append(o);
        }
        return array;
    };
    QJsonObject top = m_extra;
    top.insert("version", 1);
    top.insert("bookmarks", write(tree));
    return QJsonDocument(top).toJson();
}

QString Bookmarks::transact(const Change &change) {
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    // Held from reading the file to writing it, so another window's change
    // can't land in between and be lost.
    QLockFile lock(m_file + ".lock");
    if (!lock.tryLock(2000)) {
        m_problem = lock.error() == QLockFile::LockFailedError
            ? tr("Couldn't save the bookmarks to %1: another window is still saving them.").arg(m_file)
            : tr("Couldn't save the bookmarks to %1.").arg(m_file);
        emit changed();
        return m_problem;
    }
    // The change is made to what's on disk now, not to what was last seen,
    // which the watcher may not have caught up with.
    const bool fresh = sync();
    if (m_broken) {
        emit changed();
        return m_problem;
    }
    Tree tree = m_root;
    const QString refused = change(tree);
    if (!refused.isEmpty()) {
        if (fresh)
            emit changed();
        return refused;
    }
    const QByteArray bytes = serialize(tree);
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        // What's shown stays what's on disk.
        m_problem = tr("Couldn't save the bookmarks to %1.").arg(m_file);
        emit changed();
        return m_problem;
    }
    m_root = std::move(tree);
    m_bytes = bytes;
    m_loaded = true;
    m_problem.clear();
    watch();
    emit changed();
    return {};
}

QVariantMap Bookmarks::mapOf(const Node &node) const {
    QVariantMap map{{"id", node.id}, {"type", node.folder ? "folder" : "bookmark"}, {"name", node.name}};
    if (node.folder) {
        QVariantList children;
        for (const Node &child : node.children)
            children << mapOf(child);
        map.insert("children", children);
    } else {
        map.insert({{"path", node.path}, {"kind", node.kind}});
    }
    return map;
}

QVariantList Bookmarks::items() const {
    QVariantList out;
    for (const Node &node : m_root)
        out << mapOf(node);
    return out;
}

QVariantList Bookmarks::folders() const {
    QVariantList out{QVariantMap{{"id", QString()}, {"name", tr("Bookmarks Bar")}, {"depth", 0}}};
    std::function<void(const Tree &, int)> walk = [&](const Tree &nodes, int depth) {
        for (const Node &node : nodes) {
            if (!node.folder)
                continue;
            out << QVariantMap{{"id", node.id}, {"name", node.name}, {"depth", depth}};
            walk(node.children, depth + 1);
        }
    };
    walk(m_root, 1);
    return out;
}

Bookmarks::Node *Bookmarks::findIn(Tree &tree, const QString &id, Tree **siblings, Node **parent) {
    std::function<Node *(Tree &, Node *)> walk = [&](Tree &nodes, Node *owner) -> Node * {
        for (Node &node : nodes) {
            if (node.id == id) {
                if (siblings)
                    *siblings = &nodes;
                if (parent)
                    *parent = owner;
                return &node;
            }
            if (Node *found = walk(node.children, &node))
                return found;
        }
        return nullptr;
    };
    return id.isEmpty() ? nullptr : walk(tree, nullptr);
}

const Bookmarks::Node *Bookmarks::findIn(const Tree &tree, const QString &id, const Node **parent) {
    Node *owner = nullptr;
    const Node *found = findIn(const_cast<Tree &>(tree), id, nullptr, &owner);
    if (parent)
        *parent = owner;
    return found;
}

Bookmarks::Tree *Bookmarks::childrenIn(Tree &tree, const QString &parentId) {
    if (parentId.isEmpty())
        return &tree;
    Node *node = findIn(tree, parentId);
    return node && node->folder ? &node->children : nullptr;
}

bool Bookmarks::within(const Tree &tree, const QString &id, const QString &ancestorId) {
    if (id == ancestorId)
        return true;
    for (QString up = id; !up.isEmpty();) {
        if (up == ancestorId)
            return true;
        const Node *parent = nullptr;
        if (!findIn(tree, up, &parent))
            return false;
        up = parent ? parent->id : QString();
    }
    return ancestorId.isEmpty();
}

QString Bookmarks::insertIn(Tree &tree, const QString &parentId, std::vector<Node> nodes, int index) {
    Tree *into = childrenIn(tree, parentId);
    if (!into)
        return tr("That folder is gone.");
    int at = index < 0 || index > int(into->size()) ? int(into->size()) : index;
    for (Node &node : nodes)
        into->insert(into->begin() + at++, std::move(node));
    return {};
}

QString Bookmarks::moveIn(Tree &tree, const QString &id, const QString &parentId, int index) {
    Tree *from = nullptr;
    Node *node = findIn(tree, id, &from);
    if (!node)
        return tr("That bookmark is gone.");
    // Into itself, or anywhere below itself, is nowhere.
    if (within(tree, parentId, id))
        return tr("A folder can't go inside itself.");
    if (!childrenIn(tree, parentId))
        return tr("That folder is gone.");

    const int fromIndex = int(node - from->data());
    const bool sameList = childrenIn(tree, parentId) == from;
    Node taken = std::move(*node);
    from->erase(from->begin() + fromIndex);
    // `index` counts the list as it was, with the item still in it.
    int at = index;
    if (sameList && index > fromIndex)
        --at;
    Tree *into = childrenIn(tree, parentId);
    if (at < 0 || at > int(into->size()))
        at = int(into->size());
    into->insert(into->begin() + at, std::move(taken));
    return {};
}

QString Bookmarks::add(const QString &parentId, const QString &name, const QString &path, int index) {
    const QString stored = normalize(path);
    if (stored.isEmpty())
        return {};
    QString id;
    const QString failed = transact([&](Tree &tree) {
        Node node;
        node.id = id = newId(tree);
        node.name = name.trimmed().isEmpty() ? defaultName(stored) : name.trimmed();
        node.path = stored;
        node.kind = kindOf(stored);
        std::vector<Node> nodes;
        nodes.push_back(std::move(node));
        return insertIn(tree, parentId, std::move(nodes), index);
    });
    return failed.isEmpty() ? id : QString();
}

QString Bookmarks::addFolder(const QString &parentId, const QString &name, int index) {
    QString id;
    const QString failed = transact([&](Tree &tree) {
        Node node;
        node.id = id = newId(tree);
        node.folder = true;
        node.name = name.trimmed().isEmpty() ? tr("New Folder") : name.trimmed();
        std::vector<Node> nodes;
        nodes.push_back(std::move(node));
        return insertIn(tree, parentId, std::move(nodes), index);
    });
    return failed.isEmpty() ? id : QString();
}

int Bookmarks::addUrls(const QString &parentId, const QList<QUrl> &urls, int index) {
    QStringList paths;
    for (const QUrl &url : urls)
        if (url.isLocalFile() && !normalize(url.toLocalFile()).isEmpty())
            paths << normalize(url.toLocalFile());
    if (paths.isEmpty())
        return 0;
    const QString failed = transact([&](Tree &tree) {
        std::vector<Node> nodes;
        for (const QString &path : paths) {
            Node node;
            // Kept apart from those made just before it, not yet in the tree.
            do
                node.id = newId(tree);
            while (std::any_of(nodes.begin(), nodes.end(), [&](const Node &n) { return n.id == node.id; }));
            node.name = defaultName(path);
            node.path = path;
            node.kind = kindOf(path);
            nodes.push_back(std::move(node));
        }
        return insertIn(tree, parentId, std::move(nodes), index);
    });
    return failed.isEmpty() ? int(paths.size()) : 0;
}

QString Bookmarks::editItem(const QString &id, const QString &name, const QString &path,
                            const std::optional<QString> &parentId) {
    const QString stored = normalize(path);
    return transact([&](Tree &tree) {
        Node *parent = nullptr;
        Node *node = findIn(tree, id, nullptr, &parent);
        if (!node)
            return tr("That bookmark is gone.");
        if (!name.trimmed().isEmpty())
            node->name = name.trimmed();
        if (!node->folder && !stored.isEmpty()) {
            node->path = stored;
            node->kind = kindOf(stored);
        }
        // Checked on the copy, so a refused move leaves the rename unsaved too.
        if (parentId && *parentId != (parent ? parent->id : QString()))
            return moveIn(tree, id, *parentId, -1);
        return QString();
    });
}

QString Bookmarks::edit(const QString &id, const QString &name, const QString &path, const QVariant &parentId) {
    if (!parentId.isValid() || parentId.isNull())
        return editItem(id, name, path, std::nullopt);
    return editItem(id, name, path, parentId.toString());
}

bool Bookmarks::move(const QString &id, const QString &parentId, int index) {
    return transact([&](Tree &tree) { return moveIn(tree, id, parentId, index); }).isEmpty();
}

bool Bookmarks::remove(const QString &id) {
    return transact([&](Tree &tree) {
               Tree *from = nullptr;
               Node *node = findIn(tree, id, &from);
               if (!node)
                   return tr("That bookmark is gone.");
               from->erase(from->begin() + (node - from->data()));
               return QString();
           })
        .isEmpty();
}

QVariantMap Bookmarks::get(const QString &id) const {
    const Node *parent = nullptr;
    const Node *node = findIn(m_root, id, &parent);
    if (!node)
        return {};
    QVariantMap map = mapOf(*node);
    map.insert("parentId", parent ? parent->id : QString());
    return map;
}

bool Bookmarks::isWithin(const QString &id, const QString &ancestorId) const {
    return within(m_root, id, ancestorId);
}

QString Bookmarks::find(const QString &path) const {
    const QString wanted = normalize(path);
    std::function<QString(const Tree &)> walk = [&](const Tree &nodes) {
        for (const Node &node : nodes) {
            if (!node.folder && node.path == wanted)
                return node.id;
            const QString found = walk(node.children);
            if (!found.isEmpty())
                return found;
        }
        return QString();
    };
    return wanted.isEmpty() ? QString() : walk(m_root);
}

QString Bookmarks::normalize(const QString &path) const {
    // Not trimmed: a path from the star or a drop is exact, and a folder's
    // name may end in a space. Typed text is trimmed where it's typed.
    if (path.trimmed().isEmpty())
        return {};
    QString p = path;
    if (!isLocal(p))
        return p;
    if (p.startsWith("file://"))
        p = QUrl(p).toLocalFile();
    if (p == "~" || p.startsWith("~/"))
        p = m_home + p.mid(1);
    else if (!p.startsWith('/'))
        p = m_home + "/" + p;
    p = QDir::cleanPath(p);
    if (p == m_home)
        return "~";
    if (p.startsWith(m_home + "/"))
        return "~" + p.mid(m_home.size());
    return p;
}

QString Bookmarks::resolve(const QString &path) const {
    const QString p = normalize(path);
    if (p == "~")
        return m_home;
    if (p.startsWith("~/"))
        return m_home + p.mid(1);
    return p;
}

QString Bookmarks::absolute(const QString &path, const QString &base) const {
    const QString p = path.trimmed();
    if (p.isEmpty() || p.startsWith('/') || p.startsWith('~') || p.startsWith("file://") || !isLocal(p))
        return path;
    // Recents isn't a folder: like the path bar, a relative path there is
    // taken from home.
    // The base is an exact folder (it may end in a space); only what was
    // typed is trimmed.
    const QString from = base;
    const QString folder = from.isEmpty() || !isLocal(from) ? m_home : resolve(from);
    return normalize(folder + "/" + p);
}

QString Bookmarks::defaultName(const QString &path) const {
    const QString p = resolve(path);
    if (p == m_home)
        return tr("Home");
    if (p == "/")
        return "/";
    if (!isLocal(p))
        return p;
    return QFileInfo(p).fileName();
}

QString Bookmarks::kindOf(const QString &path) const {
    if (!isLocal(path))
        return "folder";
    const QFileInfo info(resolve(path));
    return info.isDir() ? "folder" : info.exists() ? "file" : "missing";
}

bool Bookmarks::refreshKinds(Tree &nodes) {
    bool any = false;
    for (Node &node : nodes) {
        if (node.folder) {
            any = refreshKinds(node.children) || any;
            continue;
        }
        const QString kind = kindOf(node.path);
        if (kind != node.kind) {
            node.kind = kind;
            any = true;
        }
    }
    return any;
}

void Bookmarks::refresh() {
    // Only when something changed: the bar is rebuilt on every change.
    if (refreshKinds(m_root))
        emit changed();
}

QString Bookmarks::completePath(const QString &text, const QString &base) const {
    // The path bar's own completion, from `base` rather than the pane's
    // folder now (the pane may have moved on since the editor opened).
    const QString from = resolve(base.isEmpty() ? QStringLiteral("~") : base);
    return Backend::complete(text, m_home, QFileInfo(from).isDir() ? from : m_home);
}
