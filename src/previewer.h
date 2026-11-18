#pragma once

#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <functional>

#include "foldermodel.h"

// A helper program (bsdtar) reading the shown item, stopped unread when the view moves on or goes.
class Helper : public QObject {
public:
    // How a run ended when it didn't exit by itself.
    static constexpr int crashed = -1, stopped = -2, missing = -3;
    // As much of stderr as a run keeps for its caller; the rest is read and dropped.
    static constexpr qint64 errorsKept = 64 * 1024;

    explicit Helper(QObject *parent = nullptr, int milliseconds = 5000);
    ~Helper() override;

    // Stops the last run; `output` gets stdout as it arrives, `finished` the exit code and stderr.
    void run(const QString &program, const QStringList &args, qint64 outputLimit,
             const std::function<void(const QByteArray &)> &output,
             const std::function<void(int, const QByteArray &)> &finished);
    void stop();

private:
    void end(int code, const QByteArray &errors);

    QProcess *m_process = nullptr;
    QTimer m_limit;
    qint64 m_read = 0;
    qint64 m_outputLimit = 0;
    QByteArray m_errors;
    std::function<void(int, const QByteArray &)> m_finished;
};

// An archive's names from `bsdtar -tf`, read as they arrive: the first kept, the rest counted.
class ArchiveNames {
public:
    static constexpr int shown = 500;
    // As much as a text file shows of itself.
    static constexpr qint64 shownBytes = 128 * 1024;

    // Takes the next part of the output; true when it adds a name to those shown.
    bool add(const QByteArray &output);
    qint64 count() const { return m_count; }
    // The count, a blank line, the names and how many more; "at least" until it's whole.
    QString text(bool whole) const;
    // One line of `bsdtar -tf` back to the name it escapes.
    static QString nameFrom(QByteArrayView line);

private:
    QByteArray m_pending;
    QStringList m_names;
    qint64 m_count = 0;
    qint64 m_bytes = 0;
    bool m_full = false;
};

// The state behind `folio preview`: the file being looked at, its siblings
// to step through, and which viewer shows it. It runs in its own process, as
// Sushi does for Nautilus, so a video or PDF that crashes a decoder takes the
// preview window down, never the file manager.
//
// Talks to the Folio window that started it over stdin and stdout, one path
// per line: Folio writes the item selected in its grid, the preview writes
// the item it steps to, and each side follows the other. ↑ and ↓ are written
// as "step -1" and "step 1": rows belong to the folder view, so Folio moves
// its selection, sends the new item back, then "stepped". Arrows pressed
// before that are let go: taken from the old item, they'd undo the move,
// and replayed later, from a file not yet looked at, they'd skip it.
class Previewer : public QObject {
    Q_OBJECT
    // Listing the folder only changes where the item sits among its
    // siblings, so it has a signal of its own: re-reading the item would set
    // a playing video's source again, which stops it.
    Q_PROPERTY(int count READ count NOTIFY listed)
    Q_PROPERTY(int index READ index NOTIFY listed)
    Q_PROPERTY(QVariantMap item READ item NOTIFY changed)
    Q_PROPERTY(QString viewer READ viewer NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    // A text's opening part, or the type of a file its name gave none, is
    // still being read: until it's in, whether it's a long text (whether ↑
    // and ↓ scroll it or step past it) isn't known.
    Q_PROPERTY(bool readingText READ readingText NOTIFY changed)
    Q_PROPERTY(QString type READ type NOTIFY changed)
    // Said in place of an archive's names: it's empty, can't be read, or needs bsdtar.
    Q_PROPERTY(QString sentence READ sentence NOTIFY changed)

public:
    explicit Previewer(QObject *parent = nullptr);
    // How a file is opened, for a test to put its own in the place of the default app: it gives what went wrong, or nothing.
    using Opener = std::function<QString(const QString &)>;
    explicit Previewer(Opener opener, QObject *parent = nullptr);

    // Shows the file right away, then lists its folder in the background so
    // the arrow keys can step to its neighbours.
    void start(const QString &path, bool showHidden, const QString &sort);
    // Reads paths from stdin and reports steps on stdout; the tests leave
    // stdin and stdout alone and talk through receive() and reported().
    void connectToFolio(bool overStdio = true);
    // One line from Folio: a path to show, or "stepped" once it has moved a
    // row for ↑ or ↓.
    void receive(const QString &line);
    // Shows another file; one in the same folder keeps the list.
    void show(const QString &path);
    // Lists the folder again, keeping the file shown: after a language
    // switch, names sort by the new alphabet.
    void relist();

    int count() const { return m_entries.size(); }
    int index() const { return m_index; }
    QVariantMap item() const;
    QString viewer() const { return m_viewer; }
    QString url() const;
    QString text() const { return m_text; }
    bool readingText() const { return m_readingText; }
    QString type() const;
    QString sentence() const { return m_sentence; }

    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // ↑ or ↓: the item above or below in the Folio window's view, or the
    // previous or next file when the preview runs on its own.
    Q_INVOKABLE void stepRow(int direction);
    Q_INVOKABLE void open();

    // Which view shows a type: image, animated, thumbnail, video, audio,
    // pdf, markdown, text, archive, folder or other.
    static QString viewerFor(const QString &mime, bool isDir);
    // Whether `bsdtar -tf` lists an archive of this type; a lone .gz, .xz or .zst isn't one.
    static bool listsArchive(const QString &mime);
    // The opening part of a text file, ending at a line when there's more,
    // or nothing when it looks binary. Laying out text takes the UI thread
    // about a third of a millisecond per KB, so the preview reads 128 KB.
    static QString readText(const QString &path, qint64 limit = 128 * 1024);
    // The top of a Markdown text up to the line where its tables pass
    // `cells` cells: laid out, each costs the UI thread about as much as a
    // whole line of prose, and a long table froze the preview for seconds.
    static QString markdownOpening(const QString &text, int cells = 600);
    // Lists an archive with `helper`: `show` gets the names so far, then all, or a sentence instead.
    static void listArchive(Helper &helper, const QString &path,
                            const std::function<void(const QString &text, const QString &sentence)> &show);

signals:
    void changed();
    void listed();
    // The item was handed to its app, and the preview has done its part.
    void opened();
    // A line for Folio: the path stepped to, or "step -1" / "step 1".
    void reported(const QString &line);

private:
    enum class Move { Previous, Next, Up, Down };
    void move(Move move);
    void stepped();
    void setIndex(int index, bool report);
    void report(const QString &line);

    Opener m_opener;
    QVector<FolderModel::Entry> m_entries;
    int m_index = -1;
    QString m_viewer;
    QString m_text;
    // The type read from what the shown file holds when its name gave none, for this showing only: a file written since is read again.
    QString m_readType;
    bool m_readingText = false;
    QString m_sentence;
    Helper m_helper{this};
    bool m_showHidden = false;
    QString m_sort = "name";
    quint64 m_generation = 0;
    // The newest item read in the background; older readings are dropped,
    // or skipped when they haven't started.
    std::shared_ptr<std::atomic<quint64>> m_reading = std::make_shared<std::atomic<quint64>>(0);
    bool m_reporting = false;
    bool m_overStdio = false;
    // Set from a "step" until Folio's "stepped"; the arrows pressed meanwhile
    // are let go.
    bool m_awaitingStep = false;
    quint64 m_step = 0;
    QSocketNotifier *m_stdin = nullptr;
    QByteArray m_pending;
};
