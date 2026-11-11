#pragma once

#include "foldermodel.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>
#include <functional>
#include <vector>

class QMimeDatabase;

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
    };

    // Reads `root` and every folder below it, breadth first, staying on its
    // drive and out of symlinked folders. False, and empty, once `stop`
    // returns true.
    bool walk(const QString &root, bool showHidden, const std::function<bool()> &stop = {});
    qsizetype size() const { return m_rows.size(); }
    // The row's path from the root, in the folder's own bytes.
    QByteArray path(quint32 row) const;
    // The row as the views show it, named by its path from the root.
    FolderModel::Entry entry(quint32 row, const QMimeDatabase &mimes);
    // The first `limit` rows, in the walk's order, whose name contains
    // `query` ignoring case, sorted by `sort` in `locale`'s alphabet. Empty once `stop` returns true.
    QVector<FolderModel::Entry> find(const QString &query, const QString &sort, int limit,
                                     const std::function<bool()> &stop = {}, const QLocale &locale = QLocale());

    Counts counts;

private:
    static constexpr quint32 None = UINT32_MAX;
    // Names are kept in slabs this big, which go back to the system with the search.
    static constexpr qsizetype SlabBytes = 1 << 20;
    struct Name { quint32 offset, length; };
    // A folder is its name and the folder holding it; a row likewise.
    struct Folder { quint32 name, parent; };
    struct Row { quint32 name, parent; };

    quint32 intern(QByteArrayView text);
    QByteArrayView name(quint32 id) const;
    QByteArray joined(quint32 folder, QByteArrayView last) const;

    QString m_root;
    std::vector<QByteArray> m_slabs;
    QVector<Name> m_names;
    // Open addressing over m_names, so a name seen before isn't kept again.
    QVector<quint32> m_lookup;
    QVector<Folder> m_folders;
    QVector<Row> m_rows;
};
