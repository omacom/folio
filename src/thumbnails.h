#pragma once

#include <QHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QImage>
#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QStringList>
#include <QSet>
#include <QThreadPool>
#include <atomic>

// Thumbnails per the freedesktop thumbnail spec: one shared cache under
// ~/.cache/thumbnails, keyed by the MD5 of the file's URI and invalidated by
// its modification time, so thumbnails made by any other app are reused and
// ours are useful to them. Images are scaled and text is set on a page
// in-process; everything else goes through the thumbnailers installed in
// /usr/share/thumbnailers (videos via ffmpegthumbnailer, and so on), with
// Poppler's pdftoppm drawing PDFs when no thumbnailer does. Nothing is generated until a
// tile asks for it: there is no background crawl.
class Thumbnails : public QObject {
    Q_OBJECT

public:
    // `patience` covers a thumbnail and any fallback, in milliseconds. A null
    // stlHelper finds the bundled companion; an explicit empty string disables it.
    explicit Thumbnails(const QString &cacheDirectory = {}, const QStringList &thumbnailerDirectories = {},
                        int patience = 15000, const QString &stlHelper = {});

    // Returns a thumbnail no larger than `size` on its longest side, from the
    // cache when it is fresh, or a null image when none can be made.
    QImage load(const QString &path, const QString &mime, int size) const;
    // Stops the thumbnailers still running and starts no more, as Folio quits: it waits for every thumbnail being made,
    // and a hung thumbnailer would keep it waiting.
    void quit() { m_quitting = true; }

    static QString cacheKey(const QString &path);
    // The spec's fail/<program>-<version>: the number is raised when Folio learns to draw more, so old failures are tried
    // again. 4: a bundled CPU helper can draw STL files a system thumbnailer could not.
    static inline const QString failures = QStringLiteral("fail/folio-4");
    static QString bucketFor(int size);
    // A text file's first lines set on a page in the given colors. Made
    // fresh each time rather than cached, since the colors follow the theme.
    static QImage textPage(const QString &path, int pixels, const QColor &paper, const QColor &ink);
    QString thumbnailerFor(const QString &mime) const { return m_thumbnailers.value(mime); }
    // The package that would draw `mime` when neither Qt nor an installed thumbnailer does, or nothing.
    Q_INVOKABLE QString missingPackage(const QString &mime) const;

private:
    QString cachePath(const QString &path, int size) const;
    QString failPath(const QString &path) const;
    bool nativeStl(const QString &mime) const;
    QString providerKey(const QString &mime, int pixels) const;
    // Sets `attempted` when Qt's reader or a thumbnailer could take the file.
    QImage generate(const QString &path, const QString &mime, int pixels, bool &attempted) const;
    // Both answer whether the program judged the file: one that could not start, or was stopped as Folio quit, has not.
    bool runThumbnailer(const QString &command, const QString &path, int pixels, QImage &image, QDeadlineTimer deadline) const;
    bool pdfPage(const QString &path, int pixels, QImage &image, QDeadlineTimer deadline) const;
    bool stlPicture(const QString &path, int pixels, QImage &image, QDeadlineTimer deadline) const;
    void remember(QImage image, const QString &path, int size, const QDateTime &modified) const;
    // A failure goes with the type's thumbnailer as it was then, none included: one installed since gets its turn.
    void rememberFailure(const QString &path, const QDateTime &modified, const QString &thumbnailer) const;
    bool failedBefore(const QString &path, const QDateTime &modified, const QString &thumbnailer) const;
    void save(QImage image, const QString &target, const QString &path, const QDateTime &modified) const;
    static bool madeFor(const QImage &image, const QString &path, const QDateTime &modified);

    QString m_cache;
    int m_patience;
    std::atomic_bool m_quitting{false};
    QHash<QString, QString> m_thumbnailers;
    QSet<QString> m_userThumbnailers;
    QString m_stlHelper;
};

// Serves image://thumb/<size>/<modified>/<mime>/<path> to QML on a thread pool.
class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    explicit ThumbnailProvider(const Thumbnails *thumbnails);
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    const Thumbnails *m_thumbnails;
    QThreadPool m_pool;
};
