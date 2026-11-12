#include "chooser.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include "backend.h"
#include "windows.h"

using Answer = std::pair<QString, QString>;
Q_DECLARE_METATYPE(Chooser::Choice)

// A choice travels as (ssa(ss)s); Qt marshals the pairs and lists itself.
static QDBusArgument &operator<<(QDBusArgument &argument, const Chooser::Choice &choice) {
    argument.beginStructure();
    argument << choice.id << choice.label << choice.options << choice.value;
    argument.endStructure();
    return argument;
}

static const QDBusArgument &operator>>(const QDBusArgument &argument, Chooser::Choice &choice) {
    argument.beginStructure();
    argument >> choice.id >> choice.label >> choice.options >> choice.value;
    argument.endStructure();
    return argument;
}

Chooser::Chooser(const Request &request, QObject *parent) : QObject(parent), m_request(request) {}

QString Chooser::title() const {
    if (!m_request.title.isEmpty())
        return m_request.title;
    return mode() == "save" ? tr("Save As") : mode() == "saveInto" ? tr("Save Files")
         : mode() == "folder" ? tr("Open Folder") : multiple() ? tr("Open Files") : tr("Open File");
}

QString Chooser::acceptLabel() const {
    // Apps mark the letter that Alt presses with an underscore ("_Open"),
    // and write a real one twice.
    QString label = m_request.acceptLabel;
    label.replace("__", QString(QChar(0))).remove('_').replace(QChar(0), '_');
    return label;
}

QStringList Chooser::filterNames() const {
    QStringList names;
    for (const Filter &filter : m_request.filters)
        names << filter.first;
    return names;
}

void Chooser::setFilterIndex(int index) {
    if (index == m_request.filter || index < 0 || index >= m_request.filters.size())
        return;
    m_request.filter = index;
    emit filterChanged();
}

QStringList Chooser::filter() const {
    // No file is a folder, so this lists the folders alone.
    if (mode() == "folder")
        return {"inode/directory"};
    QStringList rules;
    if (m_request.filter >= 0 && m_request.filter < m_request.filters.size())
        for (const Rule &rule : m_request.filters.at(m_request.filter).second)
            // FolderModel tells a type from a pattern by its slash.
            if (rule.second.contains('/') == (rule.first == 1))
                rules << rule.second;
    return rules;
}

QVariantList Chooser::choices() const {
    QVariantList list;
    for (const Choice &choice : m_request.choices) {
        QVariantList options;
        for (const auto &[id, label] : choice.options)
            options << QVariantMap{{"id", id}, {"label", label}};
        list << QVariantMap{{"id", choice.id}, {"label", choice.label}, {"options", options}, {"value", choice.value}};
    }
    return list;
}

void Chooser::setChoice(const QString &id, const QString &value) {
    for (Choice &choice : m_request.choices) {
        if (choice.id != id || choice.value == value)
            continue;
        choice.value = value;
        emit choicesChanged();
    }
}

QString Chooser::shown(const QString &path) {
    const QString home = QDir::homePath();
    return path == home ? "~" : path.startsWith(home + '/') ? '~' + path.mid(home.size()) : path;
}

