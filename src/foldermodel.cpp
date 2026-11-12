#include "foldermodel.h"

#include <QCache>
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QXmlStreamReader>
#include <QUrl>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QtConcurrent>
#include <algorithm>
#include <utility>
#include <sys/stat.h>

FolderModel::FolderModel(QObject *parent) : QAbstractListModel(parent) {
    // Copies, saves and downloads change a folder in bursts; wait for the
    // burst to settle instead of relisting on every event.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    // An open folder in the tree is re-read on its own, not with the folder.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &dir) {
        if (m_childWatched.contains(dir)) {
            m_childDirty.insert(dir);
            m_childDebounce.start();
        } else {
            m_debounce.start();
        }
    });
    // Recents' file is rewritten (replaced) by GTK, so it's watched again
    // after each change.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &file) {
        m_debounce.start();
        if (!m_watcher.files().contains(file) && QFileInfo::exists(file))
            m_watcher.addPath(file);
    });
    connect(&m_debounce, &QTimer::timeout, this, &FolderModel::reload);
    m_childDebounce.setSingleShot(true);
    m_childDebounce.setInterval(250);
    connect(&m_childDebounce, &QTimer::timeout, this, [this] {
        const QSet<QString> dirty = std::exchange(m_childDirty, {});
        for (const QString &folder : dirty)
            if (m_childWatched.contains(folder))
                listChildren(folder);
    });
    m_staleClear.setSingleShot(true);
    m_staleClear.setInterval(150);
    connect(&m_staleClear, &QTimer::timeout, this, &FolderModel::clearEntries);
}

// Generations start at 1, so 0 stops any search or folder count still
// running for this model.
FolderModel::~FolderModel() {
    *m_latest = 0;
    *m_treeLatest = 0;
}

// Recent listings, shared by every model (the window's folder and each
// column), so stepping back and forth or opening a column for a folder just
// seen shows it in the same frame instead of an old or empty one. Each is
// re-read in the background and only replaced when something changed.
static QCache<QString, QVector<FolderModel::Entry>> &recentListings() {
    static QCache<QString, QVector<FolderModel::Entry>> cache(64);
    return cache;
}

// The language is part of it: names sort by its alphabet.
QString FolderModel::cacheKey() const {
    return m_path + '\n' + (m_showHidden ? "h" : "") + '\n' + m_sort + '\n' + QLocale().name();
}

void FolderModel::remember() {
    // Search results are a moment's question, not a folder to come back to.
    if (!m_query.isEmpty())
        return;
    // The folder's own items: open folders are closed on coming back.
    recentListings().insert(cacheKey(), new QVector<Entry>(m_top));
}

// By path, not name: a folder copied with cp -a or rsync lists the same
// names, sizes and times as the one it came from. Name, subtitle, icon and
// details too: a plugin's item keeps its path (its id) when they change.
static bool sameEntry(const FolderModel::Entry &a, const FolderModel::Entry &b) {
    return a.path == b.path && a.size == b.size && a.modified == b.modified && a.isDir == b.isDir
        && a.name == b.name && a.extension == b.extension && a.icon == b.icon && a.details == b.details
        && a.depth == b.depth && a.lines == b.lines;
}

bool FolderModel::sameListing(const QVector<Entry> &a, const QVector<Entry> &b) {
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (!sameEntry(a[i], b[i]))
            return false;
    return true;
}

void FolderModel::setEntries(const QVector<Entry> &entries) {
    beginResetModel();
    m_top = entries;
    m_entries = flatten(entries);
    endResetModel();
    emit countChanged();
}

void FolderModel::clearEntries() {
    m_staleClear.stop();
    if (!m_entries.isEmpty())
        setEntries({});
}

int FolderModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant FolderModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case SizeRole: return e.size;
    case ModifiedRole: return e.modified;
    case KindRole: return e.kind;
    case MimeRole: return e.mime;
    case ExtensionRole: return e.extension;
    case ItemCountRole: return e.itemCount;
    case SelectedRole: return m_selected.contains(e.path);
    case IconRole: return e.icon;
    case DepthRole: return e.depth;
    case OpenRole: return e.isDir && m_open.contains(e.path);
    case LinesRole: return e.lines;
    }
    return {};
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {
        {NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"}, {SizeRole, "size"},
        {ModifiedRole, "modified"}, {KindRole, "kind"}, {MimeRole, "mime"},
        {ExtensionRole, "extension"}, {ItemCountRole, "itemCount"}, {SelectedRole, "selected"},
        {IconRole, "icon"}, {DepthRole, "depth"}, {OpenRole, "open"},
        {LinesRole, "lines"},
    };
}

