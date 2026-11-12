#include "rawembed.h"

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QVector>
#include <QtEndian>
#include <algorithm>
#include <optional>

namespace {

// Header and directory bytes read per file in total, and the reads they take: each one is its own system call, and a
// round trip on a network mount, so many tiny reads cost far more than their bytes.
constexpr quint64 kHeaderBudget = 1048576;
constexpr int kMaxReads = 4096;
// Frame-header bytes read per file across all candidates; then validation stops.
constexpr qint64 kSofBudget = 1048576;
// One candidate's frame-header walk never reads past this.
constexpr qint64 kJpegPrefix = 262144;
// Frame-header walks read through a window this big, so a byte costs no system call.
constexpr qint64 kWindow = 8192;
// More candidates than this are never validated; a real file holds a handful.
constexpr int kMaxCandidates = 16;
// More directories than this, or deeper nesting, is a corrupt chain; the limit also bounds the recursion.
constexpr int kMaxIfds = 64;
constexpr int kMaxDepth = 4;
constexpr quint16 kMaxEntries = 1024;
constexpr quint64 kEntryLength = 12;
// A value longer than this is image data or a corrupt count.
constexpr quint64 kMaxValue = 65536;
// SubIFD fan-out is capped, so one count cannot queue unbounded work.
constexpr quint32 kMaxPointers = 64;
// No camera's preview holds more pixels than this; a frame header that claims more would cost a vast decode for nothing.
constexpr quint64 kMaxPixels = 250000000;
// The largest embedded JPEG read into memory; a real one is a few MiB.
constexpr quint64 kMaxJpeg = 64 * 1048576;
// SOI plus one frame header: anything shorter is no picture.
constexpr qint64 kMinJpeg = 20;
constexpr quint16 kTiffMagic = 42;
// A TIFF header is 8 bytes; a value of 4 bytes or fewer rides inline in its entry.
constexpr quint64 kTiffHeaderLength = 8, kInlineBytes = 4;

// TIFF and EXIF tags, JPEG compression values, field types and JPEG markers.
constexpr quint16 kCompression = 0x0103, kStripOffsets = 0x0111, kOrientation = 0x0112, kStripByteCounts = 0x0117;
constexpr quint16 kSubIfds = 0x014A, kJpegOffset = 0x0201, kJpegLength = 0x0202;
// EXIF Orientation 1 is upright; 2 to 8 flip or turn the picture.
constexpr quint64 kFirstTurn = 2, kLastTurn = 8;
constexpr quint64 kOldJpeg = 6, kJpeg = 7;
constexpr quint16 kShort = 3, kLong = 4, kIfdType = 13;
constexpr quint8 kMarker = 0xFF, kSoi = 0xD8, kStuffed = 0x00, kTem = 0x01, kRst0 = 0xD0;
constexpr quint8 kSos = 0xDA, kEoi = 0xD9, kApp1 = 0xE1;
// A frame header opens with precision(1) height(2) width(2); SOF0 to SOF2 are the Huffman frames a preview uses.
constexpr quint8 kFirstFrame = 0xC0, kLastFrame = 0xCF, kLastHuffmanFrame = 0xC2, kDht = 0xC4, kJpg = 0xC8, kDac = 0xCC;
constexpr qsizetype kFrameHeadLength = 5;

// Marker, length 34, "Exif\0\0", "II" 42 and IFD0 at 8, one entry: Orientation, SHORT, count 1, no next IFD.
constexpr unsigned char kExifOrientation[36] = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2A, 0,
                                                8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr int kExifOrientationAt = 28;

quint16 rd16(const char *p, bool le) { return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p); }
quint32 rd32(const char *p, bool le) { return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p); }

// Bytes of one value of each TIFF type 0 to 13 (TIFF 6.0 section 2, 13 is an IFD offset); 0 is unknown.
quint64 typeSize(quint16 type) {
    static const quint8 sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
    return type < sizeof sizes ? sizes[type] : 0;
}

bool isLong(quint16 type) { return type == kLong || type == kIfdType; }

// A JPEG frame header; DHT, JPG and DAC share the marker range but are not frames.
bool isFrame(quint8 m) { return m >= kFirstFrame && m <= kLastFrame && m != kDht && m != kJpg && m != kDac; }

// The file through the header budget, or a slice in hand that is read at most its own length.
struct Source {
    QFile *file;
    QByteArray slice;
    quint64 size;
    quint64 left;
    int reads = kMaxReads;