QVariantMap Chooser::step(const QString &folder, const QStringList &picked, const QString &typed) const {
    const auto answer = [this](const QStringList &paths) {
        QStringList replaces;
        if (mode() == "save" || mode() == "saveInto")
            for (const QString &path : paths)
                // A link to nothing is written through, so it counts.
                if (const QFileInfo info(path); info.exists() || info.isSymLink())
                    replaces << info.fileName();
        return QVariantMap{{"paths", paths}, {"replaces", replaces}};
    };
    // Recents and plugins' views list things, but nothing is saved in them.
    const bool real = QFileInfo(folder).isDir();

    if (mode() == "save") {
        // The name as it's typed, spaces at its ends and all: they're part
        // of a file's name, and a file opened in the window puts its own
        // name here.
        const QString &name = typed;
        if (name.trimmed().isEmpty() || (!real && QDir::isRelativePath(name) && name != "~" && !name.startsWith("~/")))
            return {};
        // A name, or a path as the path bar takes one: a folder typed is
        // somewhere to go, and anything else is the file to save.
        const QFileInfo target(QDir::cleanPath(Backend::expandTyped(name, QDir::homePath(), folder, true)));
        if (target.isDir())
            return {{"enter", target.absoluteFilePath()}};
        // A name ending in a slash says folder, and there's none.
        if (name.endsWith('/') || !QFileInfo(target.absolutePath()).isDir())
            return {};
        return answer({target.absoluteFilePath()});
    }

    // Only what's really there: a plugin's item is picked like a file, but
    // isn't one.
    QStringList folders, files;
    for (const QString &path : picked)
        if (const QFileInfo info(path); info.isAbsolute() && (info.exists() || info.isSymLink()))
            (info.isDir() ? folders : files) << path;

    if (mode() == "open") {
        // A folder alone is somewhere to look, not an answer; picked along
        // with files (everything selected), it's left out of it.
        if (files.isEmpty())
            return folders.size() == 1 ? QVariantMap{{"enter", folders.first()}} : QVariantMap{};
        if (files.size() > 1 && !multiple())
            return {};
        return answer(files);
    }

    // A folder: the one picked, or else the one being shown.
    if (folders.isEmpty() && real)
        folders << folder;
    if (folders.isEmpty() || (folders.size() > 1 && (!multiple() || mode() == "saveInto")))
        return {};
    if (mode() == "folder")
        return answer(folders);
    // Each file under its own name. A folder of that name can't be written
    // over, so the file goes beside it under the next free name ("notes
    // (2).txt"): one no folder has, and no other file here is being saved as.
    const QDir into(folders.first());
    QStringList paths;
    for (const QString &file : m_request.files)
        paths << into.filePath(file);
    for (QString &path : paths) {
        const QFileInfo named(path);
        for (int n = 2; QFileInfo(path).isDir() || paths.count(path) > 1; ++n)
            path = into.filePath(named.suffix().isEmpty() ? QString("%1 (%2)").arg(named.fileName()).arg(n)
                : QString("%1 (%2).%3").arg(named.completeBaseName()).arg(n).arg(named.suffix()));
    }
    return answer(paths);
}

void Chooser::accept(const QStringList &paths) {
    if (m_answered || paths.isEmpty())
        return;
    m_answered = true;
    emit finished(true, paths);
}

void Chooser::cancel() {
    if (m_answered)
        return;
    m_answered = true;
    emit finished(false, {});
}

static const char *const serviceName = "org.freedesktop.impl.portal.desktop.folio";
static const char *const objectPath = "/org/freedesktop/portal/desktop";

ChooserPortal::ChooserPortal(Windows *windows, QObject *parent) : QDBusVirtualObject(parent), m_windows(windows) {
    qDBusRegisterMetaType<Chooser::Rule>();
    qDBusRegisterMetaType<QList<Chooser::Rule>>();
    qDBusRegisterMetaType<Chooser::Filter>();
    qDBusRegisterMetaType<QList<Chooser::Filter>>();
    qDBusRegisterMetaType<Answer>();
    qDBusRegisterMetaType<QList<Answer>>();
    qDBusRegisterMetaType<Chooser::Choice>();
    qDBusRegisterMetaType<QList<Chooser::Choice>>();
}

bool ChooserPortal::registerOn(QDBusConnection bus) {
    // The object before the name, so the call that started this process
    // finds it the moment the name is taken.
    return bus.isConnected() && bus.registerVirtualObject(objectPath, this, QDBusConnection::SubPath)
        && bus.registerService(serviceName);
}

