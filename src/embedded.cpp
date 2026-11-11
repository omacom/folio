#include "embedded.h"
#include "rawembed.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <optional>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace {

// The end record is 22 bytes and a comment of at most 65535 follows it, so the record lies in the file's last 65557.
constexpr qint64 kEndRecord = 22, kLongestComment = 0xFFFF;
// More directory entries or bytes than this is no container a person made; a big comic book holds a few thousand pages.
constexpr quint32 kMostEntries = 8192, kMostDirectoryBytes = 8 * 1048576;
// A member stored or inflated past this is no thumbnail; inflation stops here whatever the header claims.
constexpr quint32 kMostMemberBytes = 64 * 1048576;
constexpr quint32 kEndSignature = 0x06054B50, kEntrySignature = 0x02014B50, kLocalSignature = 0x04034B50;
// A zip64 end locator stands 20 bytes ahead of the end record, and all-ones fields send a reader to zip64's own.
constexpr quint32 kZip64Locator = 0x07064B50, kZip64Word = 0xFFFFFFFF;
constexpr qint64 kLocatorLength = 20;
constexpr quint16 kZip64Half = 0xFFFF;
constexpr qint64 kEntryLength = 46, kLocalLength = 30;
// General-purpose bit 0 marks an encrypted member; method 0 stores it, 8 deflates it.
constexpr quint16 kEncrypted = 0x0001, kStored = 0, kDeflated = 8;
// Raw deflate, with no zlib header or trailer, as a zip holds it.
constexpr int kRawDeflate = -MAX_WBITS;

const QString kEpub = QStringLiteral("application/epub+zip"), kComic = QStringLiteral("application/vnd.comicbook+zip");
const QString kOpenDocument = QStringLiteral("application/vnd.oasis.opendocument.");
const QString kOfficeOpenXml = QStringLiteral("application/vnd.openxmlformats-officedocument.");
const QString k3mf = QStringLiteral("model/3mf"), kKrita = QStringLiteral("application/x-krita");
const QString kOpenRaster = QStringLiteral("image/openraster"), kFreeCad = QStringLiteral("application/x-extension-fcstd");
// A comic book's pages are its pictures, by the ending of their names.
const QByteArrayList kPageEndings = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"};

// The fonts drawn from their own bytes: TrueType, OpenType, collections of either, WOFF and Type 1.
const QStringList kFonts = {"font/ttf", "font/otf", "font/collection", "font/woff", "font/woff2", "application/x-font-type1"};
// An sfnt opens with "OTTO" for CFF outlines or 00 01 00 00 for TrueType ones.
constexpr qint64 kSignatureLength = 4;
const QByteArray kCffSignature("OTTO"), kTrueTypeSignature("\0\1\0\0", kSignatureLength);
const QString kFormulaTemplate = QStringLiteral("application/vnd.oasis.opendocument.formula-template");
// A font file past this is held as no font; the largest collections people install are a few tens of MiB.
constexpr qint64 kMostFontBytes = 64 * 1048576;
// A font's letters fill this share of their square on their longer side.
constexpr qreal kFontFill = 0.8;
// Characters are looked for from "!", the first printable one, to the last code point there is.
constexpr char32_t kFirstCharacter = 0x21, kLastCharacter = 0x10FFFF;

quint16 rd16(const char *p) { return qFromLittleEndian<quint16>(p); }
quint32 rd32(const char *p) { return qFromLittleEndian<quint32>(p); }

// The file at `path` when it is a regular one, opened without waiting: a FIFO or device would hold the worker.
bool openRegular(QFile &file, const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat status = {};
    if (fd >= 0 && ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
        && file.open(fd, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle))
        return true;
    if (fd >= 0)
        ::close(fd);
    return false;
}

// `length` bytes at `offset`, or none when the file holds fewer.
QByteArray at(QFile &file, qint64 offset, qint64 length) {
    if (offset < 0 || length < 0 || offset > file.size() - length || !file.seek(offset))
        return {};
    const QByteArray out = file.read(length);
    return out.size() == length ? out : QByteArray();
}

