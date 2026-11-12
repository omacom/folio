#include "thumbnails.h"
#include "rawembed.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
#include <QDir>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QStringDecoder>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QSaveFile>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>

namespace {

QString fileUri(const QString &path) {
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

// Qt writes longer PNG text values as compressed zTXt chunks. The thumbnail
// spec's keys belong in plain tEXt, which is all some thumbnail readers look
// at, so re-encode them after Qt writes the image.
QByteArray withPlainText(const QByteArray &png) {
    if (!png.startsWith("\x89PNG\r\n\x1a\n"))
        return png;
    QByteArray out = png.left(8);
    int at = 8;
    while (at + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + at);
        if (at + 12 + qsizetype(length) > png.size())
            return png;
        const QByteArray type = png.mid(at + 4, 4);
        const QByteArray body = png.mid(at + 8, length);
        if (type == "zTXt") {
            const int nul = body.indexOf('\0');
            QByteArray compressed = body.mid(nul + 2);
            // qUncompress wants the expected length up front; the 4 bytes are a
            // hint, so give it a generous one.
            compressed.prepend(QByteArray("\x00\x01\x00\x00", 4));
            const QByteArray text = qUncompress(compressed);
            if (nul > 0 && !text.isNull()) {
                const QByteArray plain = body.left(nul) + '\0' + text;
                QByteArray chunk(4, 0);
                qToBigEndian<quint32>(plain.size(), chunk.data());
                const QByteArray typed = "tEXt" + plain;
                chunk += typed;
                QByteArray crc(4, 0);
                qToBigEndian<quint32>(crc32(0, reinterpret_cast<const Bytef *>(typed.constData()), typed.size()), crc.data());
                out += chunk + crc;
                at += 12 + length;
                continue;
            }
        }
        out += png.mid(at, 12 + length);
        at += 12 + length;
    }
    return out;
}

// The spec's Exec field: %i input path, %u input URI, %o output, %s size.
QStringList expandCommand(const QString &command, const QString &input, const QString &output, int size) {
    QStringList args = QProcess::splitCommand(command);
    for (QString &arg : args) {
        arg.replace("%i", input).replace("%u", fileUri(input)).replace("%o", output)
           .replace("%s", QString::number(size)).replace("%%", "%");
    }
    return args;
}

// Each optional package's thumbnailers and the MimeType= lines they ship, as "video/mp4;video/webm;". Copied from the
// .thumbnailer files of Arch's ffmpegthumbnailer 2.3.1 (the video one only: the audio one draws just cover art, which
// most files lack), glycin 2.2.6, papers 51.1 and libgsf's gsf-office; refresh them from there when a package adds types.
struct PackageTypes {
    const char *package;
    const char *types;
};
const PackageTypes packageTypes[] = {
    {"ffmpegthumbnailer",
     "video/3gpp;video/3gpp2;video/annodex;video/dv;video/isivideo;video/matroska;video/matroska-3d;"
     "video/mj2;video/mp2t;video/mp4;video/mpeg;video/ogg;video/quicktime;video/vnd.avi;"
     "video/vnd.mpegurl;video/vnd.radgamettools.bink;video/vnd.radgamettools.smacker;"
     "video/vnd.rn-realvideo;video/vnd.vivo;video/vnd.youtube.yt;video/wavelet;video/webm;"
     "video/x-anim;video/x-flic;video/x-flv;video/x-javafx;video/x-matroska;video/x-matroska-3d;"
     "video/x-mjpeg;video/x-mng;video/x-ms-wmv;video/x-nsv;video/x-ogm+ogg;video/x-sgi-movie;"
     "video/x-theora+ogg;application/mxf;application/vnd.ms-asf;application/vnd.rn-realmedia;"
     "application/matroska;application/x-matroska;application/ogg;"},
    {"glycin",
     "image/avif;image/heif;image/apng;image/bmp;image/gif;image/jp2;image/jpeg;image/png;image/qoi;"
     "image/tiff;image/vnd.microsoft.icon;image/webp;image/x-dds;image/x-exr;image/x-jp2-codestream;"
     "image/x-portable-anymap;image/x-portable-bitmap;image/x-portable-graymap;"
     "image/x-portable-pixmap;image/x-tga;image/x-win-bitmap;image/x-xbitmap;image/x-xpixmap;"
     "image/jxl;image/svg+xml;image/svg+xml-compressed;"},
    {"papers",
     "application/vnd.comicbook-rar;application/vnd.comicbook+zip;application/x-cb7;application/x-cbr;"
     "application/x-cbt;application/x-cbz;application/x-ext-cb7;application/x-ext-cbr;"
     "application/x-ext-cbt;application/x-ext-cbz;application/x-ext-djv;application/x-ext-djvu;"
     "image/vnd.djvu;image/vnd.djvu+multipage;application/pdf;application/x-bzpdf;"
     "application/x-ext-pdf;application/x-gzpdf;application/x-xzpdf;application/illustrator;"
     "image/tiff;"},
    {"libgsf",
     "application/msword;application/vnd.ms-excel;application/vnd.ms-powerpoint;"
     "application/vnd.oasis.opendocument.chart;application/vnd.oasis.opendocument.formula;"
     "application/vnd.oasis.opendocument.graphics;"
     "application/vnd.oasis.opendocument.graphics-template;application/vnd.oasis.opendocument.image;"
     "application/vnd.oasis.opendocument.presentation;"
     "application/vnd.oasis.opendocument.presentation-template;"
     "application/vnd.oasis.opendocument.spreadsheet;"
     "application/vnd.oasis.opendocument.spreadsheet-template;application/vnd.oasis.opendocument.text;"
     "application/vnd.oasis.opendocument.text-master;application/vnd.oasis.opendocument.text-template;"
     "application/vnd.openxmlformats-officedocument.presentationml.presentation;"
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet;"
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document;"
     "application/vnd.sun.xml.calc;application/vnd.sun.xml.calc.template;application/vnd.sun.xml.draw;"
     "application/vnd.sun.xml.draw.template;application/vnd.sun.xml.impress;"
     "application/vnd.sun.xml.impress.template;application/vnd.sun.xml.math;"
     "application/vnd.sun.xml.writer;application/vnd.sun.xml.writer.global;"
     "application/vnd.sun.xml.writer.template;"},
};

// The package for each type by its own name, read once; the first package listed wins, so glycin draws TIFF before papers.
const QHash<QString, QString> &packageFor() {
    static const QHash<QString, QString> table = [] {
        QMimeDatabase mimes;
        QHash<QString, QString> byType;
        for (const PackageTypes &entry : packageTypes) {
            for (const QString &declared : QString::fromLatin1(entry.types).split(';', Qt::SkipEmptyParts)) {
                const QString type = mimes.mimeTypeForName(declared).name();
                if (const QString key = type.isEmpty() ? declared : type; !byType.contains(key))
                    byType.insert(key, QString::fromLatin1(entry.package));
            }
        }
        return byType;
    }();
    return table;
}

} // namespace