    bool get(quint64 offset, quint64 length, QByteArray &out) {
        if (reads == 0 || length > left || offset > size || length > size - offset)
            return false;
        --reads;
        left -= length;
        if (!file)
            out = slice.mid(qsizetype(offset), qsizetype(length));
        else
            out = file->seek(qint64(offset)) ? file->read(qint64(length)) : QByteArray();
        return quint64(out.size()) == length;
    }
};

// 49 49 2A 00 and the first IFD's offset, or 4D 4D 00 2A big-endian; BigTIFF (43) is refused, not misread.
bool tiffHeader(Source &src, quint64 at, bool &le, quint64 &ifd) {
    QByteArray b;
    if (!src.get(at, kTiffHeaderLength, b) || (!b.startsWith("II") && !b.startsWith("MM")))
        return false;
    le = b.startsWith("II");
    ifd = rd32(b.constData() + 4, le);
    return rd16(b.constData() + 2, le) == kTiffMagic;
}

// A JPEG a directory names, with the turn that directory gives its picture, if any.
struct Candidate {
    quint64 offset = 0, length = 0;
    std::optional<quint64> turn;
};

struct Walker {
    explicit Walker(Source &source) : src(source) {}
    Source &src;
    QVector<Candidate> candidates;
    // Each directory is read once, so a cycle neither loops nor uses up the limit.
    QVector<quint64> seen;
    std::optional<quint64> orientation;

    // A value's bytes: the entry's last four when they fit, else that many bytes at the offset those four name.
    bool value(bool le, quint16 type, quint32 count, const char *val, QByteArray &out) {
        const quint64 total = typeSize(type) * count;
        if (typeSize(type) == 0 || total > kMaxValue)
            return false;
        if (total <= kInlineBytes)
            out = QByteArray(val, int(total));
        return total <= kInlineBytes || src.get(rd32(val, le), total, out);
    }

    // The first value of a SHORT or LONG field.
    std::optional<quint64> one(bool le, quint16 type, quint32 count, const char *val) {
        QByteArray b;
        if (count == 0 || (type != kShort && !isLong(type)) || !value(le, type, count, val, b))
            return {};
        return type == kShort ? rd16(b.constData(), le) : rd32(b.constData(), le);
    }

    // An IFD is count(2), then 12-byte entries tag(2) type(2) count(4) value-or-offset(4), then the next IFD(4).
    bool directory(bool le, quint64 at, quint16 &count, QByteArray &entries) {
        QByteArray b;
        if (!src.get(at, 2, b))
            return false;
        count = rd16(b.constData(), le);
        // A 0xFFFF count is the hostile case: this directory is skipped, never read or looped.
        return count <= kMaxEntries && src.get(at + 2, count * kEntryLength, entries);
    }

    // The Orientation one directory names itself, its next and child directories left unread.
    std::optional<quint64> turnAt(bool le, quint64 at) {
        quint16 count = 0;
        QByteArray entries;
        if (!directory(le, at, count, entries))
            return {};
        for (int i = 0; i < count; ++i) {
            const char *e = entries.constData() + i * kEntryLength;
            if (rd16(e, le) == kOrientation)
                return one(le, rd16(e + 2, le), rd32(e + 4, le), e + 8);
        }
        return {};
    }

