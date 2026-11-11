#pragma once

#include "foldermodel.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>
#include <climits>
#include <functional>
#include <vector>

class QMimeDatabase;

// One query, matched against names a byte at a time: ASCII letters of either
// case alike, other text compared once Qt has folded its case.
class SearchMatcher {
public:
    // The classes of a match, best first, far enough apart that the score
    // inside a class never reaches the next one.
    enum Tier : int {
        Exact = 5000000, // the name is the query
        Prefix = 4000000, // the name starts with it
        Word = 3500000, // a word in the name starts with it
        Named = 2000000, // its letters are in the name, in order
    };
    static constexpr int Span = 100000;
    // A match whose score inside its class is only worked out if it's shown.
    static constexpr int Pending = -2;
    static constexpr int NoMatch = INT_MIN;

    explicit SearchMatcher(const QString &query);
    const QByteArray &needle() const { return m_needle; }
    quint64 bits() const { return m_bits; }
    // The class `text` matches in, Pending when that's Named, or NoMatch.
    int name(QByteArrayView text) const;
    // A Named match's score: runs, word starts and letters close together
    // earn more.
    int align(QByteArrayView text) const;
    // Whether every match of `now` is among the matches of `before`.
    static bool canNarrow(const QString &before, const QString &now);
    // One bit for each letter, digit and common mark in `text`: a name that
    // lacks one of the query's can't match it.
    static quint64 bitsOf(QByteArrayView text);
    static constexpr quint64 Unknown = 1ull << 63;
    static constexpr quint64 NotAscii = 1ull << 62;

    // The score inside a class, read a piece of the path at a time: each of
    // the query's letters placed, in order, earns more next to the last one,
    // at the start of a word and in the row's own name, and a letter
    // skipped costs one.
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
};

// Everything below a folder by name, read once for a search. Names stay the
// bytes the folder gave, each kept once however many folders hold it, and a
// row's path is put together from its folders only when it's shown.
class SearchIndex {
public:
    // What reading and answering cost, for the tests.
    struct Counts {
        qsizetype opens = 0; // folders opened
        qsizetype stats = 0; // one for each folder opened, each row shown, and each entry of unknown type
        qsizetype walks = 0;
        qsizetype names = 0; // names matched, each once a query however many rows share it
    };

    // Reads `root` and every folder below it, breadth first, staying on its
    // drive and out of symlinked folders. False, and empty, once `stop`
    // returns true.
    bool walk(const QString &root, bool showHidden, const std::function<bool()> &stop = {});
    qsizetype size() const { return m_rows.size(); }
    // The memory it holds.
    qsizetype bytes() const;
    // The row's path from the root, in the folder's own bytes.
    QByteArray path(quint32 row) const;
    // The row as the views show it, named by its path from the root.
    FolderModel::Entry entry(quint32 row, const QMimeDatabase &mimes);
    // The rows as entries; empty once `stop` returns true.
    QVector<FolderModel::Entry> entries(const QVector<quint32> &rows, const std::function<bool()> &stop = {});
    // Matches `query`, best first, and gives how many rows match. The rows
    // themselves come from rows(), ordered only as far as they're asked for.
    qsizetype answer(const QString &query);
    // The answer's rows from `from`, at most `count` of them, best first:
    // a better class first, then a better score, a folder before a file,
    // the shorter path, and the paths in natural order.
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
    // A folder is its name, the folder holding it and the length of its
    // path; a row is its name and its folder.
    struct Folder { quint32 name, parent, length; };
    struct Row { quint32 name, parent; };
    struct Hit { int score; quint32 row; };

    quint32 intern(QByteArrayView text);
    QByteArrayView name(quint32 id) const;
    // The name as the matcher reads it: folded first when it isn't ASCII.
    QByteArrayView text(quint32 id, QByteArray &folded) const;
    QByteArray joined(quint32 folder, QByteArrayView last) const;
    bool isDir(quint32 row) const { return m_kinds[row / 8] & (1 << (row % 8)); }
    qsizetype pathLength(quint32 row) const;
    int score(quint32 id, QByteArray &folded) const;
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

    // The last answer: its query and matcher, its matches, how many of them
    // are in their final order, and how many rows there were to match.
    QString m_query;
    SearchMatcher m_matcher{QString()};
    QVector<Hit> m_hits;
    qsizetype m_ordered = 0;
    qsizetype m_answered = -1;
    // Each name's score for the query the stamp is for.
    quint32 m_stamp = 0;
    QVector<quint32> m_stamps;
    QVector<int> m_scores;
};