QString ChooserPortal::introspect(const QString &path) const {
    if (path != objectPath)
        return "<interface name=\"org.freedesktop.impl.portal.Request\"><method name=\"Close\"/></interface>";
    const QString method = "<method name=\"%1\">"
                           "<arg type=\"o\" name=\"handle\" direction=\"in\"/>"
                           "<arg type=\"s\" name=\"app_id\" direction=\"in\"/>"
                           "<arg type=\"s\" name=\"parent_window\" direction=\"in\"/>"
                           "<arg type=\"s\" name=\"title\" direction=\"in\"/>"
                           "<arg type=\"a{sv}\" name=\"options\" direction=\"in\"/>"
                           "<arg type=\"u\" name=\"response\" direction=\"out\"/>"
                           "<arg type=\"a{sv}\" name=\"results\" direction=\"out\"/>"
                           "</method>";
    return "<interface name=\"org.freedesktop.impl.portal.FileChooser\">" + method.arg("OpenFile")
         + method.arg("SaveFile") + method.arg("SaveFiles") + "</interface>";
}

bool ChooserPortal::handleMessage(const QDBusMessage &message, const QDBusConnection &connection) {
    if (message.type() != QDBusMessage::MethodCallMessage)
        return false;
    // The portal gave up on a request (its app went away): the dialog goes,
    // which answers the call it was opened for.
    if (message.interface() == "org.freedesktop.impl.portal.Request" && message.member() == "Close") {
        // Only for whoever asked: another program on the bus that knows the
        // request's path doesn't get to close someone else's dialog.
        if (const Asked asked = m_asked.value(message.path()); asked.chooser && asked.call.service() == message.service())
            asked.chooser->cancel();
        connection.send(message.createReply());
        return true;
    }
    if (message.interface() != "org.freedesktop.impl.portal.FileChooser"
        || !QStringList{"OpenFile", "SaveFile", "SaveFiles"}.contains(message.member()))
        return false;
    if (message.signature() != "osssa{sv}")
        connection.send(message.createErrorReply(QDBusError::InvalidArgs, "expected (osssa{sv})"));
    else
        ask(message, connection);
    return true;
}

// Inside a{sv}, what Qt can't unpack on its own arrives still packed. It's
// unpacked only when it's the shape expected: any caller on the bus can send
// anything, and unpacking the wrong shape reads past it.
template <typename T> static T unpacked(const QVariant &value, const char *signature) {
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return value.value<T>();
    const QDBusArgument packed = value.value<QDBusArgument>();
    return packed.currentSignature() == QLatin1String(signature) ? qdbus_cast<T>(packed) : T();
}

// A name an app gives is a name: never a path out of the folder shown.
static QString bareName(const QString &name) {
    const QString bare = QFileInfo(name).fileName();
    return bare == "." || bare == ".." ? QString() : bare;
}

// Paths travel as bytes, with a zero at the end.
static QString bytePath(QByteArray bytes) {
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QFile::decodeName(bytes);
}

Chooser::Request ChooserPortal::requestFor(const QString &method, const QString &title, const QVariantMap &options,
                                           QString *folder, QString *select) {
    Chooser::Request request;
    request.mode = method == "SaveFile" ? "save" : method == "SaveFiles" ? "saveInto"
                 : options.value("directory").toBool() ? "folder" : "open";
    request.title = title;
    request.acceptLabel = options.value("accept_label").toString();
    request.multiple = options.value("multiple").toBool();
    request.choices = unpacked<QList<Chooser::Choice>>(options.value("choices"), "a(ssa(ss)s)");
    // An app may leave what a choice starts on to the dialog: the first of
    // its options, or a checkbox unticked.
    for (Chooser::Choice &choice : request.choices)
        if (choice.value.isEmpty())
            choice.value = choice.options.isEmpty() ? "false" : choice.options.first().first;
    request.name = bareName(options.value("current_name").toString());
    for (const QByteArray &file : unpacked<QList<QByteArray>>(options.value("files"), "aay"))
        if (const QString name = bareName(bytePath(file)); !name.isEmpty())
            request.files << name;

    request.filters = unpacked<QList<Chooser::Filter>>(options.value("filters"), "a(sa(us))");
    request.filter = request.filters.isEmpty() ? -1 : 0;
    // The filter to start on. One that isn't in the list joins it.
    if (options.contains("current_filter")) {
        const auto current = unpacked<Chooser::Filter>(options.value("current_filter"), "(sa(us))");
        if (!request.filters.contains(current) && !current.second.isEmpty())
            request.filters << current;
        request.filter = qMax(request.filters.isEmpty() ? -1 : 0, int(request.filters.indexOf(current)));
    }

    *folder = bytePath(options.value("current_folder").toByteArray());
    select->clear();
    // A save over a file that's there starts beside it, under its name.
    if (const QFileInfo file(bytePath(options.value("current_file").toByteArray())); file.isAbsolute()) {
        *folder = file.absolutePath();
        request.name = file.fileName();
        if (file.exists())
            *select = file.fileName();
    }
    return request;
}

