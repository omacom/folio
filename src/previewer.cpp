#include "previewer.h"
#include "fontsample.h"

#include "fileops.h"
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMimeDatabase>
#include <QUrl>
#include <QtConcurrent>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdio>
#include <sys/prctl.h>
#include <unistd.h>

Previewer::Previewer(QObject *parent) : QObject(parent) {}

QString Previewer::viewerFor(const QString &mime, bool isDir) {
    if (isDir)
        return "folder";
    if (mime == "application/pdf")
        return "pdf";
    if (FontSample::isFont(mime))
        return "font";
    if (mime.startsWith("video/"))
        return "video";
    if (mime.startsWith("audio/"))
        return "audio";
    if (mime.startsWith("image/")) {
        // Formats that can move are drawn by AnimatedImage, which shows the
        // still ones just as well.
        if (mime == "image/gif" || mime == "image/webp" || mime == "image/apng")
            return "animated";
        // Qt decodes the common formats at full size; the rest (HEIC, JPEG
        // XL, raw camera files) go through the installed thumbnailers at the
        // largest size the cache spec has.
        if (QImageReader::supportedMimeTypes().contains(mime.toLatin1()))
            return "image";
        return "thumbnail";
    }
    if (listsArchive(mime))
        return "archive";
    if (mime == "text/markdown")
        return "markdown";
    const QMimeType type = QMimeDatabase().mimeTypeForName(mime);
    if (mime.startsWith("text/") || type.inherits("text/plain"))
        return "text";
    return "other";
}

bool Previewer::listsArchive(const QString &mime) {
    static const QStringList types{"application/zip", "application/x-tar", "application/x-compressed-tar",
                                   "application/x-xz-compressed-tar", "application/x-bzip2-compressed-tar",
                                   "application/x-zstd-compressed-tar", "application/x-lz4-compressed-tar",
                                   "application/x-7z-compressed", "application/vnd.rar", "application/x-iso9660-image",
                                   "application/x-cpio"};
    // By the names the system's database gives, older or newer (an ISO is application/vnd.efi.iso in newer ones).
    const QMimeDatabase mimes;
    const QString name = mimes.mimeTypeForName(mime).name();
    return !name.isEmpty() && std::any_of(types.begin(), types.end(), [&](const QString &type) {
        return mimes.mimeTypeForName(type).name() == name;
    });
}

QString Previewer::readText(const QString &path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray data = file.read(limit + 1);
    if (data.left(8192).contains('\0'))
        return {};
    // Cut where a line ends, not inside one; with no line end to cut at,
    // at least not inside a character.
    if (data.size() > limit) {
        data.truncate(limit);
        if (const qsizetype end = data.lastIndexOf('\n'); end >= 0) {
            data.truncate(end + 1);
        } else {
            // Back to the start of the last character, then keep it only
            // if all of its bytes made it in.
            qsizetype start = data.size() - 1;
            while (start > 0 && start > data.size() - 4 && (uchar(data.at(start)) & 0xC0) == 0x80)
                --start;
            const uchar lead = uchar(data.at(start));
            const int length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (data.size() - start < length)
                data.truncate(start);
        }
    }
    return QString::fromUtf8(data);
}

QString Previewer::markdownOpening(const QString &text, int cells) {
    // A table cell is counted by its | (a row has one more than its cells,
    // which errs on the safe side). Code in fences is shown as it is, never
    // laid out as a table, so its | don't count. As Markdown reads them, a
    // fence is three or more ` or ~, indented less than four spaces (more is
    // indented code), and only a run of the same mark at least as long, with
    // nothing after it, closes it.
    qsizetype at = 0;
    QChar fence;
    qsizetype fenceLength = 0;
    while (at < text.size()) {
        qsizetype end = text.indexOf('\n', at);
        end = end < 0 ? text.size() : end + 1;
        const QStringView line = QStringView(text).mid(at, end - at);
        qsizetype indent = 0;
        while (indent < 4 && indent < line.size() && line.at(indent) == ' ')
            ++indent;
        const QChar mark = indent < line.size() ? line.at(indent) : QChar();
        qsizetype run = 0;
        while (indent < 4 && indent + run < line.size() && line.at(indent + run) == mark && (mark == '`' || mark == '~'))
            ++run;
        const QStringView rest = line.mid(indent + run).trimmed();
        if (fence.isNull() && run >= 3 && !(mark == '`' && rest.contains('`'))) {
            fence = mark;
            fenceLength = run;
        } else if (!fence.isNull() && mark == fence && run >= fenceLength && rest.isEmpty()) {
            fence = QChar();
        } else if (fence.isNull()) {
            cells -= line.count('|');
        }
        // One line is a paragraph, however many | it has.
        if (cells < 0)
            return text.left(at > 0 ? at : end);
        at = end;
    }
    return text;
}

