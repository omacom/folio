#include "foldermodel.h"

#include <QCache>
#include <QCollator>
#include <QDir>
#include <QDirListing>
#include <QXmlStreamReader>
#include <QUrl>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <sys/stat.h>

FolderModel::FolderModel(QObject *parent) : QAbstractListModel(parent) {
    // Copies, saves and downloads change a folder in bursts; wait for the
    // burst to settle instead of relisting on every event.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    // Recents' file is rewritten (replaced) by GTK, so it's watched again
    // after each change.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &file) {
        m_debounce.start();
        if (!m_watcher.files().contains(file) && QFileInfo::exists(file))
            m_watcher.addPath(file);
    });
    connect(&m_debounce, &QTimer::timeout, this, &FolderModel::reload);
    m_staleClear.setSingleShot(true);
    m_staleClear.setInterval(150);
    connect(&m_staleClear, &QTimer::timeout, this, &FolderModel::clearEntries);
}

// Recent listings, shared by every model (the window's folder and each
// column), so stepping back and forth or opening a column for a folder just
// seen shows it in the same frame instead of an old or empty one. Each is
// re-read in the background and only replaced when something changed.
static QCache<QString, QVector<FolderModel::Entry>> &recentListings() {
    static QCache<QString, QVector<FolderModel::Entry>> cache(64);
    return cache;
}

QString FolderModel::cacheKey() const {
    return m_path + '\n' + (m_showHidden ? "h" : "") + '\n' + m_sort;
}

void FolderModel::remember() {
    // Search results are a moment's question, not a folder to come back to.
    if (!m_query.isEmpty())
        return;
    recentListings().insert(cacheKey(), new QVector<Entry>(m_entries));
}

bool FolderModel::sameListing(const QVector<Entry> &a, const QVector<Entry> &b) {
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].size != b[i].size || a[i].modified != b[i].modified
            || a[i].isDir != b[i].isDir)
            return false;
    return true;
}

void FolderModel::clearEntries() {
    m_staleClear.stop();
    if (m_entries.isEmpty())
        return;
    beginResetModel();
    m_entries.clear();
    endResetModel();
    emit countChanged();
}

int FolderModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant FolderModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return e.name;
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case SizeRole: return e.size;
    case ModifiedRole: return e.modified;
    case KindRole: return e.kind;
    case MimeRole: return e.mime;
    case ExtensionRole: return e.extension;
    case ItemCountRole: return e.itemCount;
    case PeekRole: return e.peek;
    case SelectedRole: return m_selected.contains(e.name);
    }
    return {};
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {
        {NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"}, {SizeRole, "size"},
        {ModifiedRole, "modified"}, {KindRole, "kind"}, {MimeRole, "mime"},
        {ExtensionRole, "extension"}, {ItemCountRole, "itemCount"}, {PeekRole, "peek"},
        {SelectedRole, "selected"},
    };
}

int FolderModel::folderCount() const {
    return std::count_if(m_entries.begin(), m_entries.end(), [](const Entry &e) { return e.isDir; });
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
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged();
    }
    // Back to the folder: its remembered listing shows at once.
    if (m_query.isEmpty()) {
        if (const auto *cached = recentListings().object(cacheKey())) {
            beginResetModel();
            m_entries = *cached;
            endResetModel();
            emit countChanged();
        }
    }
    reload();
}

void FolderModel::setPath(const QString &path) {
    const QString clean = path.isEmpty() || path == recentPath() ? path : QDir::cleanPath(path);
    if (clean == m_path)
        return;
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
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged();
    }

    // The old folder stays up while the new one is read, which locally takes
    // a few milliseconds: swapping straight from one to the other avoids an
    // empty frame between them. A slow listing (a tailnet machine) clears it
    // after a moment, so the old folder never lingers under the new path.
    // An empty path is a column with nothing to show yet.
    if (m_path.isEmpty()) {
        clearEntries();
        return;
    }
    if (const auto *cached = recentListings().object(cacheKey())) {
        m_staleClear.stop();
        beginResetModel();
        m_entries = *cached;
        endResetModel();
        emit countChanged();
        setError({});
        emit loaded();
    } else {
        m_staleClear.start();
    }
    m_watcher.addPath(m_path == recentPath() ? recentFile() : m_path);
    reload();
}

