#pragma once

#include <QAbstractListModel>
#include <functional>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocale>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QSet>
#include <QVector>
#include <atomic>
#include <memory>

// One folder's contents, listed off the UI thread so a slow mount (an SFTP
// share on another machine, say) never freezes the window. Listing happens in
// two passes: the entries first, then each subfolder's item count.
class QFileInfo;
class QMimeDatabase;
class SearchIndex;

class FolderModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // Set, the folder lists what matches it here and in every folder below.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // The items a plugin's view lists, as Plugins::views gives them.
    Q_PROPERTY(QVariantList items READ items WRITE setItems NOTIFY itemsChanged)
    // How many match the query, shown or not; -1 until a search has answered.
    Q_PROPERTY(int matches READ matches NOTIFY matchesChanged)
    // While a search is still reading the folders below.
    Q_PROPERTY(bool reading READ reading NOTIFY readingChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        SizeRole,
        ModifiedRole,
        KindRole,
        MimeRole,
        ExtensionRole,
        ItemCountRole,
        SelectedRole,
        IconRole,
    };

    struct Entry {
        QString name;
        QString path;
        bool isDir = false;
        // Listed by name, with the size and date left for when a view asks for them.
        bool unread = false;
        qint64 size = 0;
        QDateTime modified;
        // Null for a file or folder the model listed: typed() reads them from its name when they're asked for.
        QString kind;
        QString mime;
        // A file's extension; a plugin item's subtitle.
        QString extension;
        // A plugin item's glyph and its rows for the details pane.
        QString icon;
        QVariantList details;
        int itemCount = -1;
        // The first pictures in a folder, for the preview window.
        QStringList peek;
    };

    explicit FolderModel(QObject *parent = nullptr);
    ~FolderModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    int count() const { return m_entries.size(); }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    QString sortMode() const { return m_sort; }
    void setSortMode(const QString &sort);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QVariantList items() const { return m_items; }
    void setItems(const QVariantList &items);
    int matches() const { return m_matches; }
    bool reading() const { return m_stillReading; }

    void setPath(const QString &path);
    Q_INVOKABLE void reload();
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOf(const QString &name) const;
    // By path, for Recents, where two files from different folders can
    // share a name.
    Q_INVOKABLE int indexOfPath(const QString &path) const;

    // The selection, kept by path so it survives a re-read or a re-sort.
    // The views keep the cursor (the current item) themselves.
    int selectedCount() const { return m_selected.size(); }
    Q_INVOKABLE void selectOnly(int row);
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void selectRows(const QList<int> &rows);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(int row) const;
    Q_INVOKABLE QStringList selectedPaths() const;

    // Rows a listing reads sizes and dates for: 13 rows of 22 smallest tiles fill a 4K screen and what a view builds ahead.
    static constexpr int firstRows = 320;
    // How many names' types a model keeps: the rows in sight and many folders back, never all of a large folder scrolled through.
    static constexpr int typesKept = 4096;

    // Synchronous building blocks, used by the background passes and tests.
    // A listing stops, empty, as soon as `stop` returns true. Names sort
    // by `locale`'s alphabet; a background pass passes the one it was
    // started with, because the default locale may only be read on the
    // thread that sets it. Not whole, it leaves types, and sizes and dates past firstRows, unread.
    static QVector<Entry> list(const QString &path, bool showHidden, const QString &sort, QString *error,
                               const std::function<bool()> &stop = {}, const QLocale &locale = QLocale(),
                               bool whole = true);
    static Entry entryFor(const QFileInfo &info, const QMimeDatabase &mimes);
    // The entry with its kind, MIME type and extension, read from its name when it was listed untyped.
    static Entry typed(const Entry &entry);
    // An entry as QML reads it, by the names of its fields.
    static QVariantMap toMap(const Entry &entry);
    // A folder's item count and first pictures, for the preview window.
    static void summarize(Entry &entry, bool showHidden);
    // Only how many items a folder holds, for the folder counts, which have
    // no use for its pictures or their order.
    static int itemCount(const QString &folder, bool showHidden);
    static QStringList sortModes();
    static void sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale = QLocale());
    // Sorts `entries` by merging the ascending runs they arrived in, as stable_sort would; false, untouched, when they're too many.
    static bool mergeRuns(QVector<Entry> &entries, const std::function<bool(const Entry &, const Entry &)> &less);
    // The Recents view: a path that isn't a folder, listing recently used files.
    static QString recentPath() { return QStringLiteral("recent:"); }
    static QString recentFile();
    static QVector<Entry> listRecent(const QString &xbel, bool showHidden, int limit = 200,
                                     const std::function<bool()> &stop = {});
    // A plugin's view: plugin:<id>, listing `items` in the plugin's order,
    // each an item of the "item" kind at plugin:<id>/<item id>. The ones
    // whose names contain `query` (ignoring case), or all of them.
    static bool isPluginView(const QString &path) { return path.startsWith("plugin:") && !path.contains('/'); }
    static QVector<Entry> listItems(const QVariantList &items, const QString &query = {});
    // Recents and plugins' views: lists of their own, not folders.
    static bool isView(const QString &path) { return path == recentPath() || isPluginView(path); }
    static QString kindForMime(const QString &mime);
    // The best `limit` matches for `query` under `root`, named from `root`; stops at other drives, symlinks and when `stop` says.
    static QVector<Entry> search(const QString &root, const QString &query, bool showHidden, const QString &sort,
                                 const std::function<bool()> &stop = {}, int limit = 1000,
                                 const QLocale &locale = QLocale());
    // What the open search has read, for the tests; null with no search open.
    std::shared_ptr<const SearchIndex> searchIndex() const;