// Where a member lies, as its directory entry says.
struct Member {
    quint16 flags = 0, method = 0;
    quint32 compressed = 0, size = 0, local = 0;
};

// 50 4B 05 06, disk(2) directory's disk(2) entries on this disk(2) entries(2) directory bytes(4) offset(4) comment length(2).
bool endRecord(QFile &file, quint32 &count, quint32 &bytes, quint32 &offset) {
    const qint64 tail = qMin(file.size(), kEndRecord + kLongestComment);
    const QByteArray end = at(file, file.size() - tail, tail);
    // The last signature whose comment ends with the file; a comment may hold the signature itself.
    qsizetype found = end.size() - kEndRecord;
    for (; found >= 0; --found) {
        const char *e = end.constData() + found;
        if (rd32(e) == kEndSignature && found + kEndRecord + rd16(e + 20) == end.size())
            break;
    }
    if (found < 0)
        return false;
    const char *e = end.constData() + found;
    count = rd16(e + 10);
    bytes = rd32(e + 12);
    offset = rd32(e + 16);
    // A split archive, or one whose real numbers are in zip64 records, is refused rather than misread.
    const bool zip64 = (found >= kLocatorLength && rd32(e - kLocatorLength) == kZip64Locator) || count == kZip64Half
        || bytes == kZip64Word || offset == kZip64Word;
    return !zip64 && rd16(e + 4) == 0 && rd16(e + 6) == 0 && rd16(e + 8) == count && count <= kMostEntries
        && bytes <= kMostDirectoryBytes;
}

// 50 4B 01 02, versions(4) flags(2) method(2) time(4) crc(4) compressed(4) size(4) name(2) extra(2) comment(2) disk(2) attributes(6) local(4) name.
bool directory(QFile &file, QMap<QByteArray, Member> &members) {
    quint32 count = 0, bytes = 0, offset = 0;
    if (!endRecord(file, count, bytes, offset))
        return false;
    const QByteArray entries = at(file, offset, bytes);
    if (entries.size() != qsizetype(bytes))
        return false;
    qsizetype p = 0;
    for (quint32 i = 0; i < count; ++i) {
        const char *e = entries.constData() + p;
        if (p + kEntryLength > entries.size() || rd32(e) != kEntrySignature
            || p + kEntryLength + rd16(e + 28) > entries.size())
            return false;
        // A name is only ever compared with the names a format fixes, never made into a path.
        members.insert(QByteArray(e + kEntryLength, rd16(e + 28)), {rd16(e + 8), rd16(e + 10), rd32(e + 20), rd32(e + 24), rd32(e + 42)});
        p += kEntryLength + rd16(e + 28) + rd16(e + 30) + rd16(e + 32);
    }
    return true;
}

// 50 4B 03 04, version(2) flags(2) method(2) time(4) crc(4) sizes(8) name(2) extra(2), then the name, extra and data.
QByteArray read(QFile &file, const Member &member) {
    // Sizes are refused before anything is allocated; zip64's all-ones fields are past the limit too.
    if ((member.flags & kEncrypted) || member.compressed > kMostMemberBytes || member.size > kMostMemberBytes
        || member.local == kZip64Word || (member.method != kStored && member.method != kDeflated))
        return {};
    const QByteArray head = at(file, member.local, kLocalLength);
    if (head.size() != kLocalLength || rd32(head.constData()) != kLocalSignature)
        return {};
    const qint64 start = qint64(member.local) + kLocalLength + rd16(head.constData() + 26) + rd16(head.constData() + 28);
    const QByteArray data = at(file, start, member.compressed);
    if (data.size() != qsizetype(member.compressed))
        return {};
    if (member.method == kStored)
        return member.size == member.compressed ? data : QByteArray();
    // Inflated into the size the entry claims and no further: a stream that would run past it is refused.
    QByteArray out(qsizetype(member.size), Qt::Uninitialized);
    z_stream stream = {};
    if (inflateInit2(&stream, kRawDeflate) != Z_OK)
        return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    const int status = inflate(&stream, Z_FINISH);
    const bool whole = status == Z_STREAM_END && stream.total_out == member.size;
    inflateEnd(&stream);
    return whole ? out : QByteArray();
}

