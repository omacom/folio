#include "places.h"
#include "fileops.h"
#include "foldermodel.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QUrl>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStorageInfo>
#include <algorithm>

namespace {

// Home's own folders, all but the hidden ones (as refreshPlaces lists them),
// are already under Home.
bool listedUnderHome(const QString &path, const QString &home) {
    const QString clean = QDir::cleanPath(home);
    const QFileInfo info(path);
    return path == clean || (info.absolutePath() == clean && !info.isHidden());
}

// Omarchy's own folder reads as the name it goes by.
QString displayName(const QString &name) {
    return name == "omarchy" ? QStringLiteral("Omarchy") : name;
}

// Reads bookmark lines ("file:///path Label") into pinned entries, leaving
// out folders already listed under Home and missing folders.
QVariantList parseBookmarks(const QStringList &lines, const QString &home) {
    QVariantList pinned;
    QStringList seen;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        const int space = line.indexOf(' ');
        const QUrl url(space < 0 ? line : line.left(space));
        const QString label = space < 0 ? QString() : line.mid(space + 1).trimmed();
        // Remote bookmarks (sftp://, smb://) stay in the file for GTK, but
        // only local folders are pinned here for now.
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        // Home's visible folders are already under Home; Omarchy also seeds
        // a few of those as bookmarks for the file dialogs.
        if (listedUnderHome(path, home) || seen.contains(path) || !QFileInfo(path).isDir())
            continue;
        seen << path;
        pinned << QVariantMap{{"name", label.isEmpty() ? displayName(QFileInfo(path).fileName()) : label},
                              {"path", path}, {"icon", "folder"}};
    }
    return pinned;
}

} // namespace

Places::Places(const QString &home, QObject *parent)
    : QObject(parent), m_home(home.isEmpty() ? QDir::homePath() : home), m_realHome(home.isEmpty()) {
    connect(&m_tailscale, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QVariantList peers = code == 0 && status == QProcess::NormalExit
            ? parseTailscale(m_tailscale.readAllStandardOutput()) : QVariantList();
        if (peers != m_peers) {
            m_peers = peers;
            emit changed();
        }
    });
    connect(&m_dropbox, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QString status = parseDropboxStatus(m_dropbox.readAllStandardOutput());
        if (status != m_dropboxStatus) {
            m_dropboxStatus = status;
            refreshPlaces();
            emit changed();
        }
    });

    m_trashSettle.setSingleShot(true);
    m_trashSettle.setInterval(150);
    connect(&m_trashSettle, &QTimer::timeout, this, &Places::refreshTrash);
    connect(&m_trashWatcher, &QFileSystemWatcher::directoryChanged, &m_trashSettle, qOverload<>(&QTimer::start));

    // Tailnet peers come and go, drives get plugged in, Dropbox syncs: a
    // gentle poll keeps the sidebar honest without any background service.
    m_poll.setInterval(15000);
    connect(&m_poll, &QTimer::timeout, this, &Places::refresh);
    if (m_realHome)
        m_poll.start();
    refresh();
}

void Places::refresh() {
    refreshPlaces();
    refreshPinned();
    refreshDevices();
    // A drive plugged in may bring a trash of its own.
    refreshTrash();
    if (m_realHome) {
        pollTailscale();
        pollDropbox();
    }
    emit changed();
}

QString Places::userDirectory(int location, const QString &fallback) const {
    if (m_realHome) {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::StandardLocation(location));
        if (!path.isEmpty() && path != m_home)
            return path;
    }
    return m_home + "/" + fallback;
}

