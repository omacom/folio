#pragma once

#include "foldermodel.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QVector>
#include <climits>
#include <functional>
#include <vector>

class QMimeDatabase;
class QMutex;

// One query, matched against names a byte at a time: ASCII letters of either case alike, other text compared once Qt has folded its case.
class SearchMatcher {
public:
    // The classes of a match, best first, far enough apart that the score inside a class never reaches the next one.
    enum Tier : int {
        Exact = 5000000, // the name is the query
        Prefix = 4000000, // the name starts with it
        Word = 3500000, // a word in the name starts with it
        Aligned = 3000000, // with a "/", its folders hold what's before it and the name the rest
        Typo = 2500000, // the name is a letter or two off, when no name spells the query out
        Named = 2000000, // its letters are in the name, or along the path, in order
    };
    static constexpr int Span = 100000;
    // A match whose score inside its class is only worked out if it's shown: Aligned, Named in the name, or Named along the path.
    static constexpr int PendingAligned = -3, Pending = -2, PendingPath = -1;
    static constexpr int NoMatch = INT_MIN;

    explicit SearchMatcher(const QString &query);
    const QByteArray &needle() const { return m_needle; }
    quint64 bits() const { return m_bits; }
    // The class `text` matches in, Pending when that's Named, or NoMatch.
    int name(QByteArrayView text) const;
    // A Named match's score: runs, word starts and letters close together earn more.
    int align(QByteArrayView text) const;
    // How many letters a name may be off from the query: none when the query doesn't start with a letter or a digit.
    int typos() const { return m_typos; }
    // The Typo score of a name that far off at most, from the start of a word, or NoMatch.
    int typo(QByteArrayView text) const;
    // Whether `text` spells the query out: whole anywhere, or in runs that each start a word, as framen does in frame_new.
    bool spells(QByteArrayView text) const;
    // How far into the query `text` takes it, from `at`, its letters taken in order.
    qsizetype reach(QByteArrayView text, qsizetype at) const;
    // The query's length up to and with its last "/", which the folders have to take it past; 0 without one.
    qsizetype folderPart() const { return m_folderPart; }
    // The bits of the query from `at` on.
    quint64 bitsFrom(qsizetype at) const { return m_bitsFrom[at]; }
    // Whether every match of `now` is among the matches of `before`.
    static bool canNarrow(const QString &before, const QString &now);
    // One bit for each letter, digit and common mark in `text`: a name that lacks one of the query's can't match it.
    static quint64 bitsOf(QByteArrayView text);
    // Where the digits' bits start, after the letters', and the marks' after those.
    static constexpr int DigitBit = 26, MarkBit = DigitBit + 10;
    static constexpr quint64 Unknown = 1ull << 63;
    static constexpr quint64 NotAscii = 1ull << 62;

    // The score inside a class, read a piece of the path at a time: a letter placed earns more next to the last, at a word's start and in the row's own name; one skipped costs one.
    struct Alignment {
        static constexpr int Dead = INT_MIN / 4;
        static constexpr int RunBonus = 8, WordBonus = 6, OwnNameBonus = 4;
        std::vector<int> best, end;
        int hit = Dead;
        void reset(qsizetype length);
        void advance(QByteArrayView needle, QByteArrayView text, bool own);
        int score(int tier) const;
    };

private:
    bool startsAt(QByteArrayView text, qsizetype at) const;

    QByteArray m_needle;
    quint64 m_bits = 0;
    qsizetype m_folderPart = 0;
    std::vector<quint64> m_bitsFrom;
    int m_typos = 0;
    // The rows of the typo pass's table, kept between names.
    mutable std::vector<int> m_older, m_previous, m_current;
};

// Everything below a folder by name, read once for a search: names stay the folder's bytes, each kept once, and a row's path is put together only when it's shown.
class SearchIndex {
public:
    // What reading and answering cost, for the tests.
    struct Counts {
        qsizetype opens = 0; // folders opened
        qsizetype stats = 0; // one for each folder opened, each row shown, and each entry of unknown type
        qsizetype walks = 0;
        qsizetype names = 0; // names matched, each once a query however many rows share it
        qsizetype folders = 0; // folders matched along a path, each at most once a query
        qsizetype typos = 0; // passes looking for names a letter or two off
        qsizetype answers = 0;
    };