Thumbnails::Thumbnails(const QString &cacheDirectory, const QStringList &thumbnailerDirectories)
    : m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails"
                  : cacheDirectory) {
    QStringList directories = thumbnailerDirectories;
    if (directories.isEmpty()) {
        // Later directories win, so the user's own thumbnailers override the
        // system's.
        QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        std::reverse(data.begin(), data.end());
        for (const QString &base : data)
            directories << base + "/thumbnailers";
    }
    QMimeDatabase mimes;
    for (const QString &directory : directories) {
        // A .thumbnailer file may name a type by an alias, as glycin names DDS pictures image/x-dds.
        QHash<QString, QString> named, aliased;
        QDirIterator it(directory, {"*.thumbnailer"}, QDir::Files);
        while (it.hasNext()) {
            // A tiny desktop-entry reader: QSettings would treat the
            // semicolons in MimeType and the %-codes in Exec as its own syntax.
            QFile file(it.next());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QHash<QString, QString> fields;
            bool inEntry = false;
            for (const QByteArray &raw : file.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.startsWith('['))
                    inEntry = line == "[Thumbnailer Entry]";
                else if (inEntry && line.contains('=') && !line.startsWith('#'))
                    fields.insert(line.section('=', 0, 0).trimmed(), line.section('=', 1).trimmed());
            }
            const QString exec = fields.value("Exec");
            const QString tryExec = fields.value("TryExec");
            if (exec.isEmpty() || (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()))
                continue;
            for (const QString &declared : fields.value("MimeType").split(';', Qt::SkipEmptyParts)) {
                const QString mime = declared.trimmed();
                named.insert(mime, exec);
                if (const QString type = mimes.mimeTypeForName(mime).name(); !type.isEmpty())
                    aliased.insert(type, exec);
            }
        }
        // Within one directory the file that names a type itself wins over one that names an alias of it.
        m_thumbnailers.insert(aliased);
        m_thumbnailers.insert(named);
    }
    // An older Folio's failure markers (fail/folio, or a fail/folio-<n> with a lower number) are never read again, so
    // they are cleared away, on a worker since there may be thousands. A newer Folio's are left for it, so an installed
    // Folio and a newer build side by side don't clear each other's on every start. A symlink is never followed, the fail
    // folder itself included: removing through one would delete files outside the cache.
    QThreadPool::globalInstance()->start([fail = m_cache + "/fail"] {
        if (QFileInfo(fail).isSymLink())
            return;
        const int current = failures.section('-', -1).toInt();
        for (const QString &name :
             QDir(fail).entryList({"folio", "folio-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            bool numbered = true;
            const int version = name == "folio" ? 0 : name.mid(6).toInt(&numbered);
            if (numbered && version < current)
                QDir(fail + "/" + name).removeRecursively();
        }
    });
}

QString Thumbnails::missingPackage(const QString &mime) const {
    // Qt draws its own pictures in-process, an installed thumbnailer needs nothing more, and pdftoppm draws a PDF without one.
    if ((mime.startsWith("image/") && QImageReader::supportedMimeTypes().contains(mime.toLatin1()))
        || m_thumbnailers.contains(mime)
        || (mime == "application/pdf" && !QStandardPaths::findExecutable("pdftoppm").isEmpty()))
        return {};
    return packageFor().value(mime);
}

QString Thumbnails::cacheKey(const QString &path) {
    return QCryptographicHash::hash(fileUri(path).toUtf8(), QCryptographicHash::Md5).toHex() + ".png";
}

QString Thumbnails::bucketFor(int size) {
    if (size <= 128) return "normal";
    if (size <= 256) return "large";
    if (size <= 512) return "x-large";
    return "xx-large";
}

static int bucketPixels(const QString &bucket) {
    if (bucket == "normal") return 128;
    if (bucket == "large") return 256;
    if (bucket == "x-large") return 512;
    return 1024;
}

QString Thumbnails::cachePath(const QString &path, int size) const {
    return m_cache + "/" + bucketFor(size) + "/" + cacheKey(path);
}

QString Thumbnails::failPath(const QString &path) const {
    return m_cache + "/" + failures + "/" + cacheKey(path);
}

// The marker's own key for the thumbnailer command the type had when it failed, empty for none.
static const QString kFailedWith = QStringLiteral("X-Folio::Thumbnailer");

QImage Thumbnails::load(const QString &path, const QString &mime, int size) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return {};

    // Thumbnails of the cache itself would recurse forever.
    if (info.absoluteFilePath().startsWith(m_cache + "/"))
        return {};

    const QDateTime modified = info.lastModified();
    if (const QImage image(cachePath(path, size)); madeFor(image, path, modified))
        return image;

    // A folder's peek asks for any picture ("image/*"); named by extension,
    // as the grid names it, it goes to the same thumbnailer as its tile.
    const QString type = mime.endsWith("/*")
        ? QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name()
        : mime;
    const QString thumbnailer = m_thumbnailers.value(type);
    if (failedBefore(path, modified, thumbnailer))
        return {};
    const int pixels = bucketPixels(bucketFor(size));
    bool attempted = false;
    QImage image = generate(path, type, pixels, attempted);
    if (image.isNull()) {
        // Only a reader or thumbnailer that ran and failed is remembered, so
        // one installed later still gets its turn.
        if (attempted)
            rememberFailure(path, modified, thumbnailer);
        return {};
    }
    if (image.width() > pixels || image.height() > pixels)
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    remember(image, path, size, modified);
    return image;
}