// The sidebar's Home section is every folder in home but the hidden ones,
// the way they appear in the home folder itself, so whatever someone keeps
// at the top of their home (Work, omarchy, Dropbox) is one click away
// without pinning it.
void Places::refreshPlaces() {
    QVariantList places;
    auto add = [&](const QString &name, const QString &path, const QString &icon, const QString &status = {}) {
        places << QVariantMap{{"name", name}, {"path", path}, {"icon", icon}, {"status", status}};
    };
    // The XDG folders keep their own glyphs, wherever user-dirs points.
    const QHash<QString, QString> icons{
        {userDirectory(QStandardPaths::DocumentsLocation, "Documents"), "documents"},
        {userDirectory(QStandardPaths::DownloadLocation, "Downloads"), "downloads"},
        {userDirectory(QStandardPaths::PicturesLocation, "Pictures"), "pictures"},
        {userDirectory(QStandardPaths::MusicLocation, "Music"), "music"},
        {userDirectory(QStandardPaths::MoviesLocation, "Videos"), "videos"},
        {m_home + "/Dropbox", "dropbox"},
    };

    QStringList names = QDir(m_home).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    for (const QString &name : names) {
        const QString path = m_home + "/" + name;
        const QString icon = icons.value(path, "folder");
        add(displayName(name), path, icon,
            icon == "dropbox" ? m_dropboxStatus : QString());
    }

    m_places = places;

    // Views: lists that aren't a folder of their own.
    m_views = {QVariantMap{{"name", tr("Recents")}, {"path", FolderModel::recentPath()}, {"icon", "recent"}},
               QVariantMap{{"name", tr("Trash")}, {"path", QDir::cleanPath(FileOps::homeTrash() + "/files")}, {"icon", "trash"}}};
}

bool Places::anyTrashed(const QStringList &trashDirs) {
    for (const QString &dir : trashDirs) {
        QDirIterator items(dir + "/files", QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        if (items.hasNext())
            return true;
    }
    return false;
}

void Places::refreshTrash() {
    const QStringList dirs = FileOps::trashDirs();
    // Each trash's files folder; and the trash itself, which sees files/
    // made, or the whole trash removed and made again by something that
    // empties it that way. With no home trash yet, where it will be made.
    QStringList watch;
    for (const QString &dir : dirs) {
        for (const QString &path : {dir + "/files", dir})
            if (QFileInfo(path).isDir())
                watch << path;
    }
    const QString home = FileOps::homeTrash();
    if (!QFileInfo(home).isDir() && QFileInfo(QFileInfo(home).absolutePath()).isDir())
        watch << QFileInfo(home).absolutePath();
    const QStringList watched = m_trashWatcher.directories();
    for (const QString &path : watched)
        if (!watch.contains(path))
            m_trashWatcher.removePath(path);
    for (const QString &path : watch)
        if (!watched.contains(path))
            m_trashWatcher.addPath(path);

    const bool full = anyTrashed(dirs);
    if (full != m_trashFull) {
        m_trashFull = full;
        emit trashChanged();
    }
}

QString Places::bookmarksPath() const {
    // Tests use a temporary home, whose bookmarks live inside it.
    const QString config = m_realHome ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                      : m_home + "/.config";
    return config + "/gtk-3.0/bookmarks";
}

QStringList Places::readBookmarkLines() const {
    QFile file(bookmarksPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    lines.removeAll(QString());
    return lines;
}

bool Places::writeBookmarkLines(const QStringList &lines) {
    QDir().mkpath(QFileInfo(bookmarksPath()).absolutePath());
    // Written beside the file and renamed over it, so GTK never reads half
    // a list.
    QSaveFile file(bookmarksPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write((lines.join('\n') + '\n').toUtf8());
    return file.commit();
}

void Places::refreshPinned() {
    m_pinned = parseBookmarks(readBookmarkLines(), m_home);
}

bool Places::isPinned(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    for (const QVariant &p : m_pinned)
        if (p.toMap().value("path").toString() == clean)
            return true;
    return false;
}

QString Places::pin(const QString &path) {
    const QString clean = QDir::cleanPath(path);
    const QFileInfo info(clean);
    if (!info.isDir())
        return tr("Only folders can be pinned.");
    if (listedUnderHome(clean, m_home))
        return tr("%1 is already under Home.").arg(displayName(info.fileName()));
    if (isPinned(clean))
        return {};
    QStringList lines = readBookmarkLines();
    lines << QString::fromUtf8(QUrl::fromLocalFile(clean).toEncoded());
    if (!writeBookmarkLines(lines))
        return tr("Couldn't save the pin to %1.").arg(bookmarksPath());
    refreshPinned();
    emit changed();
    return {};
}

void Places::unpin(const QString &path) {
    const QString clean = QDir::cleanPath(path);
    QStringList lines = readBookmarkLines();
    const int before = lines.size();
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const QString &line) {
        const QUrl url(line.section(' ', 0, 0));
        return url.isLocalFile() && QDir::cleanPath(url.toLocalFile()) == clean;
    }), lines.end());
    if (lines.size() != before && writeBookmarkLines(lines)) {
        refreshPinned();
        emit changed();
    }
}