void FolderModel::setShowHidden(bool show) {
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    emit showHiddenChanged();
    reload();
}

void FolderModel::setSortMode(const QString &sort) {
    if (sort == m_sort || !sortModes().contains(sort))
        return;
    m_sort = sort;
    emit sortModeChanged();
    reload();
}

void FolderModel::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    // Search results are flat, and the folder comes back closed.
    clearTree();
    setSelected({});
    // Back to the folder: its remembered listing shows at once.
    if (m_query.isEmpty()) {
        if (const auto *cached = recentListings().object(cacheKey()))
            setEntries(*cached);
    }
    reload();
}

void FolderModel::setItems(const QVariantList &items) {
    if (items == m_items)
        return;
    m_items = items;
    emit itemsChanged();
    // A plugin's view follows its polls.
    if (isPluginView(m_path))
        reload();
}

void FolderModel::setPath(const QString &path) {
    const QString clean = path.isEmpty() || isView(path) ? path : QDir::cleanPath(path);
    if (clean == m_path)
        return;
    clearTree();
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    m_path = clean;
    emit pathChanged();
    // A search is of one folder; going elsewhere ends it.
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    setSelected({});

    // The old folder stays up while the new one is read, which locally takes
    // a few milliseconds: swapping straight from one to the other avoids an
    // empty frame between them. A slow listing (another machine's) clears it
    // after a moment, so the old folder never lingers under the new path.
    // An empty path is a column with nothing to show yet.
    if (m_path.isEmpty()) {
        // What's still being read is for the folder left behind.
        ++m_generation;
        *m_latest = m_generation;
        setLoading(false);
        clearEntries();
        return;
    }
    if (const auto *cached = recentListings().object(cacheKey())) {
        m_staleClear.stop();
        setEntries(*cached);
        setError({});
        emit loaded();
    } else {
        m_staleClear.start();
    }
    // Plugins' views have nothing to watch: their plugins are polled.
    if (!isPluginView(m_path))
        m_watcher.addPath(m_path == recentPath() ? recentFile() : m_path);
    reload();
}

QString FolderModel::kindForMime(const QString &mime) {
    if (mime == "inode/directory") return "folder";
    if (mime == "application/pdf") return "pdf";
    if (mime.startsWith("image/")) return "image";
    if (mime.startsWith("video/")) return "video";
    if (mime.startsWith("audio/")) return "audio";
    // Anything readable as text (source code, config, JSON, Ruby, ...) is
    // text, whatever its own type is called.
    static const QMimeDatabase types;
    if (mime.startsWith("text/") || types.mimeTypeForName(mime).inherits("text/plain"))
        return "text";
    static const QStringList archives{"application/zip", "application/x-tar", "application/gzip",
                                      "application/x-xz", "application/zstd", "application/x-7z-compressed",
                                      "application/x-compressed-tar", "application/x-zstd-compressed-tar",
                                      "application/vnd.rar", "application/x-iso9660-image"};
    // Every compressed tar too: .tar.xz, .tar.bz2, .tar.lz4 and the rest.
    if (archives.contains(mime) || mime.endsWith("-compressed-tar")) return "archive";
    return "other";
}

// Everything in a folder (sockets and broken links too), the hidden only
// when they're shown.
static QDir::Filters entryFilters(bool showHidden) {
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;
    return filters;
}

QVector<FolderModel::Entry> FolderModel::list(const QString &path, bool showHidden, const QString &sort,
                                              QString *error, const std::function<bool()> &stop,
                                              const QLocale &locale) {
    if (path == recentPath()) {
        // Recents are always most recently used first; that's what they're for.
        return listRecent(recentFile(), showHidden, 200, stop);
    }
    QVector<Entry> entries;
    QDir dir(path);
    if (!dir.exists()) {
        if (error) *error = QObject::tr("This folder no longer exists.");
        return entries;
    }
    if (!QFileInfo(path).isReadable()) {
        if (error) *error = QObject::tr("You don't have permission to open this folder.");
        return entries;
    }

    // Extension-only matching: reading every file's header would make large
    // folders and network mounts crawl.
    QMimeDatabase mimes;
    QDirIterator it(path, entryFilters(showHidden));
    while (it.hasNext()) {
        if (stop && stop())
            return {};
        entries.push_back(entryFor(it.nextFileInfo(), mimes));
    }
    sortEntries(entries, sort, locale);
    return entries;
}