void Previewer::start(const QString &path, bool showHidden, const QString &sort) {
    m_showHidden = showHidden;
    m_sort = sort;
    show(path);
}

void Previewer::show(const QString &path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path == absolute) {
            if (i != m_index)
                setIndex(i, false);
            return;
        }
    }

    // A file from another folder: show it alone, then fill in its siblings.
    m_entries = {FolderModel::entryFor(QFileInfo(absolute), QMimeDatabase())};
    setIndex(0, false);
    relist();
}

void Previewer::relist() {
    if (m_index < 0 || m_index >= m_entries.size())
        return;
    const quint64 generation = ++m_generation;
    const QString folder = QFileInfo(m_entries.at(m_index).path).absolutePath();
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QLocale locale;
    auto *watcher = new QFutureWatcher<QVector<FolderModel::Entry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const auto entries = watcher->result();
        const QString current = m_entries.at(m_index).path;
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].path == current) {
                // Keep the entry already shown: it has the folder summary.
                const FolderModel::Entry shown = m_entries.at(m_index);
                m_entries = entries;
                m_entries[i] = shown;
                m_index = i;
                emit listed();
                return;
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([folder, showHidden, sort, locale] {
        return FolderModel::list(folder, showHidden, sort, nullptr, {}, locale);
    }));
}

QVariantMap Previewer::item() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    // Only the preview fills in a folder's first pictures (in setIndex), so
    // only its items carry them; the folder model's would always be empty.
    const FolderModel::Entry &e = m_entries.at(m_index);
    QVariantMap map = FolderModel::toMap(e);
    map.insert("peek", e.peek);
    return map;
}

QString Previewer::url() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    return QUrl::fromLocalFile(m_entries.at(m_index).path).toString();
}

QString Previewer::type() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    const FolderModel::Entry &e = m_entries.at(m_index);
    return QMimeDatabase().mimeTypeForName(FontSample::typeOf(e.mime, e.path)).comment();
}

void Previewer::next() {
    if (m_index + 1 < m_entries.size())
        setIndex(m_index + 1, true);
}

void Previewer::previous() {
    if (m_index > 0)
        setIndex(m_index - 1, true);
}

void Previewer::open() {
    if (m_index >= 0)
        // The same lookup as opening from the folder: the type's default
        // app, or its parent type's.
        if (const QString problem = FileOps::openInDefaultApp(m_entries.at(m_index).path); !problem.isEmpty())
            qWarning("%s", qPrintable(problem));
}