QVariantList Places::physicalDrives(const QList<Mount> &mounts, const QString &systemDevice) {
    // Where the system itself lives; a drive mounted here is the computer,
    // not a device to browse to.
    static const QStringList systemRoots{"/", "/boot", "/efi", "/home", "/var", "/usr", "/opt", "/srv",
                                         "/root", "/tmp", "/nix", "/snap", "/swap"};
    static const QStringList pseudo{"tmpfs", "devtmpfs", "squashfs", "overlay", "ramfs", "fuse.portal",
                                    "fuse.gvfsd-fuse", "efivarfs", "autofs"};
    QVariantList drives;
    QStringList seen;
    for (const Mount &m : mounts) {
        const bool system = systemRoots.contains(m.root)
            || std::any_of(systemRoots.begin() + 1, systemRoots.end(),
                           [&](const QString &r) { return m.root.startsWith(r + "/"); })
            || m.root.startsWith("/.");
        // Only block devices; loop images (snaps, AppImages) and zram aren't
        // drives. Btrfs subvolumes of the system disk share its device.
        const bool block = m.device.startsWith("/dev/") && !m.device.startsWith("/dev/loop")
                           && !m.device.startsWith("/dev/zram");
        if (system || !block || pseudo.contains(m.type) || m.device == systemDevice || seen.contains(m.device))
            continue;
        seen << m.device;
        const QString name = m.label.isEmpty() ? QFileInfo(m.root).fileName() : m.label;
        drives << QVariantMap{{"name", name.isEmpty() ? m.root : name}, {"path", m.root}, {"icon", "drive"}};
    }
    return drives;
}

void Places::refreshDevices() {
    QList<Mount> mounts;
    auto volumes = QStorageInfo::mountedVolumes();
    // Shortest path first, so a drive mounted twice is listed at its top.
    std::sort(volumes.begin(), volumes.end(),
              [](const QStorageInfo &a, const QStorageInfo &b) { return a.rootPath().size() < b.rootPath().size(); });
    for (const QStorageInfo &v : volumes) {
        if (v.isValid() && v.isReady())
            mounts << Mount{v.rootPath(), QString::fromUtf8(v.device()), QString::fromUtf8(v.fileSystemType()),
                            v.name()};
    }
    m_devices = physicalDrives(mounts, QString::fromUtf8(QStorageInfo::root().device()));
}

void Places::pollTailscale() {
    if (m_tailscale.state() != QProcess::NotRunning)
        return;
    const QString tailscale = QStandardPaths::findExecutable("tailscale");
    if (tailscale.isEmpty())
        return;
    m_tailscale.start(tailscale, {"status", "--json"});
}

void Places::pollDropbox() {
    if (m_dropbox.state() != QProcess::NotRunning || !QFileInfo(m_home + "/Dropbox").isDir())
        return;
    const QString cli = QStandardPaths::findExecutable("dropbox-cli");
    if (cli.isEmpty())
        return;
    m_dropbox.start(cli, {"status"});
}

QVariantList Places::parseTailscale(const QByteArray &json) {
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QVariantList peers;
    const QJsonObject all = root.value("Peer").toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject peer = it.value().toObject();
        QString host = peer.value("DNSName").toString();
        if (host.endsWith('.'))
            host.chop(1);
        const QString name = peer.value("HostName").toString();
        // Only machines that can hold files and can be reached right now:
        // phones can't be browsed over SFTP, and offline machines would only
        // be dead ends. The 15-second poll brings them back when they wake.
        const QString os = peer.value("OS").toString();
        if (name.isEmpty() || host.isEmpty() || (os != "linux" && os != "macOS" && os != "windows")
            || !peer.value("Online").toBool())
            continue;
        peers << QVariantMap{{"name", name}, {"host", host}};
    }
    std::sort(peers.begin(), peers.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
    });
    return peers;
}

QString Places::parseDropboxStatus(const QByteArray &output) {
    const QString text = QString::fromUtf8(output).trimmed();
    if (text.isEmpty() || text.contains("isn't running", Qt::CaseInsensitive))
        return "stopped";
    if (text.startsWith("Up to date", Qt::CaseInsensitive))
        return "synced";
    return "syncing";
}