FolderModel::Entry FolderModel::entryFor(const QFileInfo &info, const QMimeDatabase &mimes) {
    Entry e;
    e.name = info.fileName();
    e.path = info.absoluteFilePath();
    e.isDir = info.isDir();
    e.size = e.isDir ? 0 : info.size();
    e.modified = info.lastModified();
    e.mime = e.isDir ? QStringLiteral("inode/directory")
                     : mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
    e.kind = kindForMime(e.mime);
    e.extension = e.isDir ? QString() : info.suffix().toUpper();
    return e;
}

QVector<FolderModel::Entry> FolderModel::search(const QString &root, const QString &query, bool showHidden,
                                                const QString &sort, const std::function<bool()> &stop,
                                                int limit, const QLocale &locale) {
    QVector<Entry> entries;
    struct stat top;
    if (::stat(QFile::encodeName(root).constData(), &top) != 0)
        return entries;
    const QDir::Filters filters = entryFilters(showHidden);

    // Breadth first, so what's near comes before what's deep when there are
    // more matches than the limit.
    QMimeDatabase mimes;
    const QDir base(root);
    QStringList folders{root};
    while (!folders.isEmpty() && entries.size() < limit) {
        if (stop && stop())
            return {};
        QDirIterator it(folders.takeFirst(), filters);
        while (it.hasNext()) {
            if (stop && stop())
                return {};
            const QFileInfo info = it.nextFileInfo();
            if (info.fileName().contains(query, Qt::CaseInsensitive) && entries.size() < limit) {
                Entry e = entryFor(info, mimes);
                e.name = base.relativeFilePath(e.path);
                entries.push_back(e);
            }
            // Other filesystems (a mounted drive, another machine, /proc
            // from /) and symlinked folders aren't walked into.
            struct stat st;
            if (info.isDir() && !info.isSymLink()
                && ::lstat(QFile::encodeName(info.absoluteFilePath()).constData(), &st) == 0
                && st.st_dev == top.st_dev)
                folders << info.absoluteFilePath();
        }
    }
    sortEntries(entries, sort, locale);
    return entries;
}

QStringList FolderModel::sortModes() {
    // "modified" is newest first; it keeps its name so saved settings
    // from before "oldest" existed still read right.
    return {"name", "kind", "modified", "oldest", "size"};
}

void FolderModel::sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale) {
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        // Folders first in every order, as everywhere else on the desktop.
        if (a.isDir != b.isDir)
            return a.isDir;
        if (sort == "modified" && a.modified != b.modified)
            return a.modified > b.modified;
        if (sort == "oldest" && a.modified != b.modified)
            return a.modified < b.modified;
        // Biggest first; folders have no size of their own and stay by name.
        if (sort == "size" && !a.isDir && a.size != b.size)
            return a.size > b.size;
        if (sort == "kind" && !a.isDir) {
            if (a.kind != b.kind)
                return a.kind < b.kind;
            if (a.extension != b.extension)
                return collator.compare(a.extension, b.extension) < 0;
        }
        return collator.compare(a.name, b.name) < 0;
    });
}

// Recents are GTK's recently used files (~/.local/share/recently-used.xbel),
// which every GTK and many other apps add to: the files still there, the
// most recently used first.
QVector<FolderModel::Entry> FolderModel::listRecent(const QString &xbel, bool showHidden, int limit,
                                                    const std::function<bool()> &stop) {
    struct Used { QString path; QDateTime when; };
    QVector<Used> used;
    QFile file(xbel);
    if (file.open(QIODevice::ReadOnly)) {
        QXmlStreamReader xml(&file);
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("bookmark")) {
                const auto attrs = xml.attributes();
                const QUrl url(attrs.value("href").toString());
                QDateTime when = QDateTime::fromString(attrs.value("visited").toString(), Qt::ISODate);
                const QDateTime modified = QDateTime::fromString(attrs.value("modified").toString(), Qt::ISODate);
                if (!when.isValid() || (modified.isValid() && modified > when))
                    when = modified;
                if (url.isLocalFile())
                    used.push_back({url.toLocalFile(), when});
            }
        }
    }
    std::stable_sort(used.begin(), used.end(), [](const Used &a, const Used &b) { return a.when > b.when; });

    QVector<Entry> entries;
    QStringList seen;
    QMimeDatabase mimes;
    for (const Used &u : used) {
        // Each check can wait on a slow drive, so leaving Recents stops it.
        if (stop && stop())
            break;
        const QFileInfo info(u.path);
        if (seen.contains(u.path) || !info.exists() || (!showHidden && info.fileName().startsWith('.')))
            continue;
        seen << u.path;
        entries.push_back(entryFor(info, mimes));
        if (entries.size() == limit)
            break;
    }
    return entries;
}

