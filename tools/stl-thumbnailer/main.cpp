#include "render.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QTextStream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    bool sizeOk = false;
    const int pixels = args.value(3).toInt(&sizeOk);
    if (args.size() != 4 || !QDir::isAbsolutePath(args[1]) || !QDir::isAbsolutePath(args[2])
        || !sizeOk || pixels < 1 || pixels > StlThumbnail::maxPixels) return 2;
    // Nonblocking open and fstat refer to the same descriptor, including symlinks:
    // a FIFO swapped in during lookup cannot hold the helper waiting for a writer.
    const int fd = ::open(QFile::encodeName(args[1]).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 1;
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size <= 0 || status.st_size > StlThumbnail::maxBytes) { ::close(fd); return 1; }
    QFile input;
    if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return 1; }
    const QByteArray bytes = input.read(status.st_size+1);
    if (input.error() != QFileDevice::NoError || bytes.size() != status.st_size) return 1;
    QString error;
    const QImage image = StlThumbnail::render(bytes, pixels, &error);
    if (image.isNull()) { QTextStream(stderr) << error << '\n'; return 1; }
    QFile output(args[2]);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 1;
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QImageWriter writer(&output, "png");
    writer.setCompression(1);
    if (!writer.write(image) || !output.flush()) { output.close(); output.remove(); return 1; }
    return 0;
}
