#include "foldermodel.h"

#include <QCache>
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QDirListing>
#include <QXmlStreamReader>
#include <QUrl>
#include <QStandardPaths>
#include <QTimeZone>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QVarLengthArray>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>
#include <vector>
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

// Generations start at 1, so 0 stops any search or folder count still
// running for this model.
FolderModel::~FolderModel() {
    *m_latest = 0;
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
    recentListings().insert(cacheKey(), new QVector<Entry>(m_entries));
}

bool FolderModel::sameListing(const QVector<Entry> &a, const QVector<Entry> &b) {
    if (a.size() != b.size())
        return false;
    // By path, not name: a folder copied with cp -a or rsync lists the same
    // names, sizes and times as the one it came from.
    for (int i = 0; i < a.size(); ++i)
        // Name, subtitle, icon and details too: a plugin's item keeps its
        // path (its id) when they change.
        if (a[i].path != b[i].path || a[i].size != b[i].size || a[i].modified != b[i].modified
            || a[i].isDir != b[i].isDir || a[i].name != b[i].name || a[i].extension != b[i].extension
            || a[i].icon != b[i].icon || a[i].details != b[i].details)
            return false;
    return true;
}

void FolderModel::setEntries(const QVector<Entry> &entries) {
    beginResetModel();
    m_entries = entries;
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
    case KindRole: return typed(e).kind;
    case MimeRole: return typed(e).mime;
    case ExtensionRole: return typed(e).extension;
    case ItemCountRole: return e.itemCount;
    case SelectedRole: return m_selected.contains(e.path);
    case IconRole: return e.icon;
    }
    return {};
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {
        {NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"}, {SizeRole, "size"},
        {ModifiedRole, "modified"}, {KindRole, "kind"}, {MimeRole, "mime"},
        {ExtensionRole, "extension"}, {ItemCountRole, "itemCount"}, {SelectedRole, "selected"},
        {IconRole, "icon"},
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
    if (mime == "inode/directory") return QStringLiteral("folder");
    if (mime == "application/pdf") return QStringLiteral("pdf");
    if (mime.startsWith("image/")) return QStringLiteral("image");
    if (mime.startsWith("video/")) return QStringLiteral("video");
    if (mime.startsWith("audio/")) return QStringLiteral("audio");
    // Anything readable as text (source code, config, JSON, Ruby, ...) is
    // text, whatever its own type is called.
    static const QMimeDatabase types;
    if (mime.startsWith("text/") || types.mimeTypeForName(mime).inherits("text/plain"))
        return QStringLiteral("text");
    static const QStringList archives{"application/zip", "application/x-tar", "application/gzip",
                                      "application/x-xz", "application/zstd", "application/x-7z-compressed",
                                      "application/x-compressed-tar", "application/x-zstd-compressed-tar",
                                      "application/vnd.rar", "application/x-iso9660-image"};
    // Every compressed tar too: .tar.xz, .tar.bz2, .tar.lz4 and the rest.
    if (archives.contains(mime) || mime.endsWith("-compressed-tar")) return QStringLiteral("archive");
    return QStringLiteral("other");
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
                                              const QLocale &locale, bool typed) {
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
    // One string per type and extension, shared by every entry that has it.
    QSet<QString> shared;
    // Sorting by kind compares types, so they're read here.
    const bool typing = typed || sort == "kind";
    // Links unresolved, so broken ones, sockets and fifos are listed too, as entryFilters() has them.
    const auto flags = showHidden ? QDirListing::IteratorFlag::IncludeHidden : QDirListing::IteratorFlag::Default;
    for (const QDirListing::DirEntry &item : QDirListing(path, flags)) {
        if (stop && stop())
            return {};
        Entry e;
        if (typing || !item.isFile()) {
            e = entryFor(item.fileInfo(), mimes);
            e.mime = *shared.insert(e.mime);
            if (!e.extension.isEmpty())
                e.extension = *shared.insert(e.extension);
        } else {
            // A regular file's type comes from its name alone, so it can wait until it's asked for.
            e.name = item.fileName();
            e.path = item.absoluteFilePath();
            e.size = item.size();
            e.modified = item.lastModified(QTimeZone::LocalTime);
        }
        entries.push_back(std::move(e));
    }
    // Growing left room for many more entries than were read; the listing is kept, so give it back.
    entries.squeeze();
    sortEntries(entries, sort, locale);
    return entries;
}

FolderModel::Entry FolderModel::typed(const Entry &entry) {
    // A plugin's item has a kind and no file type: its name says nothing about it.
    if (!entry.mime.isNull() || !entry.kind.isNull())
        return entry;
    Entry e = entry;
    e.mime = QMimeDatabase().mimeTypeForFile(e.name, QMimeDatabase::MatchExtension).name();
    e.kind = kindForMime(e.mime);
    e.extension = QFileInfo(e.name).suffix().toUpper();
    return e;
}

FolderModel::Entry FolderModel::entryFor(const QFileInfo &info, const QMimeDatabase &mimes) {
    Entry e;
    e.name = info.fileName();
    e.path = info.absoluteFilePath();
    e.isDir = info.isDir();
    e.size = e.isDir ? 0 : info.size();
    e.modified = info.lastModified();
    // A regular file is typed by its name alone; handing Qt the QFileInfo stats it once more first.
    e.mime = e.isDir ? QStringLiteral("inode/directory")
           : info.isFile() ? mimes.mimeTypeForFile(e.name, QMimeDatabase::MatchExtension).name()
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

bool FolderModel::mergeRuns(QVector<Entry> &entries, const std::function<bool(const Entry &, const Entry &)> &less) {
    constexpr qsizetype mostRuns = 64; // merging 64 runs takes 6 compares an entry, a third of sorting 100,000
    // Where each run starts, then where the last one ends.
    QVarLengthArray<QVector<Entry>::iterator, mostRuns + 1> bounds{entries.begin()};
    for (auto at = entries.begin(); at != entries.end();) {
        if (bounds.size() > mostRuns)
            return false;
        at = std::is_sorted_until(at, entries.end(), less);
        bounds.push_back(at);
    }
    // Neighbouring runs merge in pairs until one is left; a merge puts its left run's equals first, so ties keep their order.
    while (bounds.size() > 2) {
        qsizetype merged = 1;
        for (qsizetype i = 0; i + 2 < bounds.size(); i += 2) {
            std::inplace_merge(bounds[i], bounds[i + 1], bounds[i + 2], less);
            bounds[merged++] = bounds[i + 2];
        }
        if (bounds.size() % 2 == 0) // an odd run out keeps its end
            bounds[merged++] = bounds.back();
        bounds.resize(merged);
    }
    return true;
}

void FolderModel::sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale) {
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    // `collate` compares the two names, or with true the two extensions.
    const auto before = [&sort](const Entry &a, const Entry &b, const auto &collate) {
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
                return collate(true) < 0;
        }
        return collate(false) < 0;
    };
    const auto less = [&](const Entry &a, const Entry &b) {
        return before(a, b, [&](bool extension) {
            return extension ? collator.compare(a.extension, b.extension) : collator.compare(a.name, b.name);
        });
    };
    // A folder listed in the order it was written, as btrfs gives one back, is in a few runs already.
    if (mergeRuns(entries, less))
        return;
    // Keys pay where names decide the order, and outside the C locale, whose keys keep the case its compare ignores.
    if ((sort != "name" && sort != "kind") || collator.locale().language() == QLocale::C) {
        std::stable_sort(entries.begin(), entries.end(), less);
        return;
    }
    // Each name and extension made into a key once, so a compare reads bytes instead of collating both strings.
    std::vector<QCollatorSortKey> names, extensions;
    names.reserve(entries.size());
    for (const Entry &e : std::as_const(entries))
        names.push_back(collator.sortKey(e.name));
    if (sort == "kind") {
        extensions.reserve(entries.size());
        for (const Entry &e : std::as_const(entries))
            extensions.push_back(collator.sortKey(e.extension));
    }
    std::vector<qsizetype> order(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](qsizetype i, qsizetype j) {
        return before(entries.at(i), entries.at(j), [&](bool extension) {
            return extension ? extensions[i].compare(extensions[j]) : names[i].compare(names[j]);
        });
    });
    QVector<Entry> sorted;
    sorted.reserve(entries.size());
    for (qsizetype i : order)
        sorted.push_back(std::move(entries[i]));
    entries = std::move(sorted);
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
    entry.itemCount = 0;
    entry.peek.clear();

    static const QStringList pictures{"jpg", "jpeg", "png", "webp", "gif", "heic", "avif", "jxl"};
    constexpr qsizetype peekSize = 2; // pictures the preview shows of a folder
    // The first pictures in QDir::Name order (a case-sensitive compare), kept in one pass instead of sorting every name.
    QStringList firsts;
    for (QDirIterator it(entry.path, entryFilters(showHidden)); it.hasNext();) {
        it.next();
        ++entry.itemCount;
        const QString name = it.fileName();
        if ((firsts.size() == peekSize && name > firsts.last()) || !pictures.contains(QFileInfo(name).suffix().toLower()))
            continue;
        firsts.insert(std::lower_bound(firsts.cbegin(), firsts.cend(), name) - firsts.cbegin(), name);
        if (firsts.size() > peekSize)
            firsts.removeLast();
    }
    const QDir dir(entry.path);
    for (const QString &name : std::as_const(firsts))
        entry.peek << dir.filePath(name);
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
        const bool same = error == m_error && !m_entries.isEmpty() && sameListing(m_entries, entries);
        if (!same)
            setEntries(entries);
        if (!m_selected.isEmpty()) {
            QSet<QString> still;
            for (const Entry &e : std::as_const(m_entries))
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
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run([path, showHidden, sort, query, items, latest, generation, locale] {
        QString error;
        const auto overtaken = [&] { return *latest != generation; };
        auto entries = isPluginView(path) ? listItems(items, query)
                     : query.isEmpty() || path == recentPath() ? list(path, showHidden, sort, &error, overtaken, locale, false)
                     : search(path, query, showHidden, sort, overtaken, 1000, locale);
        return qMakePair(entries, error);
    }));
}

