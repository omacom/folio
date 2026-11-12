#include <QCollator>
#include <QDir>
#include <memory>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QTemporaryDir>
#include <QtTest>

#include "translator.h"

// Never read files or start Folio's thumbnail workers in these view tests.
class BlankThumbnail : public QQuickImageProvider {
public:
    BlankThumbnail() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *, const QSize &) override {
        QImage image(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        return image;
    }
};

class LocaleTests : public QObject {
    Q_OBJECT

    static void write(const QString &path, const QByteArray &value) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(value), value.size());
    }

    static void verifyUiLocale(const Translator &translator, const QLocale &ui, const QString &open) {
        QCOMPARE(QLocale().bcp47Name(), ui.bcp47Name());
        QCOMPARE(QLocale().toString(12345), ui.toString(12345));
        QCOMPARE(QLocale().toString(1.5, 'f', 1), ui.toString(1.5, 'f', 1));
        QCOMPARE(QCollator().locale().bcp47Name(), ui.bcp47Name());
        QCOMPARE(QGuiApplication::layoutDirection(), ui.textDirection());
        QCOMPARE(translator.translate("", "Open"), open);
    }

private slots:
    void regionalDates_data() {
        QTest::addColumn<QByteArray>("all");
        QTest::addColumn<QByteArray>("time");
        QTest::addColumn<QString>("regional");
        QTest::newRow("unset") << QByteArray() << QByteArray() << QString();
        QTest::newRow("empty") << QByteArray("") << QByteArray("") << QString();
        QTest::newRow("portuguese") << QByteArray("") << QByteArray("pt_PT.UTF-8") << QString("pt_PT");
        QTest::newRow("all-precedes-time") << QByteArray("de_DE.UTF-8") << QByteArray("pt_PT.UTF-8") << QString("de_DE");
        QTest::newRow("invalid-time") << QByteArray() << QByteArray("not_a_locale") << QString();
        QTest::newRow("invalid-all") << QByteArray("not_a_locale") << QByteArray("pt_PT.UTF-8") << QString();
        QTest::newRow("C") << QByteArray() << QByteArray("C") << QString("C");
        QTest::newRow("POSIX") << QByteArray() << QByteArray("POSIX") << QString("C");
        QTest::newRow("C-utf8") << QByteArray("C.UTF-8") << QByteArray("pt_PT.UTF-8") << QString("C");
        QTest::newRow("POSIX-utf8") << QByteArray() << QByteArray("POSIX.UTF-8") << QString("C");
        QTest::newRow("serbian-latin") << QByteArray() << QByteArray("sr_Latn_RS") << QString("sr_Latn_RS");
        QTest::newRow("azerbaijani-cyrillic") << QByteArray() << QByteArray("az_Cyrl_AZ") << QString("az_Cyrl_AZ");
        QTest::newRow("registry-script") << QByteArray() << QByteArray() << QString();
    }

    void regionalDates() {
        QTest::failOnWarning(QRegularExpression(".*"));
        QFETCH(QByteArray, all);
        QFETCH(QByteArray, time);
        QFETCH(QString, regional);
        const QByteArray oldAll = qgetenv("LC_ALL"), oldTime = qgetenv("LC_TIME"), oldOmarchy = qgetenv("OMARCHY_PATH");
        const QLocale oldLocale;
        const auto oldDirection = QGuiApplication::layoutDirection();
        const auto restore = qScopeGuard([&] {
            for (const auto &entry : {qMakePair("LC_ALL", oldAll), qMakePair("LC_TIME", oldTime), qMakePair("OMARCHY_PATH", oldOmarchy)}) {
                if (entry.second.isNull()) qunsetenv(entry.first);
                else qputenv(entry.first, entry.second);
            }
            QLocale::setDefault(oldLocale);
            QGuiApplication::setLayoutDirection(oldDirection);
        });
        if (all.isNull()) qunsetenv("LC_ALL"); else qputenv("LC_ALL", all);
        if (time.isNull()) qunsetenv("LC_TIME"); else qputenv("LC_TIME", time);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("OMARCHY_PATH", QFile::encodeName(dir.path()));
        QVERIFY(QDir().mkpath(dir.filePath("default/i18n")));
        // The real Omarchy registry deliberately uses Western digits for Arabic.
        write(dir.filePath("default/i18n/locales.json"), R"({"en":{"formatLocale":"en_US"},"pt":{"formatLocale":"pt_PT"},"ar":{"formatLocale":"ar_AE"},"fa":{"formatLocale":"fa_IR"},"sr":{"formatLocale":"sr_Latn_RS"}})");
        write(dir.filePath("en.json"), R"({"Open":"Open"})");
        write(dir.filePath("pt.json"), R"({"Open":"Abrir"})");
        write(dir.filePath("ar.json"), R"({"Open":"افتح"})");
        write(dir.filePath("fa.json"), R"({"Open":"باز کردن"})");
        write(dir.filePath("sr.json"), R"({"Open":"Otvori"})");
        write(dir.filePath("language"), "en\n");
        Translator translator(dir.filePath("language"), dir.path());
        QQmlEngine engine;
        engine.addImageProvider("thumb", new BlankThumbnail);
        connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
        engine.rootContext()->setContextProperty("translator", &translator);
        QVariantMap theme;
        for (const auto &name : {"background", "darkBackground", "foreground", "brightForeground", "secondaryForeground", "accent", "accentForeground", "red", "blue", "magenta", "orange", "yellow", "cyan"})
            theme.insert(name, QColor("#888888"));
        theme.insert("radius", 0);
        engine.rootContext()->setContextProperty("theme", theme);
        engine.rootContext()->setContextProperty("places", QVariantMap{{"devices", QVariantList{}}, {"home", dir.path()}});
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
Item {
    id: fixture
    property date stamp: new Date(2001, 10, 9, 14, 7)
    property bool trash: false
    // Exercise Qt's QML locale parser with month/day names, not only digits.
    function longDate(localeName) { return stamp.toLocaleDateString(Qt.locale(localeName), Locale.LongFormat) }
    property var backend: QtObject {
        property var details: fixture.trash ? ({origin: "/old/sample", deleted: fixture.stamp}) : ({})
        function describe() {}
        function typeName() { return "Text" }
        function applications() { return [] }
    }
    FileRow {
        objectName: "row"; width: 600; detailed: true
        index: 0; name: "sample"; path: ""; isDir: false; kind: "text"; mime: "text/plain"
        extension: "txt"; size: 1500; modified: parent.stamp; itemCount: 0; icon: ""; selected: false
    }
    FileTile {
        objectName: "tile"; width: 200; height: 200
        index: 0; name: "sample"; path: ""; isDir: false; kind: "text"; mime: "text/plain"
        extension: "txt"; size: 1500; modified: parent.stamp; itemCount: 0; icon: ""; selected: false
    }
    PreviewPane {
        objectName: "details"; width: 400; height: 700
        folderPane: QtObject {
            property var current: ({path: "", name: "sample", kind: "text", mime: "text/plain", modified: fixture.trash ? new Date(1999, 0, 1) : fixture.stamp})
            property bool inTrash: fixture.trash
            property var paneBackend: ({path: ""})
            property var paneFileOps: QtObject {
                function appsFor() { return [] }
                function defaultAppName() { return "" }
            }
        }
    }
}
)", QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js") + ".test.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        // Stable semantic selectors avoid child order, styling and unrelated text.
        auto *rowDate = root->findChild<QQuickItem *>("dateLabel");
        auto *rowSize = root->findChild<QQuickItem *>("sizeLabel");
        auto *gridMetadata = root->findChild<QQuickItem *>("metadataLabel");
        auto *detailsTimestamp = root->findChild<QQuickItem *>("timestampLabel");
        QVERIFY(rowDate && rowSize && gridMetadata && detailsTimestamp);
        const QHash<QString, QString> locales{{"en", "en_US"}, {"pt", "pt_PT"}, {"fa", "fa_IR"}, {"ar", "ar_AE"}, {"sr", "sr_Latn_RS"}};
        const QHash<QString, QString> open{{"en", "Open"}, {"pt", "Abrir"}, {"fa", "باز کردن"}, {"ar", "افتح"}, {"sr", "Otvori"}};
        const QDateTime oldDate(QDate(2001, 11, 9), QTime(14, 7));
        for (const QString &language : {QString("en"), QString("pt"), QString("fa"), QString("ar"), QString("sr"), QString("en")}) {
            write(dir.filePath("language"), language.toUtf8() + '\n');
            QTRY_COMPARE(translator.language(), language);
            QLocale ui(locales.value(language));
            ui.setNumberOptions(QLocale::OmitGroupSeparator);
            const QLocale dateLocale = regional.isEmpty() ? ui : QLocale(regional);
            verifyUiLocale(translator, ui, open.value(language));
            for (const auto &stamp : {oldDate, QDateTime(QDate::currentDate(), QTime(14, 7))}) {
                root->setProperty("stamp", stamp);
                const QString when = stamp.date() == QDate::currentDate() ? dateLocale.toString(stamp.time(), QLocale::ShortFormat) : dateLocale.toString(stamp.date(), QLocale::ShortFormat);
                const QString size = ui.toString(1.5, 'f', 1) + " KB";
                const QString timestamp = dateLocale.toString(stamp, QLocale::ShortFormat);
                QTRY_COMPARE(rowDate->property("text").toString(), when);
                QTRY_COMPARE(gridMetadata->property("text").toString(), size + " · " + when);
                QTRY_COMPARE(detailsTimestamp->property("text").toString(), timestamp);
                QCOMPARE(rowSize->property("text").toString(), size);
                QVariant longDate;
                QVERIFY(QMetaObject::invokeMethod(root.get(), "longDate", Q_RETURN_ARG(QVariant, longDate),
                                                  Q_ARG(QVariant, translator.property("dateLocale"))));
                QCOMPARE(longDate.toString(), dateLocale.toString(stamp.date(), QLocale::LongFormat));
                for (bool trash : {false, true}) {
                    root->setProperty("trash", trash);
                    QTRY_COMPARE(detailsTimestamp->property("text").toString(), timestamp);
                }
                root->setProperty("trash", false);
            }
        }
    }
};

QTEST_MAIN(LocaleTests)
#include "locale_tests.moc"
