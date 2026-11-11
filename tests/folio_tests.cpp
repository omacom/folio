#include <QDir>
#include <QMimeData>
#include <memory>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <sys/stat.h>

#include "backend.h"
#include "fileops.h"
#include "foldermodel.h"
#include "places.h"
#include "previewer.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

class FolioTests : public QObject {
    Q_OBJECT

private slots:
    void folderListsDirectoriesFirstWithoutHidden()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("zeta");
        write(dir.filePath("alpha.md"), "# hi");
        write(dir.filePath("photo10.jpg"), "x");
        write(dir.filePath("photo9.jpg"), "x");
        write(dir.filePath(".secret"), "x");

        QString error;
        const auto entries = FolderModel::list(dir.path(), false, "name", &error);
        QVERIFY(error.isEmpty());
        QStringList names;
        for (const auto &e : entries) names << e.name;
        // Folders first, then natural order, so photo9 comes before photo10.
        QCOMPARE(names, QStringList({"zeta", "alpha.md", "photo9.jpg", "photo10.jpg"}));
        QCOMPARE(entries.at(0).kind, QString("folder"));
        QCOMPARE(entries.at(1).kind, QString("text"));
        QCOMPARE(entries.at(2).kind, QString("image"));
        QCOMPARE(entries.at(2).extension, QString("JPG"));