QVector<FolderModel::Entry> FolderModel::listItems(const QVariantList &items, const QString &query) {
    QVector<Entry> entries;
    for (const QVariant &item : items) {
        const QVariantMap map = item.toMap();
        Entry e;
        e.name = map.value("name").toString();
        if (!e.name.contains(query, Qt::CaseInsensitive))
            continue;
        e.path = map.value("path").toString();
        e.kind = QStringLiteral("item");
        e.icon = map.value("icon").toString();
        e.extension = map.value("subtitle").toString();
        e.details = map.value("details").toList();
        entries.push_back(e);
    }
    return entries;
}

QString FolderModel::recentFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel";
}

void FolderModel::summarize(Entry &entry, bool showHidden) {
    QDir dir(entry.path);
    const QStringList names = dir.entryList(entryFilters(showHidden), QDir::Name);
    entry.itemCount = names.size();
    entry.peek.clear();

    static const QStringList pictures{"jpg", "jpeg", "png", "webp", "gif", "heic", "avif", "jxl"};
    for (const QString &name : names) {
        if (pictures.contains(QFileInfo(name).suffix().toLower()))
            entry.peek << dir.filePath(name);
        if (entry.peek.size() == 2)
            break;
    }
}

int FolderModel::itemCount(const QString &folder, bool showHidden) {
    int count = 0;
    for (QDirIterator it(folder, entryFilters(showHidden)); it.hasNext(); it.next())
        ++count;
    return count;
}

void FolderModel::reload() {
    if (m_path.isEmpty())
        return;
    const quint64 generation = ++m_generation;
    setLoading(true);
    // Open folders are re-read too: a new order or hidden files show in
    // them as well.
    for (const QString &folder : QSet<QString>(m_childWatched))
        listChildren(folder);

    const QString path = m_path;
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QString query = m_query;
    const QLocale locale;
    const QVariantList items = m_items;
    *m_latest = generation;
    const auto latest = m_latest;
    auto *watcher = new QFutureWatcher<QPair<QVector<Entry>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return; // A newer listing replaced this one while it ran.
        m_staleClear.stop();
        const auto [entries, error] = watcher->result();
        // Unchanged (the usual case after showing a remembered listing):
        // leave the items alone, and only fill in folder counts still missing.
        const QVector<Entry> rows = flatten(entries);
        const bool same = error == m_error && !m_entries.isEmpty() && sameListing(m_entries, rows);
        // With folders open, only what changed moves, so the tree doesn't
        // jump back to the top.
        if (!same && rows.size() != entries.size()) {
            m_top = entries;
            showRows(rows);
        } else if (!same) {
            setEntries(entries);
        }
        if (!m_selected.isEmpty()) {
            QSet<QString> still;
            for (const Entry &e : m_entries)
                if (m_selected.contains(e.path))
                    still.insert(e.path);
            if (still != m_selected) {
                m_selected = still;
                emit selectionChanged();
            }
        }
        setError(error);
        setLoading(false);
        emit loaded();
        if (error.isEmpty())
            remember();
        followOpenFolders();
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run([path, showHidden, sort, query, items, latest, generation, locale] {
        QString error;
        const auto overtaken = [&] { return *latest != generation; };
        auto entries = isPluginView(path) ? listItems(items, query)
                     : query.isEmpty() || path == recentPath() ? list(path, showHidden, sort, &error, overtaken, locale)
                     : search(path, query, showHidden, sort, overtaken, 1000, locale);
        return qMakePair(entries, error);
    }));
}