void Previewer::setIndex(int index, bool report) {
    m_index = index;
    const FolderModel::Entry e = m_entries.at(index);
    m_viewer = viewerFor(e.mime, e.isDir);
    // By its extension alone a ".otf" font is taken for an OpenDocument template.
    if (m_viewer == "other" && FontSample::isFontFile(e.path))
        m_viewer = "font";
    m_text.clear();
    m_sentence.clear();
    m_helper.stop();
    if (m_viewer == "archive") {
        listArchive(m_helper, e.path, [this](const QString &text, const QString &sentence) {
            m_text = text;
            m_sentence = sentence;
            emit changed();
        });
    }

    // A folder's count and first pictures, and a text's opening part, are
    // read off the UI thread: a big folder, or any file on a slow mount,
    // would hold up the window until they were in.
    const quint64 generation = ++*m_reading;
    const bool count = e.isDir && e.itemCount < 0;
    const bool text = m_viewer == "text" || m_viewer == "markdown";
    if (count || text) {
        auto *watcher = new QFutureWatcher<QPair<FolderModel::Entry, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, text] {
            watcher->deleteLater();
            if (generation != *m_reading)
                return; // Stepped on to another item while this was read.
            const auto [entry, read] = watcher->result();
            m_entries[m_index] = entry;
            m_text = read;
            // A ".txt" that turns out to be binary is shown as a plain file.
            if (text && m_text.isEmpty() && entry.size > 0)
                m_viewer = "other";
            emit changed();
        });
        const bool showHidden = m_showHidden;
        const bool markdown = m_viewer == "markdown";
        watcher->setFuture(QtConcurrent::run([entry = e, showHidden, count, text, markdown,
                                              latest = m_reading, generation]() mutable {
            if (*latest != generation)
                return qMakePair(entry, QString());
            if (count)
                FolderModel::summarize(entry, showHidden);
            const QString read = text ? readText(entry.path) : QString();
            return qMakePair(entry, markdown ? markdownOpening(read) : read);
        }));
    }
    emit changed();
    emit listed();

    if (report && m_reporting) {
        std::fprintf(stdout, "%s\n", e.path.toUtf8().constData());
        std::fflush(stdout);
    }
}

void Previewer::connectToFolio() {
    m_reporting = true;
    m_stdin = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_stdin, &QSocketNotifier::activated, this, [this] {
        char buffer[4096];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0) {
            // Folio went away; there's nothing left to follow.
            m_stdin->setEnabled(false);
            return;
        }
        m_pending.append(buffer, n);
        int newline;
        while ((newline = m_pending.indexOf('\n')) >= 0) {
            const QString path = QString::fromUtf8(m_pending.left(newline));
            m_pending.remove(0, newline + 1);
            if (!path.isEmpty())
                show(path);
        }
    });
}

// An archive's listing is cut there; bsdtar prints each name on a line.
static constexpr qint64 archiveOutputLimit = 32 * 1024 * 1024;

Helper::Helper(QObject *parent, int milliseconds) : QObject(parent) {
    m_limit.setSingleShot(true);
    m_limit.setInterval(milliseconds);
    connect(&m_limit, &QTimer::timeout, this, [this] { end(stopped, {}); });
}

Helper::~Helper() {
    stop();
}

void Helper::run(const QString &program, const QStringList &args, qint64 outputLimit,
                 const std::function<void(const QByteArray &)> &output,
                 const std::function<void(int, const QByteArray &)> &finished) {
    stop();
    m_read = 0;
    m_outputLimit = outputLimit;
    m_errors.clear();
    m_finished = finished;
    auto *process = new QProcess(this);
    m_process = process;
    // UTF-8 names print as they are, whatever Folio's language.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C.UTF-8");
    process->setProcessEnvironment(environment);
    // No terminal or input to ask for a passphrase on.
    process->setStandardInputFile(QProcess::nullDevice());
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    // It goes with Folio, even when Folio crashes.
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    const auto take = [this, process, output] {
        const QByteArray part = process->readAllStandardOutput();
        m_read += part.size();
        if (m_read > m_outputLimit)
            return false;
        if (!part.isEmpty())
            output(part);
        return true;
    };
    connect(process, &QProcess::readyReadStandardOutput, this, [this, take] {
        if (!take())
            end(stopped, {});
    });
    // Read as it arrives, so a program that prints errors without end fills no memory.
    const auto keep = [this, process] {
        const QByteArray part = process->readAllStandardError();
        m_errors += part.left(errorsKept - m_errors.size());
    };
    connect(process, &QProcess::readyReadStandardError, this, keep);
    connect(process, &QProcess::finished, this, [this, take, keep](int code, QProcess::ExitStatus status) {
        if (!take()) {
            end(stopped, {});
            return;
        }
        keep();
        end(status == QProcess::NormalExit ? code : crashed, std::exchange(m_errors, {}));
    });
    // A program that isn't installed never finishes, so this is the only word from it.
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            end(missing, {});
    });
    m_limit.start();
    process->start(program, args);
}