QString FolderModel::kindForMime(const QString &mime) {
    if (mime == "inode/directory") return "folder";
    if (mime == "application/pdf") return "pdf";
    if (mime.startsWith("image/")) return "image";
    if (mime.startsWith("video/")) return "video";
    if (mime.startsWith("audio/")) return "audio";
    if (mime.startsWith("text/") || mime == "application/json" || mime == "application/x-shellscript"
        || mime == "application/toml" || mime == "application/xml" || mime == "application/x-yaml")
        return "text";
    static const QStringList archives{"application/zip", "application/x-tar", "application/gzip",
                                      "application/x-xz", "application/zstd", "application/x-7z-compressed",
                                      "application/x-compressed-tar", "application/x-zstd-compressed-tar",
                                      "application/vnd.rar", "application/x-iso9660-image"};
    if (archives.contains(mime)) return "archive";
    return "other";
}

QVector<FolderModel::Entry> FolderModel::list(const QString &path, bool showHidden, const QString &sort,
                                              QString *error) {
    if (path == recentPath()) {
        // Recents are always most recently used first; that's what they're for.
        Q_UNUSED(sort);
        return listRecent(recentFile(), showHidden);
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

    // Links unresolved, so broken ones, sockets and fifos are listed too.
    const auto flags = showHidden ? QDirListing::IteratorFlag::IncludeHidden : QDirListing::IteratorFlag::Default;

    // Extension-only matching: reading every file's header would make large
    // folders and network mounts crawl.
    QMimeDatabase mimes;
    // Each entry is built as it's read, so no file info outlives its entry.
    for (const auto &item : QDirListing(path, flags))
        entries.push_back(entryFor(item.fileInfo(), mimes));

    sortEntries(entries, sort);
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
                                                int limit) {
    QVector<Entry> entries;
    struct stat top;
    if (::stat(QFile::encodeName(root).constData(), &top) != 0)
        return entries;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;

    // Breadth first, so what's near comes before what's deep when there are
    // more matches than the limit.
    QMimeDatabase mimes;
    const QDir base(root);
    QStringList folders{root};
    while (!folders.isEmpty() && entries.size() < limit) {
        if (stop && stop())
            return {};
        const QString folder = folders.takeFirst();
        const auto infos = QDir(folder).entryInfoList(filters, QDir::NoSort);
        for (const QFileInfo &info : infos) {
            if (info.fileName().contains(query, Qt::CaseInsensitive) && entries.size() < limit) {
                Entry e = entryFor(info, mimes);
                e.name = base.relativeFilePath(e.path);
                entries.push_back(e);
            }
            // Other filesystems (a mounted drive, a tailnet machine, /proc
            // from /) and symlinked folders aren't walked into.
            struct stat st;
            if (info.isDir() && !info.isSymLink()
                && ::lstat(QFile::encodeName(info.absoluteFilePath()).constData(), &st) == 0
                && st.st_dev == top.st_dev)
                folders << info.absoluteFilePath();
        }
    }
    sortEntries(entries, sort);
    return entries;
}

QStringList FolderModel::sortModes() {
    // "modified" is newest first; it keeps its name so saved settings
    // from before "oldest" existed still read right.
    return {"name", "kind", "modified", "oldest", "size"};
}

void FolderModel::sortEntries(QVector<Entry> &entries, const QString &sort) {
    QCollator collator;
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
        if (const int order = collator.compare(a.name, b.name))
            return order < 0;
        // Names the collator calls equal go in QDir's order: lower-cased, then as written.
        const int lower = a.name.toLower().compare(b.name.toLower());
        return lower ? lower < 0 : a.name < b.name;
    });
}