signals:
    void pathChanged();
    void countChanged();
    void loadingChanged();
    void errorChanged();
    void showHiddenChanged();
    void sortModeChanged();
    void queryChanged();
    void itemsChanged();
    void matchesChanged();
    void readingChanged();
    void selectionChanged();
    void loaded();

private:
    struct Search;
    void read();
    void openSearch();
    void closeSearch();
    void setReading(bool reading);
    // Answers from what's been read so far, past the hold.
    void readEarly();
    void setLoading(bool loading);
    void setEntries(const QVector<Entry> &entries);
    void clearEntries();
    QString cacheKey() const;
    void remember();
    static bool sameListing(const QVector<Entry> &a, const QVector<Entry> &b);
    void takeSizesAndDates(QVector<Entry> entries);
    // A row's kind, MIME type and extension as its roles give them.
    struct Type {
        QString kind, mime, extension;
    };
    Type typeOf(const Entry &entry) const;
    void ask(int row) const;
    void readAsked();
    void setError(const QString &error);
    void countFolders(quint64 generation);

    QString m_path;
    QVector<Entry> m_entries;
    QSet<QString> m_selected;
    void setSelected(const QSet<QString> &paths);
    bool m_loading = false;
    QString m_error;
    bool m_showHidden = false;
    QString m_sort = "name";
    QString m_query;
    QVariantList m_items;
    int m_matches = -1;
    bool m_fetching = false;
    // The rows a search showed with its whole answer: read again, it shows as many, so a selection past the first window stays.
    qsizetype m_shownRows = 0;
    // The matches a Select All took that aren't rows yet, best first: the selection holds them by path.
    QStringList m_unshown;
    bool m_stillReading = false;
    quint64 m_generation = 0;
    // The newest listing's generation, shared with the background listing,
    // search and folder counts, so work that's been overtaken (more typing,
    // another folder, the model gone) stops early.
    std::shared_ptr<std::atomic<quint64>> m_latest = std::make_shared<std::atomic<quint64>>(0);
    // What typed() read from a file's name, kept by that name until a folder is found changed: a view asks for a row's type three times each time it makes the row.
    mutable QHash<QString, Type> m_types;
    // Rows a view asked for a size or date this turn, read together, and every row asked for and not back yet.
    mutable QList<int> m_asked;
    mutable QSet<int> m_reading;
    // Rows asked for and not read by the try that followed: a view may ask again, and the folder's next re-read reads them.
    mutable QSet<int> m_owed;
    // A get() answered without a size or date, so QML takes its snapshots again (on countChanged) once they're in.
    mutable bool m_snapshotWaiting = false;
    // The folders below, read on a search's first letter and kept until it ends, the folder changes or hidden files are shown or hidden.
    std::shared_ptr<Search> m_search;
    // The same folders read again (F5, a change on disk) while m_search answers; it takes its place once it has answered.
    std::shared_ptr<Search> m_next;
    // A change on disk came while a walk ran: the folders are read once more when it's whole.
    bool m_again = false;
    // The query the rows are an answer to; empty while they're the folder's own.
    QString m_rowsQuery;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_staleClear;
    QTimer m_hold;
    QTimer m_refresh;
};