QVariantMap ChooserPortal::resultsFor(const Chooser::Request &request, const QStringList &paths) {
    QStringList uris;
    for (const QString &path : paths)
        uris << QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    // Picked by the user here, which is the leave a sandboxed app needs to
    // write to them.
    QVariantMap results{{"uris", uris}, {"writable", true}};
    if (request.filter >= 0 && request.filter < request.filters.size())
        results.insert("current_filter", QVariant::fromValue(request.filters.at(request.filter)));
    QList<Answer> answers;
    for (const Chooser::Choice &choice : request.choices)
        answers << Answer{choice.id, choice.value};
    if (!answers.isEmpty())
        results.insert("choices", QVariant::fromValue(answers));
    return results;
}

void ChooserPortal::ask(const QDBusMessage &message, const QDBusConnection &connection) {
    const QVariantList arguments = message.arguments();
    const QString handle = arguments.at(0).value<QDBusObjectPath>().path();
    // A second dialog on a request still open would leave the first call
    // without its answer.
    if (handle.isEmpty() || m_asked.contains(handle)) {
        connection.send(message.createErrorReply(QDBusError::InvalidArgs, "bad request handle"));
        return;
    }

    // Held while the request is read: what's still packed inside the options
    // is read out of it.
    const QDBusArgument packed = arguments.at(4).value<QDBusArgument>();
    QVariantMap options;
    packed >> options;
    QString folder, select;
    const Chooser::Request request = requestFor(message.member(), arguments.at(3).toString(), options, &folder, &select);
    // Files to save, and not one of them with a name: nothing to ask.
    if (request.mode == "saveInto" && request.files.isEmpty()) {
        connection.send(message.createErrorReply(QDBusError::InvalidArgs, "no files to save"));
        return;
    }
    auto *chooser = new Chooser(request, this);
    if (!QFileInfo(folder).isDir())
        folder = QFileInfo(m_lastFolder).isDir() ? m_lastFolder : QDir::homePath();
    // The window takes the chooser with it when it goes.
    if (!m_windows->open(folder, select, chooser)) {
        delete chooser;
        connection.send(message.createErrorReply(QDBusError::Failed, "the file chooser couldn't be shown"));
        return;
    }
    message.setDelayedReply(true);
    m_asked.insert(handle, {message, connection.name(), chooser, chooser});
    connect(chooser, &Chooser::finished, this, [this, handle](bool accepted, const QStringList &paths) {
        answer(handle, accepted, paths);
    });
    // However the window goes, the app hears back: for this dialog, not a
    // later one under the same handle.
    connect(chooser, &QObject::destroyed, this, [this, handle, chooser] {
        if (m_asked.value(handle).asked == chooser)
            answer(handle, false, {});
    });
    // The portal itself gone, nobody is waiting for the answer.
    auto *watcher = new QDBusServiceWatcher(message.service(), connection, QDBusServiceWatcher::WatchForUnregistration, chooser);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, chooser, &Chooser::cancel);
}

void ChooserPortal::answer(const QString &handle, bool accepted, const QStringList &paths) {
    const Asked asked = m_asked.take(handle);
    // 0 is an answer, 1 that the user called it off.
    QVariantMap results;
    if (accepted && asked.chooser) {
        results = resultsFor(asked.chooser->request(), paths);
        m_lastFolder = QFileInfo(paths.first()).absolutePath();
    }
    QDBusConnection(asked.connection).send(asked.call.createReply({QVariant::fromValue(uint(accepted ? 0 : 1)), results}));
    if (m_asked.isEmpty())
        emit idle();
}