void FolderModel::countFolders(quint64 generation) {
    // Only the folders not counted yet: a listing just read, or one whose
    // counting was overtaken by a re-read (and so remembered without them).
    QVector<QPair<int, Entry>> folders;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries.at(row).isDir && m_entries.at(row).itemCount < 0)
            folders.push_back({row, m_entries.at(row)});
    if (folders.isEmpty())
        return;

    const bool showHidden = m_showHidden;
    auto *watcher = new QFutureWatcher<QVector<QPair<int, Entry>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        bool updated = false;
        for (const auto &[row, summary] : watcher->result()) {
            if (row >= m_entries.size() || m_entries.at(row).path != summary.path)
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
    return map;
}

QVariantMap FolderModel::toMap(const Entry &listed) {
    const Entry entry = typed(listed);
    return {
        {"name", entry.name}, {"path", entry.path}, {"isDir", entry.isDir}, {"size", entry.size},
        {"modified", entry.modified}, {"kind", entry.kind}, {"mime", entry.mime},
        {"extension", entry.extension}, {"itemCount", entry.itemCount}, {"icon", entry.icon},
        {"details", entry.details},
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
    setSelected(row >= 0 && row < m_entries.size() ? QSet<QString>{m_entries.at(row).path} : QSet<QString>{});
}

void FolderModel::toggleSelected(int row) {
    if (row < 0 || row >= m_entries.size())
        return;
    QSet<QString> paths = m_selected;
    const QString path = m_entries.at(row).path;
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
        paths.insert(m_entries.at(row).path);
    setSelected(paths);
}

void FolderModel::selectRows(const QList<int> &rows) {
    QSet<QString> paths;
    for (int row : rows)
        if (row >= 0 && row < m_entries.size())
            paths.insert(m_entries.at(row).path);
    setSelected(paths);
}

void FolderModel::selectAll() {
    QSet<QString> paths;
    for (const Entry &e : std::as_const(m_entries))
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

int FolderModel::indexOf(const QString &name) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].name == name)
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
