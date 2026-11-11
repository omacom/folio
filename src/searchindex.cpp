#include "searchindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

bool SearchIndex::walk(const QString &root, bool showHidden, const std::function<bool()> &stop) {
    ++counts.walks;
    m_root = root;
    const QByteArray top = QFile::encodeName(root);
    dev_t drive = 0;
    // The folders found are the queue: each is read in the order it was
    // found, which is breadth first. -1 is the root.
    for (qsizetype folder = -1; folder < m_folders.size(); ++folder) {
        const quint32 parent = folder < 0 ? None : quint32(folder);
        const QByteArray at = folder < 0 ? top : top + '/' + joined(m_folders[folder].parent, name(m_folders[folder].name));
        // Below the root, a folder that's become a link since it was listed isn't followed.
        const int fd = ::open(at.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | (folder < 0 ? 0 : O_NOFOLLOW));
        if (fd < 0)
            continue;
        ++counts.opens;
        ++counts.stats;
        struct stat st;
        // Other filesystems (a mounted drive, another machine, /proc from
        // /) are listed but not read.
        if (::fstat(fd, &st) != 0 || (folder >= 0 && st.st_dev != drive)) {
            ::close(fd);
            continue;
        }
        if (folder < 0)
            drive = st.st_dev;
        DIR *dir = ::fdopendir(fd);
        if (!dir) {
            ::close(fd);
            continue;
        }
        while (const dirent *entry = ::readdir(dir)) {
            if (stop && stop()) {
                ::closedir(dir);
                const Counts spent = counts;
                *this = SearchIndex();
                counts = spent;
                return false;
            }
            const char *text = entry->d_name;
            if (text[0] == '.' && (!showHidden || text[1] == 0 || (text[1] == '.' && text[2] == 0)))
                continue;
            // The type comes with the name; only a filesystem that doesn't
            // say needs asking. Symlinked folders aren't walked into.
            bool isDir = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {
                ++counts.stats;
                struct stat child;
                isDir = ::fstatat(::dirfd(dir), text, &child, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(child.st_mode);
            }
            const quint32 id = intern(QByteArrayView(text));
            m_rows.push_back({id, parent});
            if (isDir)
                m_folders.push_back({id, parent});
        }
        ::closedir(dir);
    }
    // Names are only looked up while folders are read, and what's left
    // over from growing goes back.
    m_lookup = {};
    m_names.squeeze();
    m_folders.squeeze();
    m_rows.squeeze();
    return true;
}

qsizetype SearchIndex::bytes() const {
    qsizetype slabs = 0;
    for (const QByteArray &slab : m_slabs)
        slabs += slab.capacity();
    return slabs + m_names.capacity() * sizeof(Name) + m_lookup.capacity() * sizeof(quint32)
        + m_folders.capacity() * sizeof(Folder) + m_rows.capacity() * sizeof(Row);
}

quint32 SearchIndex::intern(QByteArrayView text) {
    // Kept under three quarters full, so a probe ends soon.
    if ((m_names.size() + 1) * 4 > m_lookup.size() * 3) {
        QVector<quint32> bigger(qMax<qsizetype>(1024, m_lookup.size() * 2), None);
        for (quint32 id = 0; id < quint32(m_names.size()); ++id) {
            size_t at = qHash(name(id)) & (bigger.size() - 1);
            while (bigger[at] != None)
                at = (at + 1) & (bigger.size() - 1);
            bigger[at] = id;
        }
        m_lookup.swap(bigger);
    }
    size_t at = qHash(text) & (m_lookup.size() - 1);
    for (; m_lookup[at] != None; at = (at + 1) & (m_lookup.size() - 1))
        if (name(m_lookup[at]) == text)
            return m_lookup[at];
    // A name never runs over the end of its slab.
    if (m_slabs.empty() || m_slabs.back().size() + text.size() > SlabBytes) {
        m_slabs.emplace_back();
        m_slabs.back().reserve(SlabBytes);
    }
    QByteArray &slab = m_slabs.back();
    const quint32 id = quint32(m_names.size());
    m_names.push_back({quint32((m_slabs.size() - 1) * SlabBytes + slab.size()), quint32(text.size())});
    slab.append(text);
    m_lookup[at] = id;
    return id;
}

QByteArrayView SearchIndex::name(quint32 id) const {
    const Name &n = m_names[id];
    return QByteArrayView(m_slabs[n.offset / SlabBytes].constData() + n.offset % SlabBytes, n.length);
}

QByteArray SearchIndex::joined(quint32 folder, QByteArrayView last) const {
    QByteArray out = last.toByteArray();
    for (; folder != None; folder = m_folders[folder].parent)
        out.prepend('/').prepend(name(m_folders[folder].name));
    return out;
}

QByteArray SearchIndex::path(quint32 row) const {
    return joined(m_rows[row].parent, name(m_rows[row].name));
}

FolderModel::Entry SearchIndex::entry(quint32 row, const QMimeDatabase &mimes) {
    const QString relative = QFile::decodeName(path(row));
    ++counts.stats;
    FolderModel::Entry e = FolderModel::entryFor(QFileInfo(QDir(m_root).filePath(relative)), mimes);
    e.name = relative;
    return e;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

// Whether `text` holds `needle`, ASCII letters of either case alike; the
// needle is in lower case.
static bool containsAscii(QByteArrayView text, QByteArrayView needle) {
    for (qsizetype at = 0; at + needle.size() <= text.size(); ++at) {
        qsizetype i = 0;
        while (i < needle.size() && lower(text[at + i]) == needle[i])
            ++i;
        if (i == needle.size())
            return true;
    }
    return false;
}

static bool isAscii(QByteArrayView text) {
    for (char c : text)
        if (uchar(c) >= 0x80)
            return false;
    return true;
}

QVector<FolderModel::Entry> SearchIndex::find(const QString &query, const QString &sort, int limit,
                                              const std::function<bool()> &stop, const QLocale &locale) {
    // Bytes on both sides when both are ASCII; otherwise Qt's own
    // comparison, as for any name.
    const QByteArray needle = query.toLower().toUtf8();
    const bool asciiQuery = isAscii(needle);
    QVector<FolderModel::Entry> entries;
    QMimeDatabase mimes;
    for (quint32 row = 0; row < quint32(m_rows.size()) && entries.size() < limit; ++row) {
        const QByteArrayView text = name(m_rows[row].name);
        const bool found = asciiQuery && isAscii(text) ? containsAscii(text, needle)
                                                       : QString::fromUtf8(text).contains(query, Qt::CaseInsensitive);
        if (!found)
            continue;
        if (stop && stop())
            return {};
        entries.push_back(entry(row, mimes));
    }
    FolderModel::sortEntries(entries, sort, locale);
    return entries;
}