void FolderModel::countFolders(quint64 generation) {
    // Only the folders not counted yet: a listing just read, or one whose
    // counting was overtaken by a re-read (and so remembered without them).
    QVector<QPair<int, Entry>> folders;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].isDir && m_entries[row].itemCount < 0)
            folders.push_back({row, m_entries[row]});
    if (folders.isEmpty())
        return;

    const bool showHidden = m_showHidden;
    auto *watcher = new QFutureWatcher<QVector<QPair<int, Entry>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        // By path: a folder opened or closed meanwhile moves the rows.
        bool updated = false;
        QHash<QString, int> rows, tops;
        for (int row = 0; row < m_entries.size(); ++row)
            rows.insert(m_entries[row].path, row);
        for (int top = 0; top < m_top.size(); ++top)
            tops.insert(m_top[top].path, top);
        // An open folder's items keep their counts too, or the next time
        // the tree is put together they'd come back uncounted.
        QHash<QString, QHash<QString, int>> nested;
        for (const auto &[unused, summary] : watcher->result()) {
            if (const int top = tops.value(summary.path, -1); top >= 0)
                m_top[top].itemCount = summary.itemCount;
            const QString parent = QFileInfo(summary.path).path();
            if (auto listed = m_listed.find(parent); listed != m_listed.end()) {
                if (!nested.contains(parent)) {
                    QHash<QString, int> at;
                    for (int i = 0; i < listed->size(); ++i)
                        at.insert(listed->at(i).path, i);
                    nested.insert(parent, at);
                }
                if (const int i = nested[parent].value(summary.path, -1); i >= 0)
                    (*listed)[i].itemCount = summary.itemCount;
            }
            const int row = rows.value(summary.path, -1);
            if (row < 0)
                continue;
            m_entries[row].itemCount = summary.itemCount;
            const QModelIndex at = index(row);
            emit dataChanged(at, at, {ItemCountRole});
            updated = true;
        }
        // QML re-reads its get() snapshots (the current item, which the
        // details pane shows) on countChanged, and the counts they show
        // just arrived.
        if (updated)
            emit countChanged();
        remember();
    });
    const auto latest = m_latest;
    watcher->setFuture(QtConcurrent::run([folders, showHidden, latest, generation]() mutable {
        // Overtaken (another folder, or a re-read), the counts would be
        // thrown away, so it stops instead of listing every subfolder.
        for (auto &folder : folders) {
            if (*latest != generation)
                break;
            folder.second.itemCount = itemCount(folder.second.path, showHidden);
        }
        return folders;
    }));
}

QVariantMap FolderModel::get(int row) const {
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    QVariantMap map = toMap(e);
    map.insert("selected", m_selected.contains(e.path));
    map.insert("open", e.isDir && m_open.contains(e.path));
    return map;
}

QVariantMap FolderModel::toMap(const Entry &entry) {
    return {
        {"name", entry.name}, {"path", entry.path}, {"isDir", entry.isDir}, {"size", entry.size},
        {"modified", entry.modified}, {"kind", entry.kind}, {"mime", entry.mime},
        {"extension", entry.extension}, {"itemCount", entry.itemCount}, {"icon", entry.icon},
        {"details", entry.details}, {"depth", entry.depth}, {"lines", entry.lines},
    };
}

void FolderModel::setSelected(const QSet<QString> &paths) {
    if (paths == m_selected)
        return;
    m_selected = paths;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {SelectedRole});
    emit selectionChanged();
}

void FolderModel::selectOnly(int row) {
    setSelected(row >= 0 && row < m_entries.size() ? QSet<QString>{m_entries[row].path} : QSet<QString>{});
}

void FolderModel::toggleSelected(int row) {
    if (row < 0 || row >= m_entries.size())
        return;
    QSet<QString> paths = m_selected;
    const QString path = m_entries[row].path;
    if (!paths.remove(path))
        paths.insert(path);
    setSelected(paths);
}

void FolderModel::selectRange(int from, int to, bool add) {
    if (m_entries.isEmpty())
        return;
    from = qBound(0, from, int(m_entries.size()) - 1);
    to = qBound(0, to, int(m_entries.size()) - 1);
    QSet<QString> paths = add ? m_selected : QSet<QString>{};
    for (int row = qMin(from, to); row <= qMax(from, to); ++row)
        paths.insert(m_entries[row].path);
    setSelected(paths);
}

void FolderModel::selectRows(const QList<int> &rows) {
    QSet<QString> paths;
    for (int row : rows)
        if (row >= 0 && row < m_entries.size())
            paths.insert(m_entries[row].path);
    setSelected(paths);
}