// The member name a reference makes from the part at `from`: "../images/cover.jpg" from "OEBPS/text/a.opf" is "OEBPS/images/cover.jpg".
QByteArray resolved(const QString &from, const QString &reference) {
    const QString name = QUrl::fromPercentEncoding(reference.toUtf8()), folder = from.section('/', 0, -2);
    const QString path = QDir::cleanPath(name.startsWith('/') || folder.isEmpty() ? name : folder + '/' + name);
    return (path.startsWith('/') ? path.mid(1) : path).toUtf8();
}

// <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
QString packagePath(const QByteArray &container) {
    QXmlStreamReader xml(container);
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == u"rootfile")
            return xml.attributes().value("full-path").toString();
    return {};
}

// <item id="c" href="images/cover.jpg" properties="cover-image"/>, or EPUB 2's <meta name="cover" content="c"/> naming an item.
QString coverReference(const QByteArray &package) {
    QXmlStreamReader xml(package);
    QHash<QString, QString> items;
    QString named;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QXmlStreamAttributes attributes = xml.attributes();
        if (xml.name() == u"item" && attributes.value("properties").toString().split(' ').contains("cover-image"))
            return attributes.value("href").toString();
        if (xml.name() == u"item")
            items.insert(attributes.value("id").toString(), attributes.value("href").toString());
        else if (xml.name() == u"meta" && attributes.value("name") == u"cover")
            named = attributes.value("content").toString();
    }
    return items.value(named);
}

// <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail" Target="docProps/thumbnail.jpeg"/>
QString thumbnailTarget(const QByteArray &relationships) {
    QXmlStreamReader xml(relationships);
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == u"Relationship"
            && xml.attributes().value("Type").endsWith(u"/metadata/thumbnail"))
            return xml.attributes().value("Target").toString();
    return {};
}

// A comic book's page: a picture by its ending, outside the __MACOSX folder a Mac's archiver adds.
bool isPage(const QByteArray &name) {
    const QByteArray lower = name.toLower();
    return !lower.startsWith("__macosx/")
        && std::any_of(kPageEndings.cbegin(), kPageEndings.cend(), [&](const QByteArray &ending) { return lower.endsWith(ending); });
}

// The members a type keeps its picture in under the names its format fixes, best first.
QByteArrayList fixedNames(const QString &mime, const QString &path) {
    if (mime.startsWith(kOpenDocument))
        return {"Thumbnails/thumbnail.png"};
    if (mime.startsWith(kOfficeOpenXml))
        return {"docProps/thumbnail.jpeg"};
    if (mime == k3mf)
        return {"Metadata/thumbnail.png"};
    if (mime == kKrita)
        return {"mergedimage.png", "preview.png"};
    if (mime == kOpenRaster)
        return {"mergedimage.png", "Thumbnails/thumbnail.png"};
    // shared-mime-info has no type for FreeCAD's own files; FreeCAD installs one.
    if (mime == kFreeCad || path.endsWith(".fcstd", Qt::CaseInsensitive))
        return {"thumbnails/Thumbnail.png"};
    return {};
}