QImage Thumbnails::generate(const QString &path, const QString &mime, int pixels, bool &attempted) const {
    // A camera RAW with no installed thumbnailer, the desktop's own answer, shows the JPEG the camera embedded.
    if (RawEmbed::handles(mime) && !m_thumbnailers.contains(mime)) {
        attempted = true;
        if (const QImage image = RawEmbed::thumbnail(path, pixels); !image.isNull())
            return image;
    }
    // Common photo formats decode fastest in-process; anything Qt can't read
    // (HEIC, JPEG XL) falls through to the installed thumbnailers.
    if (mime.startsWith("image/")) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            attempted = true;
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
            const QImage image = reader.read();
            if (!image.isNull())
                return image;
        }
    }
    const QString command = m_thumbnailers.value(mime);
    if (!command.isEmpty()) {
        attempted = true;
        return runThumbnailer(command, path, pixels);
    }
    // Without a PDF thumbnailer installed, Poppler's pdftoppm (there with
    // CUPS) draws the first page.
    if (mime == "application/pdf" && !QStandardPaths::findExecutable("pdftoppm").isEmpty()) {
        attempted = true;
        return pdfPage(path, pixels);
    }
    return {};
}

QImage Thumbnails::textPage(const QString &path, int pixels, const QColor &paper, const QColor &ink) {
    // Only a regular file: a FIFO or device named like text would block the
    // read, and the worker with it, until something writes to it.
    if (!QFileInfo(path).isFile())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = file.read(4096);
    // Binary that merely claims to be text (or a cut through a multibyte
    // character at the very end, which is fine) gets the stand-in instead.
    if (head.contains('\0'))
        return {};
    QStringDecoder decode(QStringDecoder::Utf8);
    QString text = decode(head);
    if (decode.hasError() && head.size() < 4096)
        return {};

    // A sheet a little narrower than it is tall, so it still reads as a page
    // rather than a folder, in the theme's colors: the monospace font at a
    // size that stays readable on the tile, as many lines as fit, no wrapping.
    const QSize size(qRound(pixels * 0.85), pixels);
    QImage page(size, QImage::Format_ARGB32_Premultiplied);
    page.fill(paper);
    QPainter painter(&page);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qMax(6, pixels / 17));
    painter.setFont(font);
    painter.setPen(ink);
    const QFontMetricsF metrics(font);
    const qreal margin = size.width() * 0.08, line = metrics.lineSpacing();
    // Long lines stop at the right margin rather than the page's edge.
    painter.setClipRect(QRectF(margin, 0, size.width() - 2 * margin, size.height()));
    qreal y = margin + metrics.ascent();
    for (QString row : text.replace("\r\n", "\n").split('\n')) {
        if (y > size.height() - margin)
            break;
        row.replace('\t', "    ");
        painter.drawText(QPointF(margin, y), row);
        y += line;
    }
    painter.end();
    return page;
}