void FolderModel::selectAll() {
    QSet<QString> paths;
    for (const Entry &e : m_entries)
        paths.insert(e.path);
    setSelected(paths);
}

void FolderModel::clearSelection() {
    setSelected({});
}

bool FolderModel::isSelected(int row) const {
    return row >= 0 && row < m_entries.size() && m_selected.contains(m_entries[row].path);
}

QStringList FolderModel::selectedPaths() const {
    // In the folder's order, not the order they were picked in.
    QStringList paths;
    for (const Entry &e : m_entries)
        if (m_selected.contains(e.path))
            paths << e.path;
    return paths;
}

// Only the folder's own items: the name is always of something in it (a file
// pasted, a folder made, a name given), never the same name in an open folder.
int FolderModel::indexOf(const QString &name) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].depth == 0 && m_entries[row].name == name)
            return row;
    return -1;
}

int FolderModel::indexOfPath(const QString &path) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].path == path)
            return row;
    return -1;
}

void FolderModel::setLoading(bool loading) {
    if (loading == m_loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void FolderModel::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}


// Only a folder's items can be folders to open: a search lists matches from
// all over, and Recents and plugins' views aren't folders.
bool FolderModel::canNest() const {
    return m_query.isEmpty() && !m_path.isEmpty() && !isView(m_path) && !m_path.startsWith("plugin:");
}

QVector<FolderModel::Entry> FolderModel::flatten(const QVector<Entry> &top) const {
    if (m_open.isEmpty())
        return top;
    QVector<Entry> rows;
    rows.reserve(top.size());
    addRows(rows, top, 0);
    return rows;
}

// `guides` holds the lines of the folders this one is in: a row's own lines
// are those, then its elbow, and its items inherit them with a line of
// their own carrying on below it only if more rows follow it here.
void FolderModel::addRows(QVector<Entry> &rows, const QVector<Entry> &entries, int depth, const QString &guides) const {
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries[i];
        const bool last = i == entries.size() - 1;
        rows.push_back(entry);
        rows.back().depth = depth;
        rows.back().lines = depth == 0 ? QString() : guides + (last ? u'└' : u'├');
        // An open folder not listed yet shows open, with its items to come.
        if (entry.isDir && m_open.contains(entry.path)) {
            const auto listed = m_listed.constFind(entry.path);
            if (listed != m_listed.constEnd())
                addRows(rows, *listed, depth + 1, depth == 0 ? QString() : guides + (last ? u' ' : u'│'));
        }
    }
}

// Moves from the rows shown to `rows` by removing and inserting only the
// stretch between what they start and end with in common, so a view keeps
// its place and its current item, then updates rows whose details changed.
void FolderModel::showRows(const QVector<Entry> &rows) {
    const auto same = [](const Entry &a, const Entry &b) { return a.path == b.path && a.depth == b.depth; };
    const int before = m_entries.size();
    int head = 0;
    while (head < before && head < rows.size() && same(m_entries[head], rows[head]))
        ++head;
    int tail = 0;
    while (tail < before - head && tail < rows.size() - head
           && same(m_entries[before - 1 - tail], rows[rows.size() - 1 - tail]))
        ++tail;
    const bool shifting = before - head - tail > 0 || rows.size() - head - tail > 0;
    if (shifting)
        emit rowsShifting();
    if (before - head - tail > 0) {
        beginRemoveRows({}, head, before - tail - 1);
        m_entries.remove(head, before - head - tail);
        endRemoveRows();
    }
    if (rows.size() - head - tail > 0) {
        beginInsertRows({}, head, rows.size() - tail - 1);
        // Put together in one go: inserted one at a time, every row below
        // would move once per row inserted, and a big folder opened above
        // a long list would hold the window up for seconds.
        QVector<Entry> spliced;
        spliced.reserve(rows.size());
        spliced.append(m_entries.mid(0, head));
        spliced.append(rows.mid(head, rows.size() - head - tail));
        spliced.append(m_entries.mid(head));
        m_entries = std::move(spliced);
        endInsertRows();
    }
    bool changed = before != m_entries.size();
    for (int row = 0; row < m_entries.size(); ++row) {
        if (sameEntry(m_entries[row], rows[row]) && m_entries[row].itemCount == rows[row].itemCount)
            continue;
        m_entries[row] = rows[row];
        emit dataChanged(index(row), index(row));
        changed = true;
    }
    // What's no longer showing (in a folder closed or gone) isn't selected.
    if (!m_selected.isEmpty()) {
        QSet<QString> still;
        for (const Entry &e : m_entries)
            if (m_selected.contains(e.path))
                still.insert(e.path);
        if (still != m_selected) {
            m_selected = still;
            emit selectionChanged();
        }
    }
    // QML re-reads its get() snapshots on countChanged.
    if (changed)
        emit countChanged();
    if (shifting)
        emit rowsShifted();
}

