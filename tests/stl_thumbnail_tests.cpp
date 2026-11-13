#include "render.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>
#include <limits>
#include <sys/stat.h>

using Point = std::array<float, 3>;
using Triangle = std::array<Point, 3>;

static QByteArray binary(const QList<Triangle> &faces) {
    QByteArray bytes(80, ' ');
    QDataStream out(&bytes, QIODevice::Append);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    out << quint32(faces.size());
    for (const auto &face : faces) {
        out << 0.f << 0.f << 0.f;
        for (const auto &point : face) for (float coordinate : point) out << coordinate;
        out << quint16(0);
    }
    return bytes;
}
static QByteArray ascii(const QList<Triangle> &faces) {
    QByteArray bytes = "solid public test\r\n";
    for (const auto &face : faces) {
        bytes += "facet\tnormal 0 0 1\r\nouter   loop\r\n";
        for (const auto &point : face) {
            bytes += "vertex";
            for (float coordinate : point) bytes += (coordinate < 0 ? " " : " +") + QByteArray::number(coordinate, 'e', 8);
            bytes += "\r\n";
        }
        bytes += "endloop\r\nendfacet\r\n";
    }
    return bytes + "endsolid public test\r\n";
}
static bool write(const QString &name, const QByteArray &bytes) {
    QFile file(name);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
static qsizetype covered(const QImage &image) {
    qsizetype count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) count += qAlpha(image.pixel(x, y)) != 0;
    return count;
}

class StlThumbnailTests : public QObject {
    Q_OBJECT
    const Triangle face{{{0,0,0}, {10,0,0}, {0,10,0}}};
    const QString worker = QCoreApplication::applicationDirPath() + "/folio-stl-thumbnailer";
private slots:
    void binaryAsciiAndBothWindingsAgree() {
        const QImage expected = StlThumbnail::render(binary({face}), 128);
        QVERIFY(!expected.isNull());
        QCOMPARE(expected.size(), QSize(128, 128));
        QVERIFY(covered(expected) > 1000);
        QVERIFY(covered(expected) < 128*128);
        QCOMPARE(StlThumbnail::render(ascii({face}), 128), expected);
        auto backwards = face;
        std::swap(backwards[1], backwards[2]);
        QCOMPARE(StlThumbnail::render(binary({backwards}), 128), expected);
        QByteArray solid = binary({face});
        solid.replace(0, 5, "solid");
        QCOMPARE(StlThumbnail::render(solid, 128), expected);
        for (int y = 0; y < expected.height(); ++y) {
            for (int x = 0; x < expected.width(); ++x) {
                const QRgb p = expected.pixel(x,y);
                QVERIFY(qRed(p) <= qAlpha(p) && qGreen(p) <= qAlpha(p) && qBlue(p) <= qAlpha(p));
            }
        }
    }
    void depthDoesNotDependOnFaceOrder() {
        const double r = std::sqrt(.5), d = 1/std::sqrt(3.), u = 1/std::sqrt(6.);
        const auto point = [&](double x, double y, double z) {
            return Point{float(x*r-y*u+z*d), float(x*r+y*u-z*d), float(2*y*u+z*d)};
        };
        const Triangle near{{point(-1,-1,2), point(1,-1,6), point(0,1,4)}};
        const Triangle far{{point(-1,-1,0), point(1,-1,0), point(0,1,0)}};
        const auto a = StlThumbnail::render(binary({near,far}), 128);
        const auto b = StlThumbnail::render(binary({far,near}), 128);
        QVERIFY(!a.isNull());
        QCOMPARE(a, b);
    }
    void rejectsInvalidAndUnboundedInputs() {
        QVERIFY(StlThumbnail::render({}, 128).isNull());
        QVERIFY(StlThumbnail::render(binary({face}).chopped(1), 128).isNull());
        QVERIFY(StlThumbnail::render(ascii({face}).replace("endfacet", "incomplete"), 128).isNull());
        QVERIFY(StlThumbnail::render(ascii({face}).replace("endsolid", "unclosed"), 128).isNull());
        QVERIFY(StlThumbnail::render(binary({face}), 0).isNull());
        QVERIFY(StlThumbnail::render(binary({face}), StlThumbnail::maxPixels+1).isNull());
        auto coloured = binary({face}); coloured[coloured.size()-1] = char(0x80);
        QVERIFY(StlThumbnail::render(coloured,128).isNull());
        coloured = binary({face}); coloured.replace(0,6,"COLOR=");
        QVERIFY(StlThumbnail::render(coloured,128).isNull());
        for (float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1e20f}) {
            auto bad = face; bad[0][0] = value;
            QVERIFY(StlThumbnail::render(binary({bad}), 128).isNull());
        }
        auto degenerate = face; degenerate[1] = degenerate[2] = degenerate[0];
        QVERIFY(StlThumbnail::render(binary({degenerate}), 128).isNull());
        QVERIFY(StlThumbnail::render("solid " + QByteArray(1025, 'a') + "\nendsolid\n", 128).isNull());
        QVERIFY(StlThumbnail::render(ascii({face}).replace("vertex", QByteArray(129, 'x')), 128).isNull());
    }
    void finiteExtremeAndTinyCoordinatesStayBounded() {
        for (float scale : {1e-25f, 1e7f}) {
            auto scaled = face;
            for (auto &p : scaled) for (auto &c : p) c *= scale;
            const auto image = StlThumbnail::render(binary({scaled}), 128);
            QVERIFY(!image.isNull());
            QVERIFY(covered(image) > 1000);
        }
        auto translated = face;
        for (auto &p : translated) for (auto &c : p) c = c*10000+1e8f;
        QVERIFY(!StlThumbnail::render(binary({translated}), 128).isNull());
    }
    void boundsOverdrawRatherThanReturningAPartialPicture() {
        QList<Triangle> repeated;
        for (int i = 0; i < 100; ++i) repeated << face;
        QString error;
        QVERIFY(StlThumbnail::render(binary(repeated), 1024, &error).isNull());
        QVERIFY(error.contains("work bound"));
    }
    void commandReadsRegularFilesAndPreservesExistingOutput() {
        QTemporaryDir dir;
        const QString input = dir.filePath("part 雪 50%off '$().stl"), output = dir.filePath("out.png");
        QVERIFY(write(input, binary({face})));
        auto run = [&](const QString &path, const QString &target) {
            QProcess process;
            process.start(worker, {path,target,"128"});
            if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(); return -1; }
            return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        };
        QCOMPARE(run(input, output), 0);
        QCOMPARE(QImage(output).size(), QSize(128,128));
        QVERIFY(write(output, "caller data"));
        QVERIFY(run(input, output) != 0);
        QFile kept(output); QVERIFY(kept.open(QIODevice::ReadOnly)); QCOMPARE(kept.readAll(), QByteArray("caller data"));
        const QString link = dir.filePath("link.stl");
        QVERIFY(QFile::link(input, link));
        QCOMPARE(run(link, dir.filePath("linked.png")), 0);
        const QString fifo = dir.filePath("fifo.stl");
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(run(fifo, dir.filePath("fifo.png")) > 0);
        const QString huge = dir.filePath("huge.stl");
        QFile sparse(huge); QVERIFY(sparse.open(QIODevice::WriteOnly));
        QVERIFY(sparse.resize(StlThumbnail::maxBytes+1)); sparse.close();
        QVERIFY(run(huge, dir.filePath("huge.png")) > 0);
        QVERIFY(!QFile::exists(dir.filePath("huge.png")));
    }
};
QTEST_GUILESS_MAIN(StlThumbnailTests)
#include "stl_thumbnail_tests.moc"