QImage Thumbnails::pdfPage(const QString &path, int pixels) {
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    QProcess process;
    process.start("pdftoppm", {"-f", "1", "-l", "1", "-singlefile", "-png", "-scale-to", QString::number(pixels),
                               path, scratch.filePath("page")});
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage(scratch.filePath("page.png"));
}

QImage Thumbnails::runThumbnailer(const QString &command, const QString &path, int pixels) const {
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    const QString output = scratch.filePath("thumbnail.png");
    QStringList args = expandCommand(command, path, output, pixels);
    if (args.isEmpty())
        return {};
    const QString program = args.takeFirst();
    // ffmpegthumbnailer's -f frames the picture as a film strip; the tile
    // marks a video with its own play badge instead.
    if (QFileInfo(program).fileName() == "ffmpegthumbnailer")
        args.removeAll("-f");

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.start(program, args);
    // A broken file must never hang a tile forever.
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage(output);
}

void Thumbnails::remember(QImage image, const QString &path, int size, const QDateTime &modified) const {
    image.setText("Software", "folio");
    save(std::move(image), cachePath(path, size), path, modified);
}

void Thumbnails::rememberFailure(const QString &path, const QDateTime &modified, const QString &thumbnailer) const {
    QImage marker(1, 1, QImage::Format_ARGB32);
    marker.fill(Qt::transparent);
    marker.setText(kFailedWith, thumbnailer);
    save(std::move(marker), failPath(path), path, modified);
}