    void walk(bool le, int depth, quint64 at) {
        QByteArray b, entries;
        quint16 count = 0;
        if (depth > kMaxDepth || seen.size() >= kMaxIfds || seen.contains(at) || !directory(le, at, count, entries))
            return;
        seen.append(at);
        std::optional<quint64> jpegOffset, jpegLength, stripOffset, stripLength, compression, turn;
        QVector<quint64> children;
        for (int i = 0; i < count; ++i) {
            const char *e = entries.constData() + i * kEntryLength;
            const quint16 tag = rd16(e, le), type = rd16(e + 2, le);
            const quint32 n = rd32(e + 4, le);
            if (tag == kOrientation) turn = one(le, type, n, e + 8);
            else if (tag == kJpegOffset) jpegOffset = one(le, type, n, e + 8);
            else if (tag == kJpegLength) jpegLength = one(le, type, n, e + 8);
            // Many strips are raw sensor data, never one preview.
            else if (tag == kStripOffsets) stripOffset = n == 1 ? one(le, type, n, e + 8) : std::nullopt;
            else if (tag == kStripByteCounts) stripLength = one(le, type, n, e + 8);
            else if (tag == kCompression) compression = one(le, type, n, e + 8);
            else if (tag == kSubIfds && n > 0 && (type == kShort || isLong(type))
                     && value(le, type, qMin(n, kMaxPointers), e + 8, b))
                for (quint32 k = 0; k < qMin(n, kMaxPointers); ++k)
                    children.append(type == kShort ? rd16(b.constData() + k * 2, le) : rd32(b.constData() + k * 4, le));
        }
        // The first directory to name a turn speaks for the file, as IFD0 does in a camera's own.
        if (!orientation) orientation = turn;
        if (jpegOffset && jpegLength && *jpegLength > 0)
            candidates.append({*jpegOffset, *jpegLength, turn});
        // Any lone JPEG-compressed strip is a preview (a CR2 keeps its big one in IFD0); the frame rule keeps raw data out.
        if (stripOffset && stripLength && compression && (*compression == kOldJpeg || *compression == kJpeg)
            && *stripLength > 0)
            candidates.append({*stripOffset, *stripLength, turn});
        for (const quint64 child : children)
            walk(le, depth + 1, child);
        // A next pointer that cannot be read ends the chain and keeps what it collected.
        if (src.get(at + 2 + count * kEntryLength, 4, b) && rd32(b.constData(), le) != 0)
            walk(le, depth, rd32(b.constData(), le));
    }
};

// The first frame header's marker and size (FF C0 00 11 08 height width), reading at most `left` file bytes through a window.
bool firstFrame(QFile &file, qint64 &left, quint8 &marker, quint32 &width, quint32 &height) {
    QByteArray window, b;
    qsizetype used = 0;
    const auto take = [&](qsizetype n) {
        b.clear();
        while (b.size() < n) {
            if (used == window.size()) {
                window = left > 0 ? file.read(qMin(kWindow, left)) : QByteArray();
                left -= window.size();
                used = 0;
                if (window.isEmpty())
                    return false;
            }
            const qsizetype part = qMin(n - b.size(), window.size() - used);
            b += window.mid(used, part);
            used += part;
        }
        return true;
    };
    if (!take(2) || quint8(b[0]) != kMarker || quint8(b[1]) != kSoi)
        return false;
    for (;;) {
        do { if (!take(1)) return false; } while (quint8(b[0]) != kMarker);
        do { if (!take(1)) return false; } while (quint8(b[0]) == kMarker);
        const quint8 m = quint8(b[0]);
        if (m == kStuffed || m == kSos || m == kEoi)
            return false;
        // TEM, the restart markers and SOI carry no length.
        if (m == kTem || (m >= kRst0 && m <= kSoi))
            continue;
        if (!take(2) || qFromBigEndian<quint16>(b.constData()) < 2)
            return false;
        // A segment is skipped by reading past it, so the window still bounds the total.
        if (!isFrame(m)) {
            if (!take(qFromBigEndian<quint16>(b.constData()) - 2))
                return false;
            continue;
        }
        if (!take(kFrameHeadLength))
            return false;
        marker = m;
        height = qFromBigEndian<quint16>(b.constData() + 1);
        width = qFromBigEndian<quint16>(b.constData() + 3);
        return true;
    }
}

// The orientation the JPEG's own first Exif segment (FF E1, length, "Exif\0\0", a TIFF) names, if any.
std::optional<quint64> ownOrientation(const QByteArray &jpeg) {
    for (qsizetype at = 2; at + 4 <= jpeg.size() && quint8(jpeg[at]) == kMarker;) {
        const quint8 marker = quint8(jpeg[at + 1]);
        // Any number of 0xFF fill bytes may stand ahead of a marker.
        if (marker == kMarker) {
            ++at;
            continue;
        }
        const qsizetype length = qFromBigEndian<quint16>(jpeg.constData() + at + 2);
        if (marker == kSos || marker == kEoi || isFrame(marker) || length < 2 || at + 2 + length > jpeg.size())
            return {};
        if (marker == kApp1 && length >= 8 && jpeg.mid(at + 4, 6) == QByteArray("Exif\0\0", 6)) {
            const QByteArray tiff = jpeg.mid(at + 10, length - 8);
            Source slice{nullptr, tiff, quint64(tiff.size()), quint64(tiff.size())};
            bool le = true;
            quint64 ifd = 0;
            // Only IFD0 describes the picture; the IFD1 it leads to is the Exif thumbnail's.
            return tiffHeader(slice, 0, le, ifd) ? Walker(slice).turnAt(le, ifd) : std::nullopt;
        }
        at += 2 + length;
    }
    return {};
}