// Recents are GTK's recently used files (~/.local/share/recently-used.xbel),
// which every GTK and many other apps add to: the files still there, the
// most recently used first.
QVector<FolderModel::Entry> FolderModel::listRecent(const QString &xbel, bool showHidden, int limit) {
    struct Used { QString path; QDateTime when; };
    QVector<Used> used;
    QFile file(xbel);
    if (file.open(QIODevice::ReadOnly)) {
        QXmlStreamReader xml(&file);
        while (xml.readNextStartElement() || !xml.atEnd()) {
            if (xml.isStartElement() && xml.name() == QLatin1String("bookmark")) {
                const auto attrs = xml.attributes();
                const QUrl url(attrs.value("href").toString());
                QDateTime when = QDateTime::fromString(attrs.value("visited").toString(), Qt::ISODate);
                const QDateTime modified = QDateTime::fromString(attrs.value("modified").toString(), Qt::ISODate);
                if (!when.isValid() || (modified.isValid() && modified > when))
                    when = modified;
                if (url.isLocalFile())
                    used.push_back({url.toLocalFile(), when});
            }
            if (xml.hasError())
                break;
        }
    }
    std::stable_sort(used.begin(), used.end(), [](const Used &a, const Used &b) { return a.when > b.when; });

    QVector<Entry> entries;
    QStringList seen;
    QMimeDatabase mimes;
    for (const Used &u : used) {
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

QString FolderModel::recentFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel";
}

void FolderModel::summarize(Entry &entry, bool showHidden) {
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;
    QDir dir(entry.path);
    const QStringList names = dir.entryList(filters, QDir::Name);
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

void FolderModel::reload() {
    if (m_path.isEmpty())
        return;
    const quint64 generation = ++m_generation;
    setLoading(true);

    const QString path = m_path;
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QString query = m_query;
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
        // leave the items, and their folder summaries, alone.
        const bool same = error == m_error && !m_entries.isEmpty() && sameListing(m_entries, entries);
        if (!same) {
            beginResetModel();
            m_entries = entries;
            endResetModel();
            emit countChanged();
        }
        if (!m_selected.isEmpty()) {
            QSet<QString> still;
            for (const Entry &e : m_entries)
                if (m_selected.contains(e.name))
                    still.insert(e.name);
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
        if (!same)
            summarizeFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run([path, showHidden, sort, query, latest, generation] {
        QString error;
        auto entries = query.isEmpty() || path == recentPath()
            ? list(path, showHidden, sort, &error)
            : search(path, query, showHidden, sort, [&] { return *latest != generation; });
        return qMakePair(entries, error);
    }));
}

void FolderModel::summarizeFolders(quint64 generation) {
    QVector<QPair<int, Entry>> folders;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].isDir)
            folders.push_back({row, m_entries[row]});
    if (folders.isEmpty())
        return;

    const bool showHidden = m_showHidden;
    auto *watcher = new QFutureWatcher<QVector<QPair<int, Entry>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        for (const auto &[row, summary] : watcher->result()) {
            if (row >= m_entries.size() || m_entries[row].path != summary.path)
                continue;
            m_entries[row].itemCount = summary.itemCount;
            m_entries[row].peek = summary.peek;
            const QModelIndex at = index(row);
            emit dataChanged(at, at, {ItemCountRole, PeekRole});
        }
        remember();
    });
    watcher->setFuture(QtConcurrent::run([folders, showHidden]() mutable {
        for (auto &folder : folders)
            summarize(folder.second, showHidden);
        return folders;
    }));
}

QVariantMap FolderModel::get(int row) const {
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return {
        {"name", e.name}, {"path", e.path}, {"isDir", e.isDir}, {"size", e.size},
        {"modified", e.modified}, {"kind", e.kind}, {"mime", e.mime},
        {"extension", e.extension}, {"itemCount", e.itemCount}, {"peek", e.peek},
        {"selected", m_selected.contains(e.name)},
    };
}

void FolderModel::setSelected(const QSet<QString> &names) {
    if (names == m_selected)
        return;
    m_selected = names;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {SelectedRole});
    emit selectionChanged();
}

void FolderModel::selectOnly(int row) {
    setSelected(row >= 0 && row < m_entries.size() ? QSet<QString>{m_entries[row].name} : QSet<QString>{});
}

void FolderModel::toggleSelected(int row) {
    if (row < 0 || row >= m_entries.size())
        return;
    QSet<QString> names = m_selected;
    const QString name = m_entries[row].name;
    if (!names.remove(name))
        names.insert(name);
    setSelected(names);
}

void FolderModel::selectRange(int from, int to, bool add) {
    if (m_entries.isEmpty())
        return;
    from = qBound(0, from, int(m_entries.size()) - 1);
    to = qBound(0, to, int(m_entries.size()) - 1);
    QSet<QString> names = add ? m_selected : QSet<QString>{};
    for (int row = qMin(from, to); row <= qMax(from, to); ++row)
        names.insert(m_entries[row].name);
    setSelected(names);
}

void FolderModel::selectRows(const QList<int> &rows) {
    QSet<QString> names;
    for (int row : rows)
        if (row >= 0 && row < m_entries.size())
            names.insert(m_entries[row].name);
    setSelected(names);
}

void FolderModel::selectAll() {
    QSet<QString> names;
    for (const Entry &e : m_entries)
        names.insert(e.name);
    setSelected(names);
}

void FolderModel::clearSelection() {
    setSelected({});
}

bool FolderModel::isSelected(int row) const {
    return row >= 0 && row < m_entries.size() && m_selected.contains(m_entries[row].name);
}

QStringList FolderModel::selectedPaths() const {
    // In the folder's order, not the order they were picked in.
    QStringList paths;
    for (const Entry &e : m_entries)
        if (m_selected.contains(e.name))
            paths << e.path;
    return paths;
}

int FolderModel::indexOf(const QString &name) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].name == name)
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