void Helper::stop() {
    m_limit.stop();
    m_finished = nullptr;
    QProcess *process = std::exchange(m_process, nullptr);
    if (!process)
        return;
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    // Killed without waiting on a hung disk; the object goes once the process has exited, or has failed to start.
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    // It leads a session of its own, so what it started (a program bsdtar unpacks with) goes with it.
    if (const qint64 pid = process->processId(); pid > 0)
        ::killpg(pid_t(pid), SIGKILL);
    process->kill();
}

void Helper::end(int code, const QByteArray &errors) {
    const auto finished = m_finished;
    stop();
    if (finished)
        finished(code, errors);
}

bool ArchiveNames::add(const QByteArray &output) {
    // What was left had no line end in it: the look for one starts at the new part.
    qsizetype from = m_pending.size();
    m_pending += output;
    const qsizetype before = m_names.size();
    qsizetype start = 0;
    qsizetype newline;
    while ((newline = m_pending.indexOf('\n', from)) >= 0) {
        const QByteArrayView line(m_pending.constData() + start, newline - start);
        start = from = newline + 1;
        // The "./" a tar made with `-C folder .` starts with, or an ISO's ".", is the archive itself.
        if (line.isEmpty() || line == "./" || line == ".")
            continue;
        ++m_count;
        // Names are shown up to a count and up to a size together: a few very long ones would hold the window as a huge text does.
        m_full = m_full || m_names.size() == shown || m_bytes + line.size() > shownBytes;
        if (m_full)
            continue;
        m_bytes += line.size();
        m_names << nameFrom(line);
    }
    m_pending.remove(0, start);
    return m_names.size() > before;
}

QString ArchiveNames::text(bool whole) const {
    const int count = int(qMin<qint64>(m_count, INT_MAX));
    const int more = count - int(m_names.size());
    QStringList lines{whole ? Previewer::tr("%Ln items", nullptr, count) : Previewer::tr("At least %Ln items", nullptr, count),
                      QString()};
    lines += m_names;
    if (more > 0)
        lines << (whole ? Previewer::tr("%Ln more", nullptr, more) : Previewer::tr("At least %Ln more", nullptr, more));
    return lines.join('\n');
}

// Sample: `./sub dir/caf\303\251 \\ two\nlines.txt` reads `sub dir/café \ two\nlines.txt` ("\n" stays).
QString ArchiveNames::nameFrom(QByteArrayView line) {
    if (line.startsWith("./"))
        line = line.sliced(2);
    const auto octal = [&](qsizetype at) { return at < line.size() && line[at] >= '0' && line[at] <= '7'; };
    QByteArray name;
    name.reserve(line.size());
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '\\') {
            name += '\\';
            ++i;
        } else if (line[i] == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            name += char((line[i + 1] - '0') * 64 + (line[i + 2] - '0') * 8 + (line[i + 3] - '0'));
            i += 3;
        } else {
            name += line[i];
        }
    }
    return QString::fromUtf8(name);
}

void Previewer::listArchive(Helper &helper, const QString &path,
                            const std::function<void(const QString &, const QString &)> &show) {
    auto names = std::make_shared<ArchiveNames>();
    // An absolute path is never read as an option, or as "-" for stdin.
    helper.run("bsdtar", {"-tf", QFileInfo(path).absoluteFilePath()}, archiveOutputLimit,
               [names, show](const QByteArray &output) {
                   if (names->add(output))
                       show(names->text(false), {});
               },
               [names, show](int code, const QByteArray &errors) {
                   if (code == 0 && names->count() > 0)
                       show(names->text(true), {});
                   else if (code == 0)
                       show({}, tr("Empty"));
                   else if (code == Helper::stopped && names->count() > 0)
                       show(names->text(false), {});
                   // Stopped before its first name: a large archive on a slow drive, not a damaged one.
                   else if (code == Helper::stopped)
                       show({}, tr("Listing this archive took too long."));
                   else if (code == Helper::missing)
                       show({}, tr("Listing archives needs bsdtar, from the libarchive package."));
                   // A failed run shows no names: bsdtar reads a renamed text file as a list of names.
                   else if (errors.contains("ncrypt"))
                       show({}, tr("The names in this archive are encrypted."));
                   else
                       show({}, tr("Couldn't read this archive."));
               });
}