void FolderModel::showTree() {
    showRows(flatten(m_top));
    followOpenFolders();
}

// Watches and lists the open folders showing, and stops watching the ones
// that went out of sight (closed, or inside one that was).
void FolderModel::followOpenFolders() {
    QSet<QString> showing;
    for (const Entry &e : m_entries)
        if (e.isDir && m_open.contains(e.path))
            showing.insert(e.path);
    for (const QString &folder : QSet<QString>(m_childWatched))
        if (!showing.contains(folder))
            unwatchChild(folder);
    for (const QString &folder : showing)
        if (!m_childWatched.contains(folder))
            listChildren(folder);
}

void FolderModel::unwatchChild(const QString &folder) {
    m_childWatched.remove(folder);
    m_childDirty.remove(folder);
    m_childListing.remove(folder);
    if (m_watcher.directories().contains(folder))
        m_watcher.removePath(folder);
}

void FolderModel::listChildren(const QString &folder) {
    if (!m_childWatched.contains(folder)) {
        m_childWatched.insert(folder);
        m_watcher.addPath(folder);
    }
    const quint64 listing = ++m_childSerial;
    m_childListing.insert(folder, listing);

    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QLocale locale;
    const auto latest = m_treeLatest;
    const quint64 tree = *m_treeLatest;
    auto *watcher = new QFutureWatcher<QPair<QVector<Entry>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, folder, listing] {
        watcher->deleteLater();
        if (m_childListing.value(folder) != listing)
            return; // Re-read again since, closed, or the tree was cleared.
        m_childListing.remove(folder);
        const auto [entries, error] = watcher->result();
        if (error.isEmpty()) {
            m_listed.insert(folder, entries);
        } else {
            // Gone or shut: it closes, with nothing inside to show.
            m_open.remove(folder);
            m_listed.remove(folder);
            unwatchChild(folder);
            if (const int row = indexOfPath(folder); row >= 0)
                emit dataChanged(index(row), index(row), {OpenRole});
        }
        showTree();
        // Its items show first and its folders are counted after, as for
        // the folder's own: thousands of folders on a slow drive would
        // otherwise keep it open and empty for as long as counting takes.
        countFolders(m_generation);
    });
    watcher->setFuture(QtConcurrent::run([folder, showHidden, sort, locale, latest, tree] {
        QString error;
        const auto overtaken = [&] { return *latest != tree; };
        auto entries = list(folder, showHidden, sort, &error, overtaken, locale);
        return qMakePair(entries, error);
    }));
}

void FolderModel::clearTree() {
    for (const QString &folder : QSet<QString>(m_childWatched))
        unwatchChild(folder);
    m_childListing.clear();
    m_childDirty.clear();
    m_listed.clear();
    m_open.clear();
    ++*m_treeLatest;
}

bool FolderModel::expand(int row) {
    if (row < 0 || row >= m_entries.size() || !canNest())
        return false;
    const Entry &e = m_entries[row];
    if (!e.isDir || m_open.contains(e.path))
        return false;
    m_open.insert(e.path);
    emit dataChanged(index(row), index(row), {OpenRole});
    // What was listed before shows at once (the folders open inside it
    // too); the folder is read again in the background either way.
    showTree();
    return true;
}

bool FolderModel::collapse(int row) {
    if (row < 0 || row >= m_entries.size())
        return false;
    const QString path = m_entries[row].path;
    if (!m_entries[row].isDir || !m_open.remove(path))
        return false;
    emit dataChanged(index(row), index(row), {OpenRole});
    showTree();
    return true;
}

void FolderModel::collapseAll() {
    if (m_open.isEmpty())
        return;
    clearTree();
    showRows(m_top);
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {OpenRole});
}

int FolderModel::parentRow(int row) const {
    if (row < 0 || row >= m_entries.size())
        return -1;
    const int depth = m_entries[row].depth;
    for (int above = row - 1; above >= 0; --above)
        if (m_entries[above].depth < depth)
            return above;
    return -1;
}
