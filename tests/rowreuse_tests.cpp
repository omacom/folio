#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <memory>
#include "backend.h"
#include "fileops.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

static QString sources() { return QFileInfo(QFINDTESTDATA("../src/Format.js")).absolutePath(); }
static void writeFile(const QString &path, const QByteArray &bytes = "fixture") {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Cannot write fixture");
}

// Only opening another program is fenced. The folder, selection and file
// operations exercised by the QML are the real ones.
class TestBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(FolderModel *folder READ folder CONSTANT)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
public:
    explicit TestBackend(const QString &home) : backend(home) {
        connect(&backend, &Backend::pathChanged, this, &TestBackend::pathChanged);
    }
    FolderModel *folder() { return backend.folder(); }
    QString path() const { return backend.path(); }
    QStringList opened;
    Q_INVOKABLE void open(const QString &path) { opened << path; }
    Q_INVOKABLE void navigate(const QString &path) { backend.navigate(path); }
    Q_INVOKABLE void goUp() { backend.goUp(); }
signals:
    void pathChanged();
private:
    Backend backend;
};

struct LocaleGuard {
    QLocale before;
    ~LocaleGuard() { QLocale::setDefault(before); }
};

class ListFixture {
public:
    LocaleGuard locale;
    QTemporaryDir home;
    TestBackend backend{home.path()};
    Theme theme{home.filePath("theme")};
    FileOps ops;
    Translator translator{home.filePath("language"), sources() + "/i18n"};
    std::unique_ptr<QQmlPropertyMap> plugins{QQmlPropertyMap::create()};
    Thumbnails thumbnails{home.filePath("cache"), {home.filePath("none")}};
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickItem *list = nullptr;
    QString error;

    ListFixture() {
        QDir().mkpath(folder());
        for (int i = 0; i < 400; ++i) writeFile(folder() + QString("/file%1.txt").arg(i, 3, 10, QChar('0')));
        plugins->insert("views", QVariantMap{});
        auto *context = engine.rootContext();
        context->setContextProperty("backend", &backend);
        context->setContextProperty("theme", &theme);
        context->setContextProperty("fileOps", &ops);
        context->setContextProperty("translator", &translator);
        context->setContextProperty("plugins", plugins.get());
        engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
    }
    ~ListFixture() {
        root.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThreadPool::globalInstance()->waitForDone();
    }
    QString folder() const { return home.filePath("folder"); }
    QQuickWindow *window() const { return qobject_cast<QQuickWindow *>(root.get()); }
    bool show(bool mirrored = false) {
        backend.navigate(folder());
        if (!QTest::qWaitFor([&] { return !backend.folder()->loading(); })) return false;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nWindow { width: 900; height: 480; visible: true\n"
                          " Item { anchors.fill: parent; LayoutMirroring.enabled: " + QByteArray(mirrored ? "true" : "false") +
                          "; LayoutMirroring.childrenInherit: true; FolderList { objectName: 'list'; anchors.fill: parent } } }",
                          QUrl::fromLocalFile(sources() + "/RowReuseTest.qml"));
        root.reset(component.create());
        error = component.errorString();
        if (!root) return false;
        list = root->findChild<QQuickItem *>("list");
        return list && QTest::qWaitForWindowExposed(window()) && QTest::qWaitFor([&] { return item(4) != nullptr; });
    }
    QQuickItem *item(int index) const {
        QQuickItem *found = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, found), Q_ARG(int, index));
        return found;
    }
    void move(int index) {
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0));
        QMetaObject::invokeMethod(list, "forceLayout");
    }
    QVariant eval(QObject *object, const QString &code) {
        QQmlExpression expression(qmlContext(object), object, code);
        QVariant result = expression.evaluate();
        if (expression.hasError()) error = expression.error().toString();
        return result;
    }
    QPoint point(QQuickItem *item) const {
        return item->mapToItem(window()->contentItem(), QPointF(100, item->height() / 2)).toPoint();
    }
};

static void clickAt(QQuickWindow *window, const QPoint &at, ulong timestamp) {
    const QPointF global = window->mapToGlobal(at);
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    press.setTimestamp(timestamp);
    QGuiApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    release.setTimestamp(timestamp + 1);
    QGuiApplication::sendEvent(window, &release);
}

class RowReuseTests : public QObject {
    Q_OBJECT
private slots:
    void reusesRowsWithoutOldDragAssets_data() {
        QTest::addColumn<bool>("mirrored");
        QTest::newRow("ltr") << false;
        QTest::newRow("rtl") << true;
    }
    void reusesRowsWithoutOldDragAssets() {
        QFETCH(bool, mirrored);
        ListFixture f;
        QVERIFY2(f.show(mirrored), qPrintable(f.error));
        QPointer<QQuickItem> row = f.item(4);
        QVERIFY(row);
        const QString original = row->property("path").toString();
        f.eval(row, "Drag.imageSource = 'file:///old-drag.png'; Drag.mimeData = {'text/uri-list':'file:///old-file.txt'}");
        f.move(120); f.move(240);
        QTRY_VERIFY(row && row->property("path").toString() != original);
        QCOMPARE(f.eval(row, "String(Drag.imageSource)").toString(), QString());
        QVERIFY(f.eval(row, "Object.keys(Drag.mimeData).length === 0").toBool());
        QVERIFY(f.error.isEmpty());
    }