struct Preview {
    quint64 offset = 0, length = 0, pixels = 0;
    std::optional<quint64> turn;
};

// Most pixels first, the longer of two equals ahead; only Huffman frames (SOF0 to SOF2) count, SOF3 is raw data.
QVector<Preview> ranked(QFile &file, const QVector<Candidate> &candidates) {
    qint64 sofLeft = kSofBudget;
    QVector<Preview> previews;
    for (int i = 0; i < qMin(int(candidates.size()), kMaxCandidates); ++i) {
        const quint64 offset = candidates[i].offset, length = candidates[i].length;
        // A span running past the end of the file is a cut file, not a preview.
        if (length < quint64(kMinJpeg) || sofLeft == 0 || offset > quint64(file.size())
            || length > quint64(file.size()) - offset)
            continue;
        const qint64 span = qMin(qint64(qMin(length, quint64(kJpegPrefix))), sofLeft);
        qint64 left = span;
        quint8 marker = 0;
        quint32 width = 0, height = 0;
        const bool found = span >= kMinJpeg && file.seek(qint64(offset)) && firstFrame(file, left, marker, width, height);
        sofLeft -= span - left;
        if (found && marker <= kLastHuffmanFrame && quint64(width) * height <= kMaxPixels)
            previews.append({offset, length, quint64(width) * height, candidates[i].turn});
    }
    std::stable_sort(previews.begin(), previews.end(), [](const Preview &a, const Preview &b) {
        return a.pixels != b.pixels ? a.pixels > b.pixels : a.length > b.length;
    });
    return previews;
}

// One preview read and scaled to fit `pixels`, turned by `turn` when it names no orientation of its own.
QImage decoded(QFile &file, const Preview &preview, quint64 turn, int pixels) {
    if (!file.seek(qint64(preview.offset)))
        return {};
    // Room for the Exif segment is kept, so putting it in moves the bytes in place instead of copying them all.
    QByteArray jpeg;
    jpeg.reserve(qsizetype(preview.length + sizeof kExifOrientation));
    jpeg.resize(qsizetype(preview.length));
    if (file.read(jpeg.data(), jpeg.size()) != jpeg.size())
        return {};
    if (turn >= kFirstTurn && turn <= kLastTurn && !ownOrientation(jpeg.left(kJpegPrefix))) {
        QByteArray segment(reinterpret_cast<const char *>(kExifOrientation), sizeof kExifOrientation);
        qToLittleEndian<quint16>(quint16(turn), segment.data() + kExifOrientationAt);
        jpeg.insert(2, segment);
    }
    QBuffer buffer(&jpeg);
    QImageReader reader(&buffer, "jpeg");
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.width() > pixels || full.height() > pixels)
        reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
    return reader.read();
}

} // namespace

bool RawEmbed::handles(const QString &mime) {
    // shared-mime-info's names for .cr2 .nef .nrw .arw .sr2 .dng .pef.
    static const QStringList types = {"image/x-canon-cr2", "image/x-nikon-nef", "image/x-nikon-nrw", "image/x-sony-arw",
                                      "image/x-sony-sr2", "image/x-adobe-dng", "image/x-pentax-pef"};
    return types.contains(mime);
}

QImage RawEmbed::thumbnail(const QString &path, int pixels) {
    // Unbuffered, so a small directory read costs its own bytes and no read-ahead.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return {};
    Source src{&file, {}, quint64(file.size()), kHeaderBudget};
    Walker walker(src);
    bool le = true;
    quint64 ifd = 0;
    if (!tiffHeader(src, 0, le, ifd))
        return {};
    walker.walk(le, 0, ifd);

    // A preview that doesn't decode is passed over for the next largest that fits what is left of one limit of bytes.
    quint64 left = kMaxJpeg;
    for (const Preview &preview : ranked(file, walker.candidates)) {
        if (preview.length > left)
            continue;
        left -= preview.length;
        // A directory's own turn rules its picture; one that names none takes the file's.
        const QImage image = decoded(file, preview, preview.turn.value_or(walker.orientation.value_or(0)), pixels);
        if (!image.isNull())
            return image;
    }
    return {};
}
