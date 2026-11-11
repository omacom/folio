#include "searchindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

static bool isSeparator(char c) {
    return c == '/' || c == '-' || c == '_' || c == '.' || c == ' ';
}

// A word starts after a separator, and at a capital after a small letter
// or a digit ("renderFrame", "2024Report").
static bool wordStart(QByteArrayView text, qsizetype at) {
    return at == 0 || isSeparator(text[at - 1]) || (lower(text[at]) != text[at] && lower(text[at - 1]) == text[at - 1]);
}

quint64 SearchMatcher::bitsOf(QByteArrayView text) {
    // a to z, then 0 to 9, then / - _ . space +; anything else is Unknown.
    static const char marks[] = "/-_. +";
    quint64 bits = 0;
    for (char raw : text) {
        const char c = lower(raw);
        const char *mark = c ? std::strchr(marks, c) : nullptr;
        if (c >= 'a' && c <= 'z')
            bits |= 1ull << (c - 'a');
        else if (c >= '0' && c <= '9')
            bits |= 1ull << (26 + c - '0');
        else if (mark)
            bits |= 1ull << (36 + (mark - marks));
        else
            bits |= Unknown;
    }
    return bits;
}

SearchMatcher::SearchMatcher(const QString &query)
    : m_needle(query.toCaseFolded().toUtf8()), m_bits(bitsOf(m_needle)) {}

bool SearchMatcher::canNarrow(const QString &before, const QString &now) {
    return !before.isEmpty() && now.startsWith(before);
}

bool SearchMatcher::startsAt(QByteArrayView text, qsizetype at) const {
    if (at + m_needle.size() > text.size())
        return false;
    for (qsizetype i = 0; i < m_needle.size(); ++i)
        if (lower(text[at + i]) != m_needle[i])
            return false;
    return true;
}

int SearchMatcher::name(QByteArrayView text) const {
    if (m_needle.isEmpty())
        return NoMatch;
    if (startsAt(text, 0))
        return text.size() == m_needle.size() ? Exact : Prefix;
    qsizetype matched = 0;
    for (qsizetype at = 0; at < text.size(); ++at) {
        if (at > 0 && lower(text[at]) == m_needle[0] && wordStart(text, at) && startsAt(text, at))
            return Word;
        if (matched < m_needle.size() && m_needle[matched] == lower(text[at]))
            ++matched;
    }
    return matched == m_needle.size() ? Pending : NoMatch;
}

void SearchMatcher::Alignment::reset(qsizetype length) {
    best.assign(length + 1, Dead);
    end.assign(length + 1, Dead);
    best[0] = 0;
    hit = length == 0 ? 0 : Dead;
}

// best[k]: the best score with the query's first k letters placed so far;
// end[k]: the same with the k-th placed on the letter just read.
void SearchMatcher::Alignment::advance(QByteArrayView needle, QByteArrayView text, bool own) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char c = lower(text[i]);
        const int bonus = (wordStart(text, i) ? WordBonus : 0) + (own ? OwnNameBonus : 0);
        for (qsizetype k = needle.size(); k >= 1; --k) {
            const int matched = needle[k - 1] == c ? std::max(best[k - 1], end[k - 1] + RunBonus) + bonus : Dead;
            best[k] = std::max(best[k] - 1, matched);
            end[k] = matched;
        }
        hit = std::max(hit, end[needle.size()]);
    }
}

int SearchMatcher::Alignment::score(int tier) const {
    return hit > Dead / 2 ? tier + std::clamp(hit, -Span, Span) : tier - Span;
}

int SearchMatcher::align(QByteArrayView text) const {
    Alignment alignment;
    alignment.reset(m_needle.size());
    alignment.advance(m_needle, text, true);
    return alignment.score(Named);
}

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
            if (m_rows.size() % 8 == 0)
                m_kinds.append('\0');
            if (isDir) {
                m_kinds.back() = char(m_kinds.back() | (1 << (m_rows.size() % 8)));
                const quint32 above = parent == None ? 0 : m_folders[parent].length + 1;
                m_folders.push_back({id, parent, above + m_names[id].length});
            }
            m_rows.push_back({id, parent});
        }
        ::closedir(dir);
    }
    // Names are only looked up while folders are read, and what's left
    // over from growing goes back.
    m_lookup = {};
    m_names.squeeze();
    m_folders.squeeze();
    m_rows.squeeze();
    m_kinds.squeeze();
    return true;
}

qsizetype SearchIndex::bytes() const {
    qsizetype slabs = 0;
    for (const QByteArray &slab : m_slabs)
        slabs += slab.capacity();
    return slabs + m_names.capacity() * sizeof(Name) + m_lookup.capacity() * sizeof(quint32)
        + m_folders.capacity() * sizeof(Folder) + m_rows.capacity() * sizeof(Row) + m_kinds.capacity()
        + m_hits.capacity() * sizeof(Hit) + m_stamps.capacity() * sizeof(quint32) + m_scores.capacity() * sizeof(int);
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
    // Folded first when it isn't ASCII, so the letters are the ones it's matched by.
    const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return uchar(c) < 0x80; });
    const quint64 bits = ascii ? SearchMatcher::bitsOf(text)
                               : SearchMatcher::bitsOf(QString::fromUtf8(text).toCaseFolded().toUtf8()) | SearchMatcher::NotAscii;
    m_names.push_back({quint32((m_slabs.size() - 1) * SlabBytes + slab.size()), quint32(text.size()), bits});
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

QVector<FolderModel::Entry> SearchIndex::entries(const QVector<quint32> &rows, const std::function<bool()> &stop) {
    QVector<FolderModel::Entry> out;
    QMimeDatabase mimes;
    for (quint32 row : rows) {
        if (stop && stop())
            return {};
        out.push_back(entry(row, mimes));
    }
    return out;
}