    void doesNotTurnTheFirstClickOnAReusedFileIntoADoubleClick() {
        ListFixture f;
        QVERIFY2(f.show(), qPrintable(f.error));
        QPointer<QQuickItem> row = f.item(4);
        const QString original = row->property("path").toString();
        const QPoint at = f.point(row);
        clickAt(f.window(), at, 1000);
        QCOMPARE(f.backend.folder()->selectedPaths(), QStringList{original});
        // Another row becomes current, making the first one reusable.
        clickAt(f.window(), f.point(f.item(0)), 1040);
        f.move(120); f.move(240);
        QTRY_VERIFY(row && row->property("path").toString() != original);
        f.move(row->property("index").toInt() - 4);
        QTRY_COMPARE(f.point(row), at);
        const QString reused = row->property("path").toString();
        clickAt(f.window(), at, 1100);
        QCOMPARE(f.backend.folder()->selectedPaths(), QStringList{reused});
        QVERIFY2(f.backend.opened.isEmpty(), "First click on the new file activated it");
        clickAt(f.window(), at, 1150);
        QCOMPARE(f.backend.opened, QStringList{reused});
    }

    void releasingAfterScrollDoesNotClickAnotherFile() {
        ListFixture f;
        QVERIFY2(f.show(), qPrintable(f.error));
        const QPoint at = f.point(f.item(4));
        const QStringList selected = f.backend.folder()->selectedPaths();
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, at);
        f.move(120); f.move(240);
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(f.backend.folder()->selectedPaths(), selected);
        QVERIFY(f.backend.opened.isEmpty());
    }

    void dragPausesReuseUntilItEnds() {
        ListFixture f;
        QVERIFY2(f.show(), qPrintable(f.error));
        QPointer<QQuickItem> row = f.item(4);
        const QString path = row->property("path").toString();
        f.eval(row, "Drag.dragType = Drag.Internal; Drag.active = true");
        QVERIFY(!f.list->property("reuseItems").toBool());
        f.move(120); f.move(240);
        QVERIFY(!row || row->property("path").toString() == path);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (row) f.eval(row, "Drag.active = false");
        QTRY_VERIFY(f.list->property("reuseItems").toBool());
        QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    }

    void delayedPicturesRespectReuseAndDestruction() {
        ListFixture f;
        QQmlComponent component(&f.engine);
        component.setData(R"(import QtQuick
Window {
    id: host
    width: 900; height: 480; visible: true
    property var callbacks: []
    function take() { return callbacks.shift() }
    FileRow {
        objectName: "row"; width: 900
        index: 0; name: "a"; path: "/a"; isDir: false; kind: "text"; mime: "text/plain"
        extension: "TXT"; size: 1; modified: new Date(0); itemCount: -1; icon: ""; selected: false
        function grabToImage(callback) { host.callbacks.push(callback); return true }
        function pool() { ListView.pooled() }
        function reuse() { ListView.reused() }
    }
})", QUrl::fromLocalFile(sources() + "/DelayedRowTest.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QPointer<QObject> row = root->findChild<QObject *>("row");
        QVERIFY(row);
        QVERIFY(f.eval(row, "captureDragPicture()").toBool());
        QVariant callback;
        QVERIFY(QMetaObject::invokeMethod(root.get(), "take", Q_RETURN_ARG(QVariant, callback)));
        QJSValue old = callback.value<QJSValue>();
        QVERIFY(old.isCallable());
        f.eval(row, "pool()"); row->setProperty("path", "/b"); f.eval(row, "reuse()");
        f.eval(row, "pool()"); row->setProperty("path", "/a"); f.eval(row, "reuse()");
        QJSValue picture = f.engine.newObject(); picture.setProperty("url", "file:///late.png");
        QVERIFY(!old.call({picture}).isError());
        QCOMPARE(f.eval(row, "String(Drag.imageSource)").toString(), QString());
        // The current request still succeeds: rejecting stale ones must not
        // disable drag pictures altogether.
        QVERIFY(f.eval(row, "captureDragPicture()").toBool());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "take", Q_RETURN_ARG(QVariant, callback)));
        QJSValue current = callback.value<QJSValue>();
        QVERIFY(current.isCallable());
        QVERIFY(!current.call({picture}).isError());
        QCOMPARE(f.eval(row, "String(Drag.imageSource)").toString(), QString("file:///late.png"));
        QVERIFY(f.eval(row, "captureDragPicture()").toBool());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "take", Q_RETURN_ARG(QVariant, callback)));
        QJSValue pending = callback.value<QJSValue>();
        QVERIFY(pending.isCallable());
        root.reset();
        QVERIFY(row.isNull());
        QVERIFY(!pending.call({picture}).isError());
        QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    }

    void closesWithRealGrabPending() {
        ListFixture f;
        QVERIFY2(f.show(), qPrintable(f.error));
        QPointer<QQuickItem> row = f.item(4);
        QVERIFY(f.eval(row, "captureDragPicture()").toBool());
        f.root.reset();
        QVERIFY(row.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
};

QTEST_MAIN(RowReuseTests)
#include "rowreuse_tests.moc"
