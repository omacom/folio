#pragma once

#include <QHash>
#include <QObject>
#include <QString>

typedef struct _GMountOperation GMountOperation;

// Mounts other machines (sftp://, smb://, anything GVfs mounts) through GIO,
// the way Nautilus does, so they land under GVfs where every app can see
// them. GVfs asks questions while mounting. Host-key questions are answered
// here with yes when the mount is verified (a plugin such as Tailscale has
// already made sure who's on the other end). Everything else (a password,
// when keys aren't set up; a server's unknown key or certificate) goes to
// the window to put to the user, instead of dying silently as it does under
// `gio mount` with no terminal to ask in.
class Mounter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY askingChanged)
    Q_PROPERTY(QString user READ user NOTIFY askingChanged)
    // A question's answers, in GVfs's words; empty for a password request.
    Q_PROPERTY(QStringList choices READ choices NOTIFY askingChanged)

public:
    explicit Mounter(QObject *parent = nullptr);
    ~Mounter() override;

    bool busy() const { return m_operation != nullptr; }
    bool asking() const { return m_asking; }
    QString prompt() const { return m_prompt; }
    QString user() const { return m_user; }
    QStringList choices() const { return m_choices; }

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

    // The user's answer to a password request; remembered for this session
    // only, never written to a keyring without asking.
    Q_INVOKABLE void providePassword(const QString &user, const QString &password);
    // The user's answer to a question, by its place among the choices.
    Q_INVOKABLE void choose(int choice);
    // Closes the password request and abandons the mount.
    Q_INVOKABLE void cancel();

signals:
    void askingChanged();
    void mounted(const QString &path);
    void failed(const QString &error);

private:
    // GIO's C callbacks, in mounter.cpp.
    friend struct MounterCallbacks;
    void setAsking(bool asking);
    void finish();

    GMountOperation *m_operation = nullptr;
    bool m_verified = false;
    QString m_url;
    bool m_asking = false;
    QString m_prompt;
    QString m_user;
    QStringList m_choices;
};
