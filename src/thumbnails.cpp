#include "thumbnails.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QSaveFile>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

namespace {

QString fileUri(const QString &path) {
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

// The cache spec's fail/<program>-<version>: a new version tries a file again that an older one gave up on.
QString failureFolder() {
    return "/fail/folio-" + QCoreApplication::applicationVersion() + "/";
}

QString mtimeOf(const QString &path) {
    return QString::number(QFileInfo(path).lastModified().toSecsSinceEpoch());
}

// The spec's Exec field: %i input path, %u input URI, %o output, %s size.
// Qt writes longer PNG text values as compressed zTXt chunks, which some
// thumbnail readers (Qt's own included) don't return. The thumbnail spec's
// keys belong in plain tEXt, so re-encode them after Qt writes the image.
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

QStringList expandCommand(const QString &command, const QString &input, const QString &output, int size) {
    QStringList args = QProcess::splitCommand(command);
    for (QString &arg : args) {
        arg.replace("%i", input).replace("%u", fileUri(input)).replace("%o", output)
           .replace("%s", QString::number(size)).replace("%%", "%");
    }
    return args;
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
    for (const QString &directory : directories) {
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
            for (const QString &mime : fields.value("MimeType").split(';', Qt::SkipEmptyParts))
                m_thumbnailers.insert(mime.trimmed(), exec);
        }
    }
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

QImage Thumbnails::load(const QString &path, const QString &mime, int size) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return {};

    // Thumbnails of the cache itself would recurse forever.
    if (info.absoluteFilePath().startsWith(m_cache + "/"))
        return {};

    const QString cached = cachePath(path, size);
    const QString mtime = mtimeOf(path);
    // QImageReader::text() splits keys at ':', so "Thumb::MTime" is only
    // readable from the decoded image.
    if (const QImage image(cached); !image.isNull() && image.text("Thumb::MTime") == mtime)
        return image;
    if (failedBefore(path))
        return {};

    const int pixels = bucketPixels(bucketFor(size));
    QImage image = generate(path, mime, pixels);
    if (image.isNull()) {
        rememberFailure(path);
        return {};
    }
    if (image.width() > pixels || image.height() > pixels)
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    remember(image, path, size);
    return image;
}

QImage Thumbnails::generate(const QString &path, const QString &mime, int pixels) const {
    // Common photo formats decode fastest in-process; anything Qt can't read
    // (HEIC, JPEG XL) falls through to the installed thumbnailers.
    if (mime.startsWith("image/")) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
            const QImage image = reader.read();
            if (!image.isNull())
                return image;
        }
    }
    const QString command = m_thumbnailers.value(mime);
    if (command.isEmpty())
        return {};
    return runThumbnailer(command, path, pixels);
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

void Thumbnails::remember(const QImage &image, const QString &path, int size) const {
    const QString target = cachePath(path, size);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::setPermissions(m_cache, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QImage tagged = image;
    tagged.setText("Thumb::URI", fileUri(path));
    tagged.setText("Thumb::MTime", mtimeOf(path));
    tagged.setText("Software", "folio");

    // Written beside the target and renamed, so another app never reads half
    // a thumbnail.
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !tagged.save(&buffer, "PNG"))
        return;
    QSaveFile file(target);
    if (file.open(QIODevice::WriteOnly) && file.write(withPlainText(png)) > 0) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.commit();
    }
}

void Thumbnails::rememberFailure(const QString &path) const {
    const QString target = m_cache + failureFolder() + cacheKey(path);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QImage marker(1, 1, QImage::Format_ARGB32);
    marker.fill(Qt::transparent);
    marker.setText("Thumb::URI", fileUri(path));
    marker.setText("Thumb::MTime", mtimeOf(path));
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !marker.save(&buffer, "PNG"))
        return;
    QSaveFile file(target);
    if (file.open(QIODevice::WriteOnly) && file.write(withPlainText(png)) > 0)
        file.commit();
}

bool Thumbnails::failedBefore(const QString &path) const {
    const QImage marker(m_cache + failureFolder() + cacheKey(path));
    return !marker.isNull() && marker.text("Thumb::MTime") == mtimeOf(path);
}

namespace {

class ThumbnailResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbnailResponse(const Thumbnails *thumbnails, const QString &id, const QSize &requested)
        : m_thumbnails(thumbnails), m_id(id), m_requested(requested) {
        setAutoDelete(false);
    }

    void run() override {
        // id = <size>/<url-encoded mime>/<url-encoded path>
        const QStringList parts = m_id.split('/');
        if (parts.size() >= 3) {
            int size = parts.at(0).toInt();
            if (m_requested.isValid())
                size = qMax(size, qMax(m_requested.width(), m_requested.height()));
            const QString mime = QUrl::fromPercentEncoding(parts.at(1).toUtf8());
            const QString path = QUrl::fromPercentEncoding(parts.mid(2).join('/').toUtf8());
            m_image = m_thumbnails->load(path, mime, qBound(64, size, 1024));
        }
        if (m_image.isNull())
            m_error = QStringLiteral("No thumbnail");
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }

private:
    const Thumbnails *m_thumbnails;
    QString m_id;
    QSize m_requested;
    QImage m_image;
    QString m_error;
};

} // namespace

ThumbnailProvider::ThumbnailProvider(const Thumbnails *thumbnails) : m_thumbnails(thumbnails) {
    // Thumbnailers are separate processes; a few at once keeps a big folder
    // filling in quickly without starving the machine.
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 6));
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    auto *response = new ThumbnailResponse(m_thumbnails, id, requestedSize);
    m_pool.start(response);
    return response;
}