        QCOMPARE(FolderModel::list(dir.path(), true, "name", &error).size(), 5);
    }

    void folderReportsMissingFolders()
    {
        QString error;
        QVERIFY(FolderModel::list("/definitely/not/here", false, "name", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void folderSummaryCountsAndPeeks()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.jpg"), "x");
        write(dir.filePath("b.png"), "x");
        write(dir.filePath("c.png"), "x");
        write(dir.filePath("notes.txt"), "x");
        FolderModel::Entry entry;
        entry.path = dir.path();
        FolderModel::summarize(entry, false);
        QCOMPARE(entry.itemCount, 4);
        QCOMPARE(entry.peek.size(), 2);
    }

    void folderSummaryPeeksTheFirstPicturesByName()
    {
        // Written in neither name order nor its reverse, so a listing in creation order, either way, gives the wrong two.
        QTemporaryDir dir;
        QDir(dir.path()).mkdir(".shots.png");
        for (int i = 20; i > 0; --i)
            write(dir.filePath(QString("note%1.txt").arg(i)), "x");
        for (const char *name : {"photo10.jpg", "\xc3\x89lan.png", "Photo9.JPG", "photo9.jpg"})
            write(dir.filePath(QString::fromUtf8(name)), "x");
        QVERIFY(QFile::link(dir.filePath("missing"), dir.filePath("gone")));
        QVERIFY(QFile::link(dir.filePath(".shots.png"), dir.filePath("link")));
        QCOMPARE(::mkfifo(QFile::encodeName(dir.filePath("pipe")).constData(), 0600), 0);

        // Name order is a case-sensitive compare: capitals first, photo10 before photo9, accents after z.
        FolderModel::Entry entry;
        entry.path = dir.path();
        FolderModel::summarize(entry, false);
        QCOMPARE(entry.itemCount, 27);
        QCOMPARE(entry.peek, QStringList({dir.filePath("Photo9.JPG"), dir.filePath("photo10.jpg")}));
        // A folder named like a picture peeks like one.
        FolderModel::summarize(entry, true);
        QCOMPARE(entry.itemCount, 28);
        QCOMPARE(entry.peek, QStringList({dir.filePath(".shots.png"), dir.filePath("Photo9.JPG")}));
    }

    void folderModelLoadsInTheBackground()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.get(0).value("name").toString(), QString("one.txt"));
        QCOMPARE(model.indexOf("one.txt"), 0);
    }

    void folderSearchesEveryFolderBelow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos/2024");
        QDir(dir.path()).mkpath(".cache");
        write(dir.filePath("beach notes.txt"), "x");
        write(dir.filePath("Photos/2024/Beach.jpg"), "x");
        write(dir.filePath("Photos/city.jpg"), "x");
        write(dir.filePath(".cache/beach.tmp"), "x");

        auto names = [&](bool hidden) {
            QStringList out;
            for (const auto &e : FolderModel::search(dir.path(), "beach", hidden, "name"))
                out << e.name;
            return out.join(", ");
        };
        // Named by where they are, ignoring case, hidden ones only when shown.
        QCOMPARE(names(false), QString("beach notes.txt, Photos/2024/Beach.jpg"));
        QCOMPARE(names(true), QString(".cache/beach.tmp, beach notes.txt, Photos/2024/Beach.jpg"));
        // Stopped, it gives nothing rather than half an answer.
        QVERIFY(FolderModel::search(dir.path(), "beach", false, "name", [] { return true; }).isEmpty());

        // In the model, a query swaps the folder for its matches; clearing
        // it, or going elsewhere, brings the folder back.
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.setQuery("BEACH");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.get(1).value("path").toString(), dir.filePath("Photos/2024/Beach.jpg"));
        model.setQuery("");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.indexOf("Photos"), 0);
        model.setQuery("city");
        QVERIFY(loaded.wait(3000));
        model.setPath(dir.filePath("Photos"));
        QVERIFY(model.query().isEmpty());
    }

    void folderModelSwapsFoldersWithoutAnEmptyFrame()
    {
        QTemporaryDir a, b;
        write(a.filePath("one.txt"), "x");
        write(b.filePath("two.txt"), "x");
        write(b.filePath("three.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));

        // Moving on keeps the old entries until the new ones are in.
        QSignalSpy counts(&model, &FolderModel::countChanged);
        model.setPath(b.path());
        QCOMPARE(model.count(), 1);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(counts.count(), 1); // straight from one to two, never zero

        // A column with no folder clears at once.
        model.setPath(QString());
        QCOMPARE(model.count(), 0);

        // Going back to a folder just seen shows it in the same step, and
        // the background re-read leaves it alone when nothing changed.
        loaded.clear();
        model.setPath(a.path());
        QCOMPARE(model.count(), 1);
        QCOMPARE(loaded.count(), 1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.count(), 1);
    }

    void fileOpsNeverOverwrite()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.tar.gz"), "x");
        QDir(dir.path()).mkdir("Photos");
        QCOMPARE(FileOps::freeName(dir.path(), "fresh.txt"), QString("fresh.txt"));
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy).tar.gz"));
        write(dir.filePath("notes (copy).tar.gz"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy 2).tar.gz"));
        QCOMPARE(FileOps::freeName(dir.path(), "Photos"), QString("Photos (copy)"));
    }

    void fileOpsCopyAndMove()
    {
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        write(from.filePath("todo.txt"), "list");
        write(to.filePath("todo.txt"), "already here");

        QString error;
        QStringList made = FileOps::transfer({from.filePath("album"), from.filePath("todo.txt")}, to.path(), false, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"album", "todo (copy).txt"}));
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        QFile kept(to.filePath("todo.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("already here"));
        QVERIFY(QFile::exists(from.filePath("todo.txt"))); // copied, not moved

        made = FileOps::transfer({from.filePath("todo.txt")}, to.path(), true, &error);
        QCOMPARE(made, QStringList({"todo (copy 2).txt"}));
        QVERIFY(!QFile::exists(from.filePath("todo.txt")));

        // A folder can't go inside itself.
        error.clear();
        FileOps::transfer({to.filePath("album")}, to.filePath("album/inner"), true, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
    }

    void fileOpsClipboardFormats()
    {
        std::unique_ptr<QMimeData> data(FileOps::mimeDataFor({"/tmp/a b.txt", "/tmp/c.txt"}, true));
        QCOMPARE(data->text(), QString("/tmp/a b.txt\n/tmp/c.txt"));
        QVERIFY(data->data("x-special/gnome-copied-files").startsWith("cut\nfile:///tmp/a%20b.txt"));
        bool cut = false;
        QCOMPARE(FileOps::pathsFrom(data.get(), &cut), QStringList({"/tmp/a b.txt", "/tmp/c.txt"}));
        QVERIFY(cut);

        // From another app that only offers a URI list: a copy.
        QMimeData plain;
        plain.setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QCOMPARE(FileOps::pathsFrom(&plain, &cut), QStringList({"/tmp/x.png"}));
        QVERIFY(!cut);
    }

    void recentsReadGtkRecentFiles()
    {
        QTemporaryDir dir;
        write(dir.filePath("old.txt"), "x");
        write(dir.filePath("new.pdf"), "x");
        write(dir.filePath(".secret"), "x");
        const QByteArray url = QUrl::fromLocalFile(dir.path()).toEncoded();
        write(dir.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/old.txt\" modified=\"2026-01-01T10:00:00Z\" visited=\"2026-01-02T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/gone.txt\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/.secret\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"https://example.com/\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/new.pdf\" modified=\"2026-03-01T10:00:00Z\" visited=\"2026-03-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto entries = FolderModel::listRecent(dir.filePath("recently-used.xbel"), false);
        // Missing, hidden and web entries are left out; newest use first.
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).name, QString("new.pdf"));
        QCOMPARE(entries.at(1).name, QString("old.txt"));
        QCOMPARE(entries.at(0).kind, QString("pdf"));
    }

    void folderSortsFiveWays()
    {
        QVector<FolderModel::Entry> e(4);
        e[0].name = "b.txt"; e[0].kind = "text"; e[0].extension = "TXT"; e[0].size = 10; e[0].modified = QDateTime::fromSecsSinceEpoch(300);
        e[1].name = "a.png"; e[1].kind = "image"; e[1].extension = "PNG"; e[1].size = 500; e[1].modified = QDateTime::fromSecsSinceEpoch(100);
        e[2].name = "c.txt"; e[2].kind = "text"; e[2].extension = "TXT"; e[2].size = 99; e[2].modified = QDateTime::fromSecsSinceEpoch(200);
        e[3].name = "Zeta"; e[3].isDir = true; e[3].modified = QDateTime::fromSecsSinceEpoch(50);
        auto order = [&](const QString &sort) {
            auto copy = e;
            FolderModel::sortEntries(copy, sort);
            QStringList names;
            for (const auto &x : copy) names << x.name;
            return names.join(' ');
        };
        QCOMPARE(order("name"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("kind"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("modified"), QString("Zeta b.txt c.txt a.png"));
        QCOMPARE(order("oldest"), QString("Zeta a.png c.txt b.txt"));
        QCOMPARE(order("size"), QString("Zeta a.png c.txt b.txt"));
    }

    void folderSelectsByName()
    {
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt", "d.txt"})
            write(dir.filePath(name), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));

        model.selectOnly(1);
        QCOMPARE(model.selectedCount(), 1);
        model.selectRange(1, 3, false);
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("b.txt"), dir.filePath("c.txt"), dir.filePath("d.txt")}));
        model.toggleSelected(2);
        QCOMPARE(model.selectedCount(), 2);
        QVERIFY(!model.isSelected(2));
        model.selectRange(0, 0, true);
        QCOMPARE(model.selectedCount(), 3);

        // A file that goes away leaves the selection; the rest stay.
        QFile::remove(dir.filePath("b.txt"));
        model.reload();
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("a.txt"), dir.filePath("d.txt")}));

        model.selectAll();
        QCOMPARE(model.selectedCount(), 3);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
    }

    void trashRestoresAndEmpties()
    {
        QTemporaryDir home;
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        QDir().mkpath(home.filePath("Docs"));
        auto trashed = [&](const QString &name, const QString &from) {
            write(trash + "/files/" + name, "x");
            write(trash + "/info/" + name + ".trashinfo",
                  "[Trash Info]\nPath=" + QUrl::toPercentEncoding(from, "/") + "\nDeletionDate=2026-11-10T12:00:00\n");
        };
        trashed("notes 1.txt", home.filePath("Docs/notes 1.txt"));
        trashed("gone.txt", home.filePath("Gone/Deep/gone.txt"));
        trashed("clash.txt", home.filePath("Docs/clash.txt"));
        write(home.filePath("Docs/clash.txt"), "newer");

        QCOMPARE(FileOps::readOriginalPath(trash + "/files/notes 1.txt"), home.filePath("Docs/notes 1.txt"));
        QCOMPARE(FileOps::readDeletionDate(trash + "/files/notes 1.txt"), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));

        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/notes 1.txt", &error), home.filePath("Docs/notes 1.txt"));
        QVERIFY(!QFile::exists(trash + "/info/notes 1.txt.trashinfo"));
        // A folder that's gone is made again; a name that's taken isn't overwritten.
        QCOMPARE(FileOps::restoreOne(trash + "/files/gone.txt", &error), home.filePath("Gone/Deep/gone.txt"));
        QCOMPARE(FileOps::restoreOne(trash + "/files/clash.txt", &error), home.filePath("Docs/clash (copy).txt"));
        QFile kept(home.filePath("Docs/clash.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("newer"));

        trashed("a.txt", home.filePath("a.txt"));
        QDir().mkpath(trash + "/files/folder/inner");
        write(trash + "/info/folder.trashinfo", "[Trash Info]\nPath=" + home.filePath("folder").toUtf8() + "\n");
        write(trash + "/info/orphan.trashinfo", "[Trash Info]\nPath=/nowhere\n");
        QCOMPARE(FileOps::emptyTrashDir(trash), 2);
        QVERIFY(QDir(trash + "/files").isEmpty());
        QVERIFY(QDir(trash + "/info").isEmpty());
    }

    void kindsFromMimeTypes()
    {
        QCOMPARE(FolderModel::kindForMime("application/pdf"), QString("pdf"));
        QCOMPARE(FolderModel::kindForMime("video/mp4"), QString("video"));
        QCOMPARE(FolderModel::kindForMime("audio/mp4"), QString("audio"));
        QCOMPARE(FolderModel::kindForMime("application/zip"), QString("archive"));
        QCOMPARE(FolderModel::kindForMime("application/octet-stream"), QString("other"));
    }

    void backendKeepsHistoryAndCrumbs()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Pictures/Trip");
        Backend backend(home.path());
        backend.navigate(home.path());
        backend.navigate(home.filePath("Pictures/Trip"));
        QCOMPARE(backend.title(), QString("Trip"));
        const auto crumbs = backend.crumbs();
        QCOMPARE(crumbs.size(), 3);
        QCOMPARE(crumbs.first().toMap().value("name").toString(), QString("~"));

        backend.goUp();
        QCOMPARE(backend.path(), home.filePath("Pictures"));
        // Coming back up selects the folder we left.
        QCOMPARE(backend.takeSelection(), QString("Trip"));
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Pictures/Trip"));
        QVERIFY(backend.canForward());
    }

    void backendCompletesTypedPaths()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Work/forge", "Documents", ".config"})
            QDir(home.path()).mkpath(dir);
        const QString h = home.path();
        QCOMPARE(Backend::expandTyped("~/Work", h, h), h + "/Work");
        QCOMPARE(Backend::expandTyped("Work/folio", h, h), h + "/Work/folio");
        QCOMPARE(Backend::expandTyped("/etc", h, h), QString("/etc"));
        QCOMPARE(Backend::complete("~/Doc", h, h), QString("~/Documents/"));
        QCOMPARE(Backend::complete("~/Work/fo", h, h), QString("~/Work/fo"));   // folio or forge
        QCOMPARE(Backend::complete("~/Work/fol", h, h), QString("~/Work/folio/"));
        QCOMPARE(Backend::complete("~/.co", h, h), QString("~/.config/"));
        QCOMPARE(Backend::complete("~/c", h, h), QString("~/c"));               // hidden stays hidden
        QCOMPARE(Backend::complete("Wo", h, h), QString("Work/"));
    }

    void backendShowItemSelectsTheFile()
    {
        QTemporaryDir home;
        write(home.filePath("report.pdf"), "x");
        Backend backend(home.path());
        backend.showItem(home.filePath("report.pdf"));
        QCOMPARE(backend.path(), home.path());
        QCOMPARE(backend.takeSelection(), QString("report.pdf"));
    }

    void placesListEveryFolderInHome()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work", "Documents", "omarchy", ".config", "project10", "project9"})
            QDir(home.path()).mkpath(dir);
        write(home.filePath("notes.txt"), "not a folder");
        auto names = [](const Places &p) {
            QStringList list;
            for (const auto &v : p.places()) list << v.toMap().value("name").toString();
            return list;
        };
        QCOMPARE(names(Places(home.path())),
                 QStringList({"Documents", "Omarchy", "project9", "project10", "Work"}));

        // Dropbox shows once its folder exists, with its own mark.
        QDir(home.path()).mkpath("Dropbox");
        const Places withDropbox(home.path());
        QCOMPARE(names(withDropbox).at(1), QString("Dropbox"));
        QCOMPARE(withDropbox.places().at(1).toMap().value("icon").toString(), QString("dropbox"));
        QCOMPARE(withDropbox.places().at(0).toMap().value("icon").toString(), QString("documents"));
        QCOMPARE(withDropbox.places().at(5).toMap().value("icon").toString(), QString("folder"));
    }

    void placesPinFoldersAsGtkBookmarks()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Downloads", "Work/hype"})
            QDir(home.path()).mkpath(dir);
        const QString bookmarks = home.filePath(".config/gtk-3.0/bookmarks");
        QDir().mkpath(QFileInfo(bookmarks).absolutePath());
        // Omarchy's seeded home folders and a remote bookmark are kept in
        // the file but not shown as pins.
        write(bookmarks, QUrl::fromLocalFile(home.filePath("Downloads")).toEncoded() + " Downloads\n"
                         "sftp://studio/srv Studio\n"
                         + QUrl::fromLocalFile(home.filePath("Work/hype")).toEncoded() + " Slides\n");

        Places places(home.path());
        QCOMPARE(places.pinned().size(), 1);
        QCOMPARE(places.pinned().at(0).toMap().value("name").toString(), QString("Slides"));

        QCOMPARE(places.pin(home.filePath("Work/folio")), QString());
        QVERIFY(places.isPinned(home.filePath("Work/folio")));
        QCOMPARE(places.pinned().size(), 2);
        QVERIFY(!places.pin(home.filePath("Downloads")).isEmpty()); // already under Home

        places.unpin(home.filePath("Work/hype"));
        QFile file(bookmarks);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("sftp://studio/srv Studio"));
        QVERIFY(saved.contains("Downloads"));
        QVERIFY(saved.contains("Work/folio"));
        QVERIFY(!saved.contains("Work/hype"));
        QCOMPARE(places.pinned().size(), 1);
    }

    void placesParseTailscalePeers()
    {
        const QByteArray json = R"({"Peer":{
            "a":{"HostName":"nas","DNSName":"nas.example.ts.net.","OS":"linux","Online":false},
            "b":{"HostName":"studio","DNSName":"studio.example.ts.net.","OS":"linux","Online":true},
            "c":{"HostName":"phone","DNSName":"phone.example.ts.net.","OS":"iOS","Online":true}}})";
        const auto peers = Places::parseTailscale(json);
        // Offline machines and phones are left out.
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers.at(0).toMap().value("name").toString(), QString("studio"));
        QCOMPARE(peers.at(0).toMap().value("host").toString(), QString("studio.example.ts.net"));
    }

    void placesListOnlyPhysicalDrives()
    {
        const QList<Places::Mount> mounts{
            {"/", "/dev/mapper/root", "btrfs", ""},
            {"/home", "/dev/mapper/root", "btrfs", ""},
            {"/.snapshots", "/dev/mapper/root", "btrfs", ""},
            {"/boot", "/dev/nvme0n1p1", "vfat", "EFI"},
            {"/tmp", "tmpfs", "tmpfs", ""},
            {"/var/lib/snapd/snap/core", "/dev/loop3", "squashfs", ""},
            {"/mnt/archive", "/dev/sda1", "ext4", ""},
            {"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"},
            {"/run/media/me/Photos/again", "/dev/sdb1", "exfat", "Photos"},
            {"/run/user/1000/gvfs", "gvfsd-fuse", "fuse.gvfsd-fuse", ""},
        };
        const auto drives = Places::physicalDrives(mounts, "/dev/mapper/root");
        QCOMPARE(drives.size(), 2);
        QCOMPARE(drives.at(0).toMap().value("name").toString(), QString("archive"));
        QCOMPARE(drives.at(1).toMap().value("name").toString(), QString("Photos"));
        QCOMPARE(drives.at(1).toMap().value("path").toString(), QString("/run/media/me/Photos"));
    }

    void placesParseDropboxStatus()
    {
        QCOMPARE(Places::parseDropboxStatus("Up to date\n"), QString("synced"));
        QCOMPARE(Places::parseDropboxStatus("Syncing 3 files\n"), QString("syncing"));
        QCOMPARE(Places::parseDropboxStatus("Dropbox isn't running!\n"), QString("stopped"));
    }

    void thumbnailsFollowTheFreedesktopCache()
    {
        QTemporaryDir dir;
        QImage picture(800, 400, QImage::Format_RGB32);
        picture.fill(Qt::red);
        const QString file = dir.filePath("wide.png");
        QVERIFY(picture.save(file));

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        const QImage thumb = thumbnails.load(file, "image/png", 256);
        QCOMPARE(thumb.size(), QSize(256, 128));

        // Cached under the spec's name, tagged so other apps trust it.
        const QString cached = dir.filePath("cache/large/" + Thumbnails::cacheKey(file));
        QVERIFY(QFile::exists(cached));
        const QImage stored(cached);
        QCOMPARE(stored.text("Thumb::URI"), QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
        QVERIFY(!stored.text("Thumb::MTime").isEmpty());

        // A fresh cache entry is served as is: overwrite it with a marker
        // color and the next load must return the cached pixels.
        QImage marked = stored;
        marked.fill(Qt::blue);
        QVERIFY(marked.save(cached));
        QCOMPARE(thumbnails.load(file, "image/png", 256).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(Thumbnails::bucketFor(128), QString("normal"));
        QCOMPARE(Thumbnails::bucketFor(512), QString("x-large"));
    }

    void thumbnailsReadInstalledThumbnailers()
    {
        QTemporaryDir dir;
        write(dir.filePath("demo.thumbnailer"),
              "[Thumbnailer Entry]\nTryExec=/bin/sh\nExec=/bin/sh -c \"exit 1\" %i %o %s\nMimeType=video/mp4;video/webm;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        QVERIFY(thumbnails.thumbnailerFor("video/webm").startsWith("/bin/sh"));
        QVERIFY(thumbnails.thumbnailerFor("video/ogg").isEmpty());

        // A thumbnailer that fails leaves a failure marker, so it isn't retried.
        write(dir.filePath("clip.mp4"), "not a video");
        QVERIFY(thumbnails.load(dir.filePath("clip.mp4"), "video/mp4", 128).isNull());
        QVERIFY(QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("clip.mp4")))));
    }

    void previewerPicksAViewerForEachType()
    {
        QCOMPARE(Previewer::viewerFor("image/png", false), QString("image"));
        QCOMPARE(Previewer::viewerFor("image/gif", false), QString("animated"));
        QCOMPARE(Previewer::viewerFor("image/x-canon-cr2", false), QString("thumbnail"));
        QCOMPARE(Previewer::viewerFor("video/mp4", false), QString("video"));
        QCOMPARE(Previewer::viewerFor("audio/mpeg", false), QString("audio"));
        QCOMPARE(Previewer::viewerFor("application/pdf", false), QString("pdf"));
        QCOMPARE(Previewer::viewerFor("text/markdown", false), QString("markdown"));
        QCOMPARE(Previewer::viewerFor("text/x-c++src", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/x-shellscript", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/zip", false), QString("other"));
        QCOMPARE(Previewer::viewerFor("inode/directory", true), QString("folder"));
    }

    void previewerReadsTextButNotBinaries()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello\nworld\n");
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt")), QString("hello\nworld\n"));
        QVERIFY(Previewer::readText(dir.filePath("blob.txt")).isEmpty());
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt"), 5), QString("hello"));

        // A binary with a text name falls back to the plain file view.
        Previewer previewer;
        previewer.start(dir.filePath("blob.txt"), false, "name");
        QCOMPARE(previewer.viewer(), QString("other"));
    }

    void previewerStepsThroughTheFolder()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("a.md"), "# A");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.pdf"), "%PDF");
        write(dir.filePath(".hidden"), "h");

        Previewer previewer;
        QSignalSpy changed(&previewer, &Previewer::changed);
        previewer.start(dir.filePath("b.txt"), false, "name");
        // Shown at once, before the folder is listed.
        QCOMPARE(previewer.item().value("name").toString(), QString("b.txt"));
        QCOMPARE(previewer.text(), QString("b"));

        QTRY_COMPARE(previewer.count(), 4);
        QCOMPARE(previewer.index(), 2);
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("markdown"));
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("folder"));
        QCOMPARE(previewer.item().value("itemCount").toInt(), 0);
        previewer.previous();
        QCOMPARE(previewer.index(), 0);
        previewer.next();
        previewer.next();
        previewer.next();
        QCOMPARE(previewer.viewer(), QString("pdf"));
        previewer.next();
        QCOMPARE(previewer.index(), 3);

        // Folio pointing at a sibling keeps the list; another folder starts over.
        previewer.show(dir.filePath("a.md"));
        QCOMPARE(previewer.index(), 1);
        QCOMPARE(previewer.count(), 4);
        previewer.show(dir.filePath("album"));
        QCOMPARE(previewer.index(), 0);
        QVERIFY(changed.count() > 0);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#FFFCF0\"\nforeground = \"#100F0F\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#FFFCF0"));
        // Missing derived backgrounds fall back to the base.
        QCOMPARE(theme.darkBackground(), QColor("#FFFCF0"));
        QVERIFY(theme.light());
    }

    void themeDerivesALighterBackgroundWhenItMatches()
    {
        QTemporaryDir dir;
        write(dir.filePath("colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#101315\"\n");
        Theme same(dir.path(), nullptr, "/nonexistent-hyprctl");
        QVERIFY(same.lighterBackground() != same.background());
        QVERIFY(same.lighterBackground().lightnessF() > same.background().lightnessF());

        write(dir.filePath("colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#232329\"\n");
        Theme distinct(dir.path(), nullptr, "/nonexistent-hyprctl");
        QCOMPARE(distinct.lighterBackground(), QColor("#232329"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Open").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n items": {"one": "%n item", "other": "%n items"}})");
        write(dir.path() + "/ru.json", R"({"%n items": {"one": "%n элемент", "few": "%n элемента", "many": "%n элементов", "other": "%n элемента"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 21), QString("%n элемент"));
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 25), QString("%n элементов"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