    // Reads `root` and the folders below, breadth first, on its drive, past no link, up to mostRows names. False and empty once `stop` says; adding under `lock`, it's answered from as it reads.
    bool walk(const QString &root, bool showHidden, const std::function<bool()> &stop = {}, QMutex *lock = nullptr);
    // Called with how many folders have been read, after each, so a test can hold a walk partway.
    inline static std::function<void(qsizetype folders)> afterFolder;
    // The most names a walk keeps: with more below, it ends with the nearest and cut() says so.
    static constexpr qsizetype MostRows = 2'000'000;
    // MostRows, until a test lowers it.
    inline static qsizetype mostRows = MostRows;
    // Whether the walk left names unread.
    bool cut() const { return m_cut; }
    qsizetype size() const { return m_rows.size(); }
    // The memory it holds.
    qsizetype bytes() const;
    // The row's path from the root, in the folder's own bytes.
    QByteArray path(quint32 row) const;
    const QString &root() const { return m_root; }
    // The rows' paths from the root, as the views name them: read from memory, so the lock is never held across the disk.
    QStringList names(const QVector<quint32> &rows) const;
    // The full paths of names(), as their entries carry them.
    static QStringList paths(const QString &root, const QStringList &names);
    // names() as entries, each asked of the disk; empty once `stop` returns true.
    static QVector<FolderModel::Entry> entriesAt(const QString &root, const QStringList &names, const std::function<bool()> &stop = {});
    // The rows as entries; empty once `stop` returns true.
    QVector<FolderModel::Entry> entries(const QVector<quint32> &rows, const std::function<bool()> &stop = {});
    // Matches `query`, best first, and gives how many rows match. The rows themselves come from rows(), ordered only as far as they're asked for.
    qsizetype answer(const QString &query);
    // The query the last answer was for.
    const QString &query() const { return m_query; }
    // The answer's rows from `from`, at most `count`, best first: by class, by score, a folder before a file, the shorter path, then the paths in natural order.
    QVector<quint32> rows(qsizetype from, qsizetype count);
    // The best `limit` rows that match `query`, sorted by `sort` in `locale`'s alphabet. Empty once `stop` returns true.
    QVector<FolderModel::Entry> find(const QString &query, const QString &sort, int limit,
                                     const std::function<bool()> &stop = {}, const QLocale &locale = QLocale());

    Counts counts;

private:
    static constexpr quint32 None = UINT32_MAX;
    // Names are kept in slabs this big, which go back to the system with the search.
    static constexpr qsizetype SlabBytes = 1 << 20;
    struct Name { quint32 offset, length; quint64 bits; };
    // A folder is its name, the folder holding it, the length of its path and the bits of every name along it; a row is its name and its folder.
    struct Folder { quint32 name, parent, length; quint64 bits; };
    struct Row { quint32 name, parent; };
    struct Hit { int score; quint32 row; };

    quint32 intern(QByteArrayView text);
    QByteArrayView name(quint32 id) const;
    // The name as the matcher reads it: what's past ASCII folded first.
    QByteArrayView text(quint32 id, QByteArray &folded) const;
    QByteArray joined(quint32 folder, QByteArrayView last) const;
    bool isDir(quint32 row) const { return m_kinds[row / 8] & (1 << (row % 8)); }
    qsizetype pathLength(quint32 row) const;
    int score(quint32 id, QByteArray &folded) const;
    qsizetype reach(quint32 folder);
    int alongPath(quint32 row, int tier, QHash<quint32, SearchMatcher::Alignment> &above);
    void restamp();
    bool before(const Hit &a, const Hit &b) const;

    QString m_root;
    std::vector<QByteArray> m_slabs;
    QVector<Name> m_names;
    // Open addressing over m_names, so a name seen before isn't kept again.
    QVector<quint32> m_lookup;
    QVector<Folder> m_folders;
    QVector<Row> m_rows;
    // A bit for each row, set for a folder.
    QByteArray m_kinds;
    bool m_cut = false;

    // The last answer: its query and matcher, its matches, how many of them are in their final order, and how many rows there were to match.
    QString m_query;
    SearchMatcher m_matcher{QString()};
    QVector<Hit> m_hits;
    qsizetype m_ordered = 0;
    qsizetype m_answered = -1;
    // Each name's score for the query the stamp is for.
    quint32 m_stamp = 0;
    QVector<quint32> m_stamps;
    QVector<int> m_scores;
    // Each folder's reach into the query, for the query its stamp is for.
    QVector<quint32> m_folderStamps;
    QVector<quint32> m_reaches;
};
