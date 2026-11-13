#include "searchindex.h"

#include <QBitArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QMutex>
#include <QScopeGuard>
#include <QVarLengthArray>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

static bool isLetter(char c) {
    c = lower(c);
    return c >= 'a' && c <= 'z';
}

static bool isAlnum(char c) {
    return isLetter(c) || (c >= '0' && c <= '9');
}

static bool isSeparator(char c) {
    return c == '/' || c == '-' || c == '_' || c == '.' || c == ' ';
}

// A word starts after a separator, and at a capital after a small letter or a digit ("renderFrame", "2024Report").
static bool wordStart(QByteArrayView text, qsizetype at) {
    return at == 0 || isSeparator(text[at - 1]) || (lower(text[at]) != text[at] && lower(text[at - 1]) == text[at - 1]);
}

// A name that isn't ASCII, as the matcher reads it: what's past ASCII folded by Qt, ASCII left in its case, so a capital still starts a word.
static QByteArray foldedPastAscii(QByteArrayView name) {
    const QString text = QString::fromUtf8(name);
    QString out;
    qsizetype from = 0;
    for (qsizetype at = 0; at <= text.size(); ++at) {
        if (at < text.size() && text[at].unicode() >= 0x80)
            continue;
        out += text.sliced(from, at - from).toCaseFolded();
        if (at < text.size())
            out += text[at];
        from = at + 1;
    }
    return out.toUtf8();
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
            bits |= 1ull << (DigitBit + c - '0');
        else if (mark)
            bits |= 1ull << (MarkBit + (mark - marks));
        else
            bits |= Unknown;
    }
    return bits;
}

SearchMatcher::SearchMatcher(const QString &query)
    : m_needle(query.toCaseFolded().toUtf8()), m_bits(bitsOf(m_needle)),
      m_folderPart(m_needle.lastIndexOf('/') + 1), m_bitsFrom(m_needle.size() + 1, 0) {
    for (qsizetype at = m_needle.size(); at-- > 0;)
        m_bitsFrom[at] = m_bitsFrom[at + 1] | bitsOf(m_needle.sliced(at, 1));
    // One typo from this many letters and digits, two from twice as many.
    constexpr int oneTypoFrom = 4;
    const qsizetype letters = std::count_if(m_needle.begin(), m_needle.end(), isAlnum);
    if (!m_needle.isEmpty() && isAlnum(m_needle[0]))
        m_typos = letters >= 2 * oneTypoFrom ? 2 : letters >= oneTypoFrom ? 1 : 0;
}

// Optimal string alignment from each word start, so a swapped pair is one typo: only letters are added, dropped or changed, never digits or marks.
int SearchMatcher::typo(QByteArrayView hay) const {
    // Each typo costs this much of the class's score.
    constexpr int typoCost = 1000;
    const int over = m_typos + 1;
    const int length = int(m_needle.size());
    int fewest = over;
    for (int start = 0; start < int(hay.size()); ++start) {
        if (!wordStart(hay, start) || int(hay.size()) - start + m_typos < length)
            continue;
        const int span = std::min(int(hay.size()) - start, length + m_typos);
        m_older.assign(span + 1, over);
        m_previous.assign(span + 1, over);
        m_current.assign(span + 1, over);
        m_previous[0] = 0;
        for (int j = 1; j <= std::min(span, m_typos); ++j)
            m_previous[j] = isLetter(hay[start + j - 1]) ? m_previous[j - 1] + 1 : over;
        for (int i = 1; i <= length; ++i) {
            std::fill(m_current.begin(), m_current.end(), over);
            const char a = m_needle[i - 1];
            m_current[0] = isLetter(a) ? m_previous[0] + 1 : over;
            int low = m_current[0];
            for (int j = std::max(1, i - m_typos); j <= std::min(span, i + m_typos); ++j) {
                const char b = lower(hay[start + j - 1]);
                int value = a == b ? m_previous[j - 1] : isLetter(a) && isLetter(b) ? m_previous[j - 1] + 1 : over;
                if (isLetter(a))
                    value = std::min(value, m_previous[j] + 1);
                if (isLetter(b))
                    value = std::min(value, m_current[j - 1] + 1);
                if (i > 1 && j > 1 && a == lower(hay[start + j - 2]) && m_needle[i - 2] == b && isLetter(a) && isLetter(b))
                    value = std::min(value, m_older[j - 2] + 1);
                m_current[j] = std::min(value, over);
                low = std::min(low, value);
            }
            m_older.swap(m_previous);
            m_previous.swap(m_current);
            if (low > m_typos)
                break;
            if (i == length)
                for (int j = std::max(0, length - m_typos); j <= span; ++j)
                    fewest = std::min(fewest, m_previous[j]);
        }
    }
    return fewest <= m_typos ? Typo - fewest * typoCost : NoMatch;
}