QByteArrayView SearchIndex::text(quint32 id, QByteArray &folded) const {
    if (!(m_names[id].bits & SearchMatcher::NotAscii))
        return name(id);
    folded = QString::fromUtf8(name(id)).toCaseFolded().toUtf8();
    return folded;
}

qsizetype SearchIndex::pathLength(quint32 row) const {
    const Row &r = m_rows[row];
    return (r.parent == None ? 0 : m_folders[r.parent].length + 1) + m_names[r.name].length;
}

int SearchIndex::score(quint32 id, QByteArray &folded) const {
    const Name &n = m_names[id];
    const quint64 wanted = m_matcher.bits();
    // A letter the query has and the name lacks rules it out unread.
    if (!((wanted | n.bits) & SearchMatcher::Unknown) && (wanted & ~n.bits))
        return SearchMatcher::NoMatch;
    return m_matcher.name(text(id, folded));
}

qsizetype SearchIndex::answer(const QString &query) {
    // A longer query's matches are among the last one's, as long as
    // nothing's been read since.
    QVector<quint32> within;
    const bool narrow = m_answered == m_rows.size() && SearchMatcher::canNarrow(m_query, query);
    if (narrow) {
        within.reserve(m_hits.size());
        for (const Hit &hit : std::as_const(m_hits))
            within.push_back(hit.row);
    }
    m_query = query;
    m_matcher = SearchMatcher(query);
    m_answered = m_rows.size();
    m_hits.clear();
    m_ordered = 0;
    // Each name is matched once, however many rows share it.
    if (++m_stamp == 0) {
        m_stamps.fill(0);
        m_stamp = 1;
    }
    m_stamps.resize(m_names.size());
    m_scores.resize(m_names.size());
    QByteArray folded;
    const qsizetype count = narrow ? within.size() : m_rows.size();
    for (qsizetype i = 0; i < count; ++i) {
        const quint32 row = narrow ? within[i] : quint32(i);
        const quint32 id = m_rows[row].name;
        if (m_stamps[id] != m_stamp) {
            m_stamps[id] = m_stamp;
            m_scores[id] = score(id, folded);
            ++counts.names;
        }
        if (m_scores[id] != SearchMatcher::NoMatch)
            m_hits.push_back({m_scores[id], row});
    }
    return m_hits.size();
}

// The classes, best first; a Pending score is in Named.
static int classOf(int score) {
    static constexpr int tiers[] = {SearchMatcher::Exact, SearchMatcher::Prefix, SearchMatcher::Word};
    if (score != SearchMatcher::Pending)
        for (int i = 0; i < int(std::size(tiers)); ++i)
            if (score >= tiers[i] - SearchMatcher::Span)
                return i;
    return int(std::size(tiers));
}
static constexpr int Classes = 4;

// Paths in the order people number things: "photo9" before "photo10".
static int natural(QByteArrayView a, QByteArrayView b) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            qsizetype ie = i, je = j;
            while (ie < a.size() && digit(a[ie]))
                ++ie;
            while (je < b.size() && digit(b[je]))
                ++je;
            while (i + 1 < ie && a[i] == '0')
                ++i;
            while (j + 1 < je && b[j] == '0')
                ++j;
            if (ie - i != je - j)
                return ie - i < je - j ? -1 : 1;
            if (const int order = a.sliced(i, ie - i).compare(b.sliced(j, je - j)))
                return order;
            i = ie;
            j = je;
        } else {
            const char x = lower(a[i]), y = lower(b[j]);
            if (x != y)
                return uchar(x) < uchar(y) ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i == a.size() && j == b.size())
        return a.compare(b);
    return i == a.size() ? -1 : 1;
}

bool SearchIndex::before(const Hit &a, const Hit &b) const {
    if (a.score != b.score)
        return a.score > b.score;
    if (isDir(a.row) != isDir(b.row))
        return isDir(a.row);
    const qsizetype la = pathLength(a.row), lb = pathLength(b.row);
    if (la != lb)
        return la < lb;
    return natural(path(a.row), path(b.row)) < 0;
}

QVector<quint32> SearchIndex::rows(qsizetype from, qsizetype count) {
    const qsizetype end = qMin(from + count, m_hits.size());
    if (end > m_ordered) {
        // Only the classes the rows reach are scored inside and ordered.
        const qsizetype need = end - m_ordered;
        qsizetype inClass[Classes] = {};
        for (qsizetype i = m_ordered; i < m_hits.size(); ++i)
            ++inClass[classOf(m_hits[i].score)];
        int last = 0;
        for (qsizetype reached = inClass[0]; reached < need; reached += inClass[++last]) {}
        const auto rest = m_hits.begin() + m_ordered;
        const auto reach = std::partition(rest, m_hits.end(), [&](const Hit &hit) { return classOf(hit.score) <= last; });
        QByteArray folded;
        for (auto it = rest; it != reach; ++it)
            if (it->score == SearchMatcher::Pending)
                it->score = m_matcher.align(text(m_rows[it->row].name, folded));
        const auto order = [this](const Hit &a, const Hit &b) { return before(a, b); };
        if (rest + need < reach)
            std::nth_element(rest, rest + need, reach, order);
        std::sort(rest, rest + need, order);
        m_ordered = end;
    }
    QVector<quint32> out;
    for (qsizetype i = from; i < end; ++i)
        out.push_back(m_hits[i].row);
    return out;
}

QVector<FolderModel::Entry> SearchIndex::find(const QString &query, const QString &sort, int limit,
                                              const std::function<bool()> &stop, const QLocale &locale) {
    answer(query);
    QVector<FolderModel::Entry> found = entries(rows(0, limit), stop);
    FolderModel::sortEntries(found, sort, locale);
    return found;
}
