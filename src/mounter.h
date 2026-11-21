#pragma once

#include <QHash>
#include <QObject>
#include <QString>

typedef struct _GFile GFile;
typedef struct _GMountOperation GMountOperation;

// Mounts other machines (sftp://, smb://, anything GVfs mounts) through GIO,
// the way Nautilus does, so they land under GVfs where every app can see
// them. GVfs asks questions while mounting. Host-key questions are answered
// here with yes when the mount is verified (a plugin such as Tailscale has
// already made sure who's on the other end). Everything else (a password,
// when keys aren't set up; a server's unknown key or certificate) goes to
// the window to put to the user, instead of dying silently as it does under
// `gio mount` with no terminal to ask in.
//
// A Windows share's host on its own (smb://nas) mounts GVfs's browser for
// it, which has no local folder: its shares are listed instead, and the
// one picked is mounted, the way other file managers open a host.
class Mounter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY askingChanged)
    Q_PROPERTY(QString user READ user NOTIFY askingChanged)
    // A question's answers, in GVfs's words; empty for a password request.
    Q_PROPERTY(QStringList choices READ choices NOTIFY askingChanged)
    // The choices are a host's shared folders, to pick one to open.
    Q_PROPERTY(bool picking READ picking NOTIFY askingChanged)

public:
    explicit Mounter(QObject *parent = nullptr);
    ~Mounter() override;

    bool busy() const { return m_operation != nullptr || m_listing || m_picking; }
    bool asking() const { return m_asking; }
    QString prompt() const { return m_prompt; }
    QString user() const { return m_user; }
    QStringList choices() const { return m_choices; }
    bool picking() const { return m_picking; }

    // Mounts the location; `mounted` carries its local GVfs path. A
    // verified location's questions are answered here, with their first
    // choice.
    void mount(const QString &uri, bool verified);

    // Where GVfs keeps its mounts, and the mount `url` is in (its root,
    // where its row is lit): worked out for SFTP, whose mount is named by
    // the URL alone, and known for the rest once mounted; empty otherwise.
    static QString root();
    static QString folderFor(const QString &url);
    // The folder `url` itself opens, inside its mount; empty when unknown.
    static QString destinationFor(const QString &url);
    // Each address mounted this session and the folder GVfs opened for it.
    static QHash<QString, QString> &known();

    // A shared folder a host lists: its name and the address it opens.
    struct Share {
        QString name, uri;
        bool hidden = false;
    };
    // The ones worth offering, by name: not hidden, nor an administrative
    // share (C$, IPC$), whose name ends in $ and which only admins open.
    static QList<Share> offered(QList<Share> shares);

    // The user's answer to a password request; remembered for this session
    // only, never written to a keyring without asking.
    Q_INVOKABLE void providePassword(const QString &user, const QString &password);
    // The user's answer to a question, by its place among the choices, or
    // the shared folder picked, which is mounted next.
    Q_INVOKABLE void choose(int choice);
    // Closes the password request (or the list of shared folders) and
    // abandons the mount.
    Q_INVOKABLE void cancel();

signals:
    void askingChanged();
    void mounted(const QString &path);
    // A host's share is mounted next, in place of the host asked for.
    void sharePicked(const QString &uri);
    void failed(const QString &error);

private:
    // GIO's C callbacks, in mounter.cpp.
    friend struct MounterCallbacks;
    void setAsking(bool asking);
    void finish();
    // Lists what the mount at `location` holds, to offer its shares.
    void browse(GFile *location);
    void offer(const QList<Share> &shares);
    void mountShare(const QString &uri);

    GMountOperation *m_operation = nullptr;
    bool m_verified = false;
    QString m_url;
    bool m_asking = false;
    QString m_prompt;
    QString m_user;
    QStringList m_choices;
    bool m_listing = false;
    bool m_picking = false;
    QStringList m_shareUris;
};