// A member's picture, upright and fitted to `pixels`; one that claims more pixels than a RAW preview may is refused undecoded.
QImage picture(QByteArray bytes, int pixels) {
    QBuffer buffer(&bytes);
    if (bytes.isEmpty() || !buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (!full.isValid() || quint64(full.width()) * quint64(full.height()) > RawEmbed::kMaxPixels)
        return {};
    if (full.width() > pixels || full.height() > pixels)
        reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
    return reader.read();
}

// The glyph a font draws `c` with, when it maps it to one that has an outline.
std::optional<quint32> glyphFor(const QRawFont &font, char32_t c) {
    const QList<quint32> glyphs = font.supportsCharacter(c) ? font.glyphIndexesForString(QString::fromUcs4(&c, 1)) : QList<quint32>();
    if (glyphs.size() != 1 || font.pathForGlyph(glyphs.first()).isEmpty())
        return {};
    return glyphs.first();
}

// "Aa" when the font draws both, else its first two letters by code point, else its first two drawable characters.
QList<quint32> sample(const QRawFont &font) {
    const std::optional<quint32> upper = glyphFor(font, 'A'), lower = glyphFor(font, 'a');
    if (upper && lower)
        return {*upper, *lower};
    QList<quint32> letters, drawable;
    // At most a lookup in the font's character map for each letter Unicode has, about 146,000 of them: a few milliseconds, once per file.
    for (char32_t c = kFirstCharacter; c <= kLastCharacter && letters.size() < 2; ++c) {
        const bool letter = QChar::isLetter(c);
        if ((!letter && drawable.size() == 2) || !QChar::isPrint(c) || QChar::isSpace(c))
            continue;
        if (const std::optional<quint32> glyph = glyphFor(font, c)) {
            if (letter)
                letters << *glyph;
            if (drawable.size() < 2)
                drawable << *glyph;
        }
    }
    return letters.size() == 2 ? letters : drawable;
}

// The font's sample black on a white square of `pixels`, made from the file's bytes and never added to Qt's list of fonts.
QImage fontSample(QFile &file, int pixels) {
    if (file.size() > kMostFontBytes)
        return {};
    const QRawFont font(at(file, 0, file.size()), pixels, QFont::PreferNoHinting);
    if (!font.isValid())
        return {};
    const QList<quint32> glyphs = sample(font);
    const QList<QPointF> advances = font.advancesForGlyphIndexes(glyphs);
    QPainterPath letters;
    qreal x = 0;
    for (qsizetype i = 0; i < glyphs.size() && i < advances.size(); ++i) {
        letters.addPath(font.pathForGlyph(glyphs[i]).translated(x, 0));
        x += advances[i].x();
    }
    const QRectF box = letters.boundingRect();
    if (box.isEmpty())
        return {};
    QImage page(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    page.fill(Qt::white);
    QPainter painter(&page);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal scale = kFontFill * pixels / qMax(box.width(), box.height());
    painter.translate(pixels / 2.0, pixels / 2.0);
    painter.scale(scale, scale);
    painter.translate(-box.center());
    painter.fillPath(letters, Qt::black);
    painter.end();
    return page;
}

} // namespace

bool Embedded::handles(const QString &mime, const QString &path) {
    return mime == kEpub || mime == kComic || kFonts.contains(mime) || !fixedNames(mime, path).isEmpty();
}

QImage Embedded::thumbnail(const QString &path, const QString &mime, int pixels) {
    QFile file;
    if (!openRegular(file, path))
        return {};
    // ".otf" names an OpenType font and an OpenDocument formula template alike, and the MIME database answers the template.
    const QByteArray signature = at(file, 0, kSignatureLength);
    if (kFonts.contains(mime) || (mime == kFormulaTemplate && (signature == kCffSignature || signature == kTrueTypeSignature)))
        return fontSample(file, pixels);
    QMap<QByteArray, Member> members;
    if (!directory(file, members))
        return {};
    const auto part = [&](const QByteArray &name) {
        const auto it = members.constFind(name);
        return it == members.cend() ? QByteArray() : read(file, *it);
    };
    QByteArrayList names;
    if (mime == kEpub) {
        // The package that container.xml names lists the cover among its items.
        const QString package = packagePath(part("META-INF/container.xml"));
        if (const QString cover = coverReference(part(package.toUtf8())); !package.isEmpty() && !cover.isEmpty())
            names << resolved(package, cover);
    } else if (mime == kComic) {
        // The first page by name order, as the members are kept.
        for (auto it = members.cbegin(); it != members.cend() && names.isEmpty(); ++it)
            if (isPage(it.key()))
                names << it.key();
    } else {
        // An Office Open XML or 3MF package may name its thumbnail as a relationship of its own, ahead of the usual name.
        if (mime.startsWith(kOfficeOpenXml) || mime == k3mf)
            if (const QString target = thumbnailTarget(part("_rels/.rels")); !target.isEmpty())
                names << resolved({}, target);
        names += fixedNames(mime, path);
    }
    for (const QByteArray &name : names)
        if (const QImage image = picture(part(name), pixels); !image.isNull())
            return image;
    return {};
}
