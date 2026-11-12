#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

// The sidebar: the user's places (with Dropbox first-class when it's set up),
// the drives, and the machines on the tailnet. Each list is small, so it is
// handed to QML as plain maps rather than a model.
class Places : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY changed)
    Q_PROPERTY(QVariantList views READ views NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantList peers READ peers NOTIFY changed)
    Q_PROPERTY(QString dropboxStatus READ dropboxStatus NOTIFY changed)
    // Whether anything is in the trash (the home one or a drive's), for the
    // sidebar's full or empty trash. Its own signal, so the sidebar's lists
    // aren't rebuilt every time something is trashed.
    Q_PROPERTY(bool trashFull READ trashFull NOTIFY trashChanged)

public:
    // `home` is overridable for tests; the real home also consults the XDG
    // user-dirs, so renamed or translated folders are found.
    explicit Places(const QString &home = {}, QObject *parent = nullptr);

    QString home() const { return m_home; }
    QVariantList places() const { return m_places; }
    QVariantList pinned() const { return m_pinned; }
    QVariantList views() const { return m_views; }
    QVariantList devices() const { return m_devices; }
    QVariantList peers() const { return m_peers; }
    QString dropboxStatus() const { return m_dropboxStatus; }
    bool trashFull() const { return m_trashFull; }

    Q_INVOKABLE void refresh();

    // Pins are GTK's bookmarks (~/.config/gtk-3.0/bookmarks), so a folder
    // pinned here is also in every GTK file dialog, and the other way round.
    // Returns an empty string, or why it couldn't be pinned.
    Q_INVOKABLE QString pin(const QString &path);
    Q_INVOKABLE void unpin(const QString &path);
    Q_INVOKABLE bool isPinned(const QString &path) const;

    // Whether any of these trashes (as FileOps::trashDirs() lists them)
    // holds anything. Stops at the first item, so a trash of thousands
    // costs no more than one of one.
    static bool anyTrashed(const QStringList &trashDirs);

    static QVariantList parseTailscale(const QByteArray &json);
    static QString parseDropboxStatus(const QByteArray &output);

    struct Mount {
        QString root;
        QString device;
        QString type;
        QString label;
    };
    // The physical drives among the mounts, one entry per drive: removable
    // and extra internal disks, never the system's own partitions.
    static QVariantList physicalDrives(const QList<Mount> &mounts, const QString &systemDevice);

signals:
    void changed();
    void trashChanged();

private:
    void refreshPlaces();
    void refreshDevices();
    void refreshPinned();
    QString bookmarksPath() const;
    QStringList readBookmarkLines() const;
    bool writeBookmarkLines(const QStringList &lines);
    void pollTailscale();
    void pollDropbox();
    QString userDirectory(int location, const QString &fallback) const;
    // Looks again at whether the trash is full, and watches the folders
    // that say so.
    void refreshTrash();

    QString m_home;
    bool m_realHome;
    QVariantList m_places, m_pinned, m_views, m_devices, m_peers;
    QString m_dropboxStatus;
    QProcess m_tailscale, m_dropbox;
    QTimer m_poll;
    // The trash changes from every window and from other apps (gio trash,
    // another file manager, rm), so it's watched rather than told. Trashing
    // a hundred files changes it a hundred times; they're looked at once.
    QFileSystemWatcher m_trashWatcher;
    QTimer m_trashSettle;
    bool m_trashFull = false;
};