qsizetype SearchMatcher::reach(QByteArrayView text, qsizetype at) const {
    for (char c : text)
        if (at < m_needle.size() && m_needle[at] == lower(c))
            ++at;
    return at;
}

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

bool SearchMatcher::spells(QByteArrayView text) const {
    for (qsizetype at = 0; at + m_needle.size() <= text.size(); ++at)
        if (startsAt(text, at))
            return true;
    // reached[k]: the query's first k letters are placed, each run from the start of a word.
    QVarLengthArray<bool, 64> reached(m_needle.size() + 1);
    std::fill(reached.begin(), reached.end(), false);
    reached[0] = true;
    for (qsizetype at = 0; at < text.size(); ++at) {
        if (!wordStart(text, at))
            continue;
        // From the far end, so a run this word starts isn't taken up again within the word.
        for (qsizetype k = m_needle.size(); k-- > 0;) {
            if (!reached[k])
                continue;
            for (qsizetype run = 0; k + run < m_needle.size() && at + run < text.size()
                                    && lower(text[at + run]) == m_needle[k + run]; ++run)
                reached[k + run + 1] = true;
        }
    }
    return reached[m_needle.size()];
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

// best[k]: the best score with the query's first k letters placed so far; end[k]: the same with the k-th placed on the letter just read.
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

bool SearchIndex::walk(const QString &root, bool showHidden, const std::function<bool()> &stop, QMutex *lock) {
    {
        // An answer on another thread reads the root under the lock.
        QMutexLocker hold(lock);
        ++counts.walks;
        m_root = root;
    }
    // Every folder is opened from the root's own descriptor: the tree read stays one tree, whatever its path leads to later.
    const int top = ::open(QFile::encodeName(root).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    const auto closed = qScopeGuard([top] {
        if (top >= 0)
            ::close(top);
    });
    dev_t drive = 0;
    // How many names are taken, and whether more were left at mostRows.
    qsizetype taken = 0;
    bool full = false;
    // Gone partway: nothing is kept.
    const auto stopped = [&] {
        QMutexLocker hold(lock);
        const Counts spent = counts;
        *this = SearchIndex();
        counts = spent;
        return false;
    };
    // A folder's names go in all at once, so an answer sees whole folders; while an answer holds the lock, the folders found are read on and wait here, in order.
    struct Read {
        quint32 parent;
        QByteArray names;
        QVector<qsizetype> ends;
        QVector<bool> folders;
        qsizetype asked = 0;
    };
    std::vector<Read> waiting;
    const auto add = [&] {
        for (const Read &read : waiting) {
            ++counts.opens;
            counts.stats += 1 + read.asked;
            for (qsizetype i = 0, from = 0; i < read.ends.size(); from = read.ends[i++]) {
                const quint32 id = intern(QByteArrayView(read.names).sliced(from, read.ends[i] - from));
                if (m_rows.size() % 8 == 0)
                    m_kinds.append('\0');
                if (read.folders[i]) {
                    m_kinds.back() = char(m_kinds.back() | (1 << (m_rows.size() % 8)));
                    const quint32 above = read.parent == None ? 0 : m_folders[read.parent].length + 1;
                    // A "/" after each folder, as in the path.
                    const quint64 bits = m_names[id].bits | SearchMatcher::bitsOf("/")
                        | (read.parent == None ? 0 : m_folders[read.parent].bits);
                    m_folders.push_back({id, read.parent, above + m_names[id].length, bits});
                }
                m_rows.push_back({id, read.parent});
            }
        }
        waiting.clear();
    };
    // The folders found are the queue: each is read in the order it was found, which is breadth first. -1 is the root.
    for (qsizetype folder = -1;; ++folder) {
        // Out of folders to read, those waiting go in, and perhaps bring more.
        if (folder >= m_folders.size() && !waiting.empty()) {
            QMutexLocker hold(lock);
            add();
        }
        if (folder >= m_folders.size())
            break;
        if (stop && stop())
            return stopped();
        const quint32 parent = folder < 0 ? None : quint32(folder);
        const QByteArray at = folder < 0 ? QByteArray(".") : joined(m_folders[folder].parent, name(m_folders[folder].name));
        // No link is followed below the root: not a folder that's become one since it was listed, nor one on the way to it.
        open_how how = {};
        how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
        how.resolve = RESOLVE_NO_SYMLINKS;
        const int fd = int(::syscall(SYS_openat2, top, at.constData(), &how, sizeof how));
        if (fd < 0)
            continue;
        struct stat st;
        // Other filesystems (a mounted drive, another machine, /proc from /) are listed but not read.
        const bool same = ::fstat(fd, &st) == 0 && (folder < 0 || st.st_dev == drive);
        if (folder < 0)
            drive = st.st_dev;
        DIR *dir = same ? ::fdopendir(fd) : nullptr;
        if (!dir)
            ::close(fd);
        Read read{parent, {}, {}, {}};
        while (const dirent *entry = dir ? ::readdir(dir) : nullptr) {
            if (stop && stop()) {
                ::closedir(dir);
                return stopped();
            }
            const char *text = entry->d_name;
            if (text[0] == '.' && (!showHidden || text[1] == 0 || (text[1] == '.' && text[2] == 0)))
                continue;
            // At the most it keeps, the walk ends with the nearest names.
            if (taken == mostRows) {
                full = true;
                break;
            }
            ++taken;
            // The type comes with the name; only a filesystem that doesn't say needs asking. Symlinked folders aren't walked into.
            bool isDir = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {
                ++read.asked;
                struct stat child;
                isDir = ::fstatat(::dirfd(dir), text, &child, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(child.st_mode);
            }
            read.names.append(text);
            read.ends.push_back(read.names.size());
            read.folders.push_back(isDir);
        }
        if (dir)
            ::closedir(dir);
        waiting.push_back(std::move(read));
        if (full)
            break;
        if (!lock) {
            add();
        } else if (lock->tryLock()) {
            add();
            lock->unlock();
        }
        if (afterFolder)
            afterFolder(folder + 2);
    }
    QMutexLocker hold(lock);
    add();
    m_cut = full;
    // Names are only looked up while folders are read, and what's left over from growing goes back.
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
        + m_hits.capacity() * sizeof(Hit) + m_stamps.capacity() * sizeof(quint32) + m_scores.capacity() * sizeof(int)
        + m_folderStamps.capacity() * sizeof(quint32) + m_reaches.capacity() * sizeof(quint32);
}

quint32 SearchIndex::intern(QByteArrayView text) {
    // Kept under three quarters full, so a probe ends soon.
    if ((m_names.size() + 1) * 4 > m_lookup.size() * 3) {
        // A power of two, as every size after it, so a hash is cut to the table with a mask.
        constexpr qsizetype firstLookup = 1024;
        QVector<quint32> bigger(qMax(firstLookup, m_lookup.size() * 2), None);
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
                               : SearchMatcher::bitsOf(foldedPastAscii(text)) | SearchMatcher::NotAscii;
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

QStringList SearchIndex::names(const QVector<quint32> &rows) const {
    QStringList out;
    out.reserve(rows.size());
    for (quint32 row : rows)
        out << QFile::decodeName(path(row));
    return out;
}

QStringList SearchIndex::paths(const QString &root, const QStringList &names) {
    QStringList out;
    out.reserve(names.size());
    const QDir dir(root);
    for (const QString &name : names)
        out << QFileInfo(dir.filePath(name)).absoluteFilePath();
    return out;
}

QVector<FolderModel::Entry> SearchIndex::entriesAt(const QString &root, const QStringList &names, const std::function<bool()> &stop) {
    QVector<FolderModel::Entry> out;
    QMimeDatabase mimes;
    const QDir dir(root);
    for (const QString &name : names) {
        if (stop && stop())
            return {};
        FolderModel::Entry e = FolderModel::entryFor(QFileInfo(dir.filePath(name)), mimes);
        e.name = name;
        out.push_back(std::move(e));
    }
    return out;
}

QVector<FolderModel::Entry> SearchIndex::entries(const QVector<quint32> &rows, const std::function<bool()> &stop) {
    QVector<FolderModel::Entry> out = entriesAt(m_root, names(rows), stop);
    counts.stats += out.size();
    return out;
}

QByteArrayView SearchIndex::text(quint32 id, QByteArray &folded) const {
    if (!(m_names[id].bits & SearchMatcher::NotAscii))
        return name(id);
    folded = foldedPastAscii(name(id));
    return folded;
}

qsizetype SearchIndex::pathLength(quint32 row) const {
    const Row &r = m_rows[row];
    return (r.parent == None ? 0 : m_folders[r.parent].length + 1) + m_names[r.name].length;
}

// Whether `held` can hold every letter in `wanted`, `edits` of them aside: one more it lacks rules it out unread. Digits and marks are never edited.
static bool possible(quint64 wanted, quint64 held, int edits = 0) {
    constexpr quint64 letters = (1ull << SearchMatcher::DigitBit) - 1;
    if ((wanted | held) & SearchMatcher::Unknown)
        return true;
    const quint64 missing = wanted & ~held;
    return !(missing & ~letters) && int(qPopulationCount(missing)) <= edits;
}

int SearchIndex::score(quint32 id, QByteArray &folded) const {
    if (!possible(m_matcher.bits(), m_names[id].bits))
        return SearchMatcher::NoMatch;
    return m_matcher.name(text(id, folded));
}

// How far into the query the folders down to `folder` take it: worked out once a folder a query, so a row costs only its own name.
qsizetype SearchIndex::reach(quint32 folder) {
    QVarLengthArray<quint32, 32> unknown;
    for (quint32 at = folder; at != None && m_folderStamps[at] != m_stamp; at = m_folders[at].parent)
        unknown.push_back(at);
    QByteArray folded;
    while (!unknown.isEmpty()) {
        const quint32 at = unknown.back();
        unknown.pop_back();
        const quint32 parent = m_folders[at].parent;
        qsizetype got = m_matcher.reach(text(m_folders[at].name, folded), parent == None ? 0 : m_reaches[parent]);
        // The query's "/" is spent on the one after the folder.
        if (got < m_matcher.needle().size() && m_matcher.needle()[got] == '/')
            ++got;
        m_folderStamps[at] = m_stamp;
        m_reaches[at] = quint32(got);
        ++counts.folders;
    }
    return m_reaches[folder];
}

// A match along the path's score: the folders' part worked out once a folder, then the row's own name.
int SearchIndex::alongPath(quint32 row, int tier, QHash<quint32, SearchMatcher::Alignment> &above) {
    const QByteArray &needle = m_matcher.needle();
    QVarLengthArray<quint32, 32> unknown;
    quint32 at = m_rows[row].parent;
    for (; at != None && !above.contains(at); at = m_folders[at].parent)
        unknown.push_back(at);
    QByteArray folded;
    while (!unknown.isEmpty()) {
        const quint32 folder = unknown.back();
        unknown.pop_back();
        SearchMatcher::Alignment alignment;
        if (m_folders[folder].parent == None)
            alignment.reset(needle.size());
        else
            alignment = above.value(m_folders[folder].parent);
        alignment.advance(needle, text(m_folders[folder].name, folded).toByteArray() + '/', false);
        above.insert(folder, alignment);
    }
    SearchMatcher::Alignment alignment = above.value(m_rows[row].parent);
    alignment.advance(needle, text(m_rows[row].name, folded), true);
    return alignment.score(tier);
}

qsizetype SearchIndex::answer(const QString &query) {
    ++counts.answers;
    // A longer query's matches are among the last one's, as long as nothing's been read since.
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
    restamp();
    m_stamps.resize(m_names.size());
    m_scores.resize(m_names.size());
    m_folderStamps.resize(m_folders.size());
    m_reaches.resize(m_folders.size());
    const QByteArray &needle = m_matcher.needle();
    const quint64 first = needle.isEmpty() ? 0 : SearchMatcher::bitsOf(needle.first(1));
    const quint64 folderBits = SearchMatcher::bitsOf(needle.first(m_matcher.folderPart()));
    QByteArray folded;
    // Whether a name spells the query out, or its folders were named with a "/": anything else found is its letters scattered about.
    bool whole = false;
    const qsizetype count = narrow ? within.size() : m_rows.size();
    for (qsizetype i = 0; i < count; ++i) {
        const quint32 row = narrow ? within[i] : quint32(i);
        const Row &r = m_rows[row];
        if (m_stamps[r.name] != m_stamp) {
            m_stamps[r.name] = m_stamp;
            m_scores[r.name] = score(r.name, folded);
            ++counts.names;
        }
        int found = m_scores[r.name];
        // Not in the name as typed: then perhaps along the path, the folders taking the query as far as they can and the name the rest; the bits rule most paths out unread.
        if ((found == SearchMatcher::NoMatch || (found < SearchMatcher::Named + SearchMatcher::Span && found != SearchMatcher::Pending))
            && r.parent != None) {
            const quint64 held = m_folders[r.parent].bits | m_names[r.name].bits;
            // A path's length rules it out only in ASCII: folding can make another letter longer than it is stored.
            if ((held & SearchMatcher::NotAscii || needle.size() <= pathLength(row)) && possible(first, held) && possible(folderBits, m_folders[r.parent].bits)
                && possible(m_matcher.bits(), held)) {
                const qsizetype got = reach(r.parent);
                if (got >= m_matcher.folderPart() && possible(m_matcher.bitsFrom(got), m_names[r.name].bits)
                    && m_matcher.reach(text(r.name, folded), got) == needle.size())
                    found = m_matcher.folderPart() > 0 ? SearchMatcher::PendingAligned : SearchMatcher::PendingPath;
            }
        }
        if (found == SearchMatcher::NoMatch)
            continue;
        m_hits.push_back({found, row});
        whole = whole || found >= SearchMatcher::Word || found == SearchMatcher::PendingAligned
            || (found == SearchMatcher::Pending && m_matcher.spells(text(r.name, folded)));
    }
    // No name spells it out: then names a typo or two off, ahead of the scattered ones. The slowest way to match, so it's only tried now.
    if (!whole && m_matcher.typos() > 0) {
        ++counts.typos;
        restamp();
        const auto off = [&](quint32 id) {
            if (m_stamps[id] != m_stamp) {
                m_stamps[id] = m_stamp;
                m_scores[id] = possible(m_matcher.bits(), m_names[id].bits, m_matcher.typos())
                    ? m_matcher.typo(text(id, folded)) : SearchMatcher::NoMatch;
            }
            return m_scores[id];
        };
        // A scattered match that is also a typo off counts as the typo.
        QBitArray listed(m_rows.size());
        for (Hit &hit : m_hits) {
            listed.setBit(hit.row);
            if (const int score = off(m_rows[hit.row].name); score != SearchMatcher::NoMatch)
                hit.score = score;
        }
        for (quint32 row = 0; row < quint32(m_rows.size()); ++row)
            if (!listed.testBit(row))
                if (const int score = off(m_rows[row].name); score != SearchMatcher::NoMatch)
                    m_hits.push_back({score, row});
    }
    return m_hits.size();
}

void SearchIndex::restamp() {
    if (++m_stamp == 0) {
        m_stamps.fill(0);
        m_folderStamps.fill(0);
        m_stamp = 1;
    }
}

// The classes, best first; a pending score is in its own.
static int classOf(int score) {
    static constexpr int tiers[] = {SearchMatcher::Exact, SearchMatcher::Prefix, SearchMatcher::Word, SearchMatcher::Aligned,
                                    SearchMatcher::Named};
    // Where a pending score's class is among them.
    constexpr int aligned = 3, named = 4;
    static_assert(tiers[aligned] == SearchMatcher::Aligned && tiers[named] == SearchMatcher::Named);
    if (score == SearchMatcher::PendingAligned)
        return aligned;
    if (score == SearchMatcher::Pending || score == SearchMatcher::PendingPath)
        return named;
    for (int i = 0; i < int(std::size(tiers)); ++i)
        if (score >= tiers[i] - SearchMatcher::Span)
            return i;
    return int(std::size(tiers));
}
static constexpr int Classes = 6;

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
        QHash<quint32, SearchMatcher::Alignment> above;
        for (auto it = rest; it != reach; ++it) {
            if (it->score == SearchMatcher::Pending)
                it->score = m_matcher.align(text(m_rows[it->row].name, folded));
            else if (it->score == SearchMatcher::PendingAligned || it->score == SearchMatcher::PendingPath)
                it->score = alongPath(it->row, it->score == SearchMatcher::PendingAligned ? SearchMatcher::Aligned : SearchMatcher::Named, above);
        }
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