bool Thumbnails::failedBefore(const QString &path, const QDateTime &modified, const QString &thumbnailer) const {
    const QImage marker(failPath(path));
    return madeFor(marker, path, modified) && marker.text(kFailedWith) == thumbnailer;
}

// Whether a cached thumbnail (or failure marker) was made from the file as it
// was last modified. The spec's Thumb::MTime is in whole seconds, which a
// picture saved over within the same second matches; Folio's own also keep
// the milliseconds. One another app made goes by the seconds alone.
// QImageReader::text() splits keys at ':', so these are only readable from
// the decoded image.
bool Thumbnails::madeFor(const QImage &image, const QString &path, const QDateTime &modified) {
    const QString exact = image.text("X-Folio::MTime");
    // The spec asks a reader to check that Thumb::URI names the file too.
    return !image.isNull() && image.text("Thumb::URI") == fileUri(path)
        && image.text("Thumb::MTime") == QString::number(modified.toSecsSinceEpoch())
        && (exact.isEmpty() || exact == QString::number(modified.toMSecsSinceEpoch()));
}

void Thumbnails::save(QImage image, const QString &target, const QString &path, const QDateTime &modified) const {
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::setPermissions(m_cache, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    image.setText("Thumb::URI", fileUri(path));
    image.setText("Thumb::MTime", QString::number(modified.toSecsSinceEpoch()));
    image.setText("X-Folio::MTime", QString::number(modified.toMSecsSinceEpoch()));

    // Written beside the target and renamed, so another app never reads half
    // a thumbnail.
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return;
    QSaveFile file(target);
    if (file.open(QIODevice::WriteOnly) && file.write(withPlainText(png)) > 0) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.commit();
    }
}

namespace {

class ThumbnailResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbnailResponse(const Thumbnails *thumbnails, const QString &id) : m_thumbnails(thumbnails), m_id(id) {
        setAutoDelete(false);
    }

    void run() override {
        // id = <size>/<modified>/<url-encoded mime>/<url-encoded path>, or
        // page/<size>/<modified>/<paper>/<ink>/<url-encoded path> for text.
        // The time is only there to tell Qt's own cache apart; load() checks
        // the file's.
        const QStringList parts = m_id.split('/');
        // A tile scrolled away before its turn came shouldn't keep the pool
        // from the ones on screen.
        if (!m_cancelled && parts.size() >= 6 && parts.at(0) == "page") {
            const QString path = QUrl::fromPercentEncoding(parts.mid(5).join('/').toUtf8());
            m_image = Thumbnails::textPage(path, qBound(64, parts.at(1).toInt(), 1024),
                                           QColor::fromString("#" + parts.at(3)), QColor::fromString("#" + parts.at(4)));
        } else if (!m_cancelled && parts.size() >= 4) {
            const int size = parts.at(0).toInt();
            const QString mime = QUrl::fromPercentEncoding(parts.at(2).toUtf8());
            const QString path = QUrl::fromPercentEncoding(parts.mid(3).join('/').toUtf8());
            m_image = m_thumbnails->load(path, mime, qBound(64, size, 1024));
        }
        if (m_image.isNull())
            m_error = QStringLiteral("No thumbnail");
        emit finished();
    }

    void cancel() override { m_cancelled = true; }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }

private:
    const Thumbnails *m_thumbnails;
    QString m_id;
    QImage m_image;
    QString m_error;
    std::atomic_bool m_cancelled{false};
};

} // namespace

ThumbnailProvider::ThumbnailProvider(const Thumbnails *thumbnails) : m_thumbnails(thumbnails) {
    // Thumbnailers are separate processes; a few at once keeps a big folder
    // filling in quickly without starving the machine.
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 6));
}

// The size comes in the id; no view sets a sourceSize to request one.
QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &) {
    auto *response = new ThumbnailResponse(m_thumbnails, id);
    m_pool.start(response);
    return response;
}
