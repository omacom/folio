#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <utility>

class Windows;

// One Open or Save dialog another app asked for. The dialog is a Folio
// window like any other, with a bar along the bottom (ChooserBar.qml) that
// holds the name being saved, the kinds of file asked for and the button
// that answers. The window reads what was asked from here, and answers here.
class Chooser : public QObject {
    Q_OBJECT
    // "open" (files), "folder" (folders), "save" (one file, under a name
    // typed here) or "saveInto" (files the app has named, into one folder).
    Q_PROPERTY(QString mode READ mode CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    // The app's own word for the button, or empty for Folio's.
    Q_PROPERTY(QString acceptLabel READ acceptLabel CONSTANT)
    Q_PROPERTY(bool multiple READ multiple CONSTANT)
    // The name a save starts with.
    Q_PROPERTY(QString name READ name CONSTANT)
    // The kinds of file to pick between ("Images", "All Files") and the one
    // picked, which is all the folders list.
    Q_PROPERTY(QStringList filterNames READ filterNames CONSTANT)
    Q_PROPERTY(int filterIndex READ filterIndex WRITE setFilterIndex NOTIFY filterChanged)
    // The app's own questions (an encoding, "Open read-only"), each {id,
    // label, options: [{id, label}...], value}. One without options is a
    // checkbox, its value "true" or "false".
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)

public:
    // A filter as the portal sends it: its name and its rules, each a name
    // pattern (0, "*.png") or a MIME type (1, "image/png").
    using Rule = std::pair<uint, QString>;
    using Filter = std::pair<QString, QList<Rule>>;
    struct Choice {
        QString id;
        QString label;
        QList<std::pair<QString, QString>> options;
        QString value;
    };
    struct Request {
        QString mode = "open";
        QString title;
        QString acceptLabel;
        bool multiple = false;
        QList<Filter> filters;
        int filter = -1;
        QList<Choice> choices;
        QString name;
        // The names to save, in "saveInto".
        QStringList files;
    };

    explicit Chooser(const Request &request, QObject *parent = nullptr);

    QString mode() const { return m_request.mode; }
    QString title() const;
    QString acceptLabel() const;
    bool multiple() const { return m_request.multiple; }
    QString name() const { return m_request.name; }
    QStringList filterNames() const;
    int filterIndex() const { return m_request.filter; }
    void setFilterIndex(int index);
    QVariantList choices() const;
    Q_INVOKABLE void setChoice(const QString &id, const QString &value);

    // What a FolderModel lists for the filter picked: only folders when a
    // folder is asked for.
    QStringList filter() const;
    // The filter picked and the choices as they stand, for the answer.
    const Request &request() const { return m_request; }

    // What the button does as the window stands: showing `folder`, with
    // `picked` in it (the selection, or the item under the cursor) and, for
    // a save, `typed` as the name. {enter: a folder to go into}, or {paths:
    // the answer, replaces: the names of files a save would write over}, or
    // empty when there's nothing to answer with yet.
    Q_INVOKABLE QVariantMap step(const QString &folder, const QStringList &picked, const QString &typed) const;
    // A path as it's shown, with ~ for home.
    Q_INVOKABLE static QString shown(const QString &path);

    // The answer, once: whichever comes first counts.
    Q_INVOKABLE void accept(const QStringList &paths);
    Q_INVOKABLE void cancel();
    // A file was opened in the window (Return, a double click).
    void activate(const QString &path) { emit activated(path); }

signals:
    void filterChanged();
    void choicesChanged();
    void activated(const QString &path);
    void finished(bool accepted, const QStringList &paths);

private:
    Request m_request;
    bool m_answered = false;
};

// Folio as the desktop's file chooser: org.freedesktop.impl.portal.FileChooser,
// the backend xdg-desktop-portal turns to for other apps' Open and Save
// dialogs when portals.conf names folio for it. `folio chooser` serves it,
// started by D-Bus when the first dialog is asked for.
//
// A virtual object, not an adaptor: the answer waits for the user, and the
// portal closes a request by calling a path of that request's own, below
// this object's.
class ChooserPortal : public QDBusVirtualObject {
    Q_OBJECT

public:
    explicit ChooserPortal(Windows *windows, QObject *parent = nullptr);

    // Claims the object path and the bus name; false when another `folio
    // chooser` is already answering.
    bool registerOn(QDBusConnection bus);
    bool busy() const { return !m_asked.isEmpty(); }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;
    QString introspect(const QString &path) const override;

    // A call's options as a request, and where its window starts: `folder`,
    // and `select` for a save over a file that's there already.
    static Chooser::Request requestFor(const QString &method, const QString &title, const QVariantMap &options,
                                       QString *folder, QString *select);
    // What the app is told: the files as URIs, the filter and the choices.
    static QVariantMap resultsFor(const Chooser::Request &request, const QStringList &paths);

signals:
    // The last dialog open was answered.
    void idle();

private:
    struct Asked {
        QDBusMessage call;
        QString connection;
        QPointer<Chooser> chooser;
        // Which dialog it is, once the chooser itself is gone.
        const QObject *asked = nullptr;
    };
    void ask(const QDBusMessage &message, const QDBusConnection &connection);
    void answer(const QString &handle, bool accepted, const QStringList &paths);

    Windows *m_windows;
    // The dialogs open, by their request's path.
    QHash<QString, Asked> m_asked;
    // Where the last dialog was answered, for an app that doesn't say where
    // to start.
    QString m_lastFolder;
};
