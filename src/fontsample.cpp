#include "fontsample.h"

#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QPainter>
#include <QQuickWindow>
#include <QTextLayout>
#include <QTextOption>
#include <QtConcurrent>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Past the largest CJK collections; a bigger file isn't read.
constexpr qint64 fontBytes = 128 * 1024 * 1024;
// As many characters as the alphabet in both cases and the digits.
constexpr int glyphCount = 62;
// QRawFont needs a size, though it's only asked which characters it has.
constexpr qreal probePixels = 12;
// After the space: controls and the space itself show nothing.
constexpr char32_t firstShown = 0x21;
constexpr char32_t lastCodePoint = 0x10FFFF;
// A collection's header: "ttcf", its version, how many faces, then a 4-byte offset per face.
constexpr int faceCountAt = 8;
constexpr int collectionHeader = 12;
constexpr int faceOffsetBytes = 4;

struct Read {
    QByteArray data;
    QString family;
    QString style;
    int faces = 0;
    QString glyphs;
};

bool showsAlone(char32_t c) {
    switch (QChar::category(c)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_NotAssigned:
        return false;
    default:
        return true;
    }
}

// The first characters by code point the font has that `take` accepts, for as long as they're `wanted`.
QString glyphsIn(const QRawFont &font, bool (*take)(char32_t), const std::function<bool()> &wanted) {
    QString glyphs;
    int found = 0;
    for (char32_t c = firstShown; c <= lastCodePoint && found < glyphCount && wanted(); ++c) {
        if (take(c) && font.supportsCharacter(c)) {
            glyphs += QString::fromUcs4(&c, 1);
            ++found;
        }
    }
    return glyphs;
}

// Runs off the UI thread: the file is read and checked here.
Read readFont(const QString &path, quint64 generation, const std::shared_ptr<std::atomic<quint64>> &shown) {
    Read read;
    // Asked before the disk is, before the font is searched and at each character of that search: a font stepped past costs little.
    const auto current = [&] { return shown->load() == generation; };
    if (!current())
        return read;
    // Opened without waiting and read only as far as its size: a pipe or a device named like a font gives nothing and holds no thread.
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    QFile file;
    if (fd < 0 || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        if (fd >= 0)
            ::close(fd);
        return read;
    }
    struct stat info;
    if (::fstat(fd, &info) != 0 || info.st_size > fontBytes)
        return read;
    const QByteArray data = file.read(info.st_size);
    if (!current())
        return read;
    const QRawFont font(data, probePixels);
    if (!font.isValid())
        return read;
    read.data = data;
    read.family = font.familyName();
    read.style = font.styleName();
    read.faces = FontSample::faceCount(data);
    // The sheet's own characters: the alphabet in both cases and the digits.
    bool sheet = true;
    for (char32_t c = 'A'; c <= 'Z' && sheet; ++c)
        sheet = font.supportsCharacter(c) && font.supportsCharacter(c - 'A' + 'a');
    for (char32_t c = '0'; c <= '9' && sheet; ++c)
        sheet = font.supportsCharacter(c);
    // The line of text has spaces between its words.
    sheet = sheet && font.supportsCharacter(' ');
    // Without all of them: letters it has (Greek, kana, an alphabet with no digits), else any it draws, never a fallback font's.
    if (!sheet) {
        read.glyphs = glyphsIn(font, [](char32_t c) { return QChar::isLetter(c); }, current);
        if (read.glyphs.isEmpty())
            read.glyphs = glyphsIn(font, showsAlone, current);
        // Nothing it draws alone (only marks, or nothing at all): no sample, as for a file that isn't a font.
        if (read.glyphs.isEmpty())
            return {};
    }
    return read;
}

}

FontSample::FontSample(QObject *parent) : QObject(parent) {}

FontSample::~FontSample() {
    unload();
    // A reading under way stops at its next look.
    ++*m_shown;
}

bool FontSample::isFont(const QString &mime) {
    static const QStringList fonts{"font/ttf", "font/otf", "font/collection", "font/woff", "font/woff2",
                                   "application/x-font-type1"};
    // Resolves the older names (application/x-font-ttf) to the current ones.
    return fonts.contains(QMimeDatabase().mimeTypeForName(mime).name());
}

bool FontSample::isFontFile(const QString &path) {
    return isFont(typeOf({}, path));
}

QString FontSample::typeOf(const QString &mime, const QString &path) {
    if (isFont(mime))
        return mime;
    for (const QMimeType &type : QMimeDatabase().mimeTypesForFileName(path))
        if (isFont(type.name()))
            return type.name();
    return mime;
}

// Sample: 74 74 63 66 | 00 01 00 00 | 00 00 00 02 | an offset per face is "ttcf", version 1.0, two faces.
int FontSample::faceCount(const QByteArray &data) {
    if (!data.startsWith("ttcf") || data.size() < collectionHeader)
        return 1;
    const quint32 faces = qFromBigEndian<quint32>(data.constData() + faceCountAt);
    return faces > 0 && faces <= quint32(data.size() - collectionHeader) / faceOffsetBytes ? int(faces) : 1;
}

void FontSample::setPath(const QString &path) {
    if (path == m_path)
        return;
    m_path = path;
    emit pathChanged();
    show();
}

void FontSample::setModified(const QVariant &modified) {
    if (modified == m_modified)
        return;
    m_modified = modified;
    emit modifiedChanged();
    // Only a font being shown is read again: the file was written over in place.
    if (!m_status.isEmpty())
        show();
}

void FontSample::show() {
    unload();
    const quint64 generation = ++*m_shown;
    m_status = !m_path.isEmpty() && isFontFile(m_path) ? QStringLiteral("loading") : QString();
    emit changed();
    if (m_status.isEmpty())
        return;
    // Read once the loop turns: a selection that moves sets the path and the time one after the other, and one reading serves both.
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == *m_shown)
            load(generation);
    }, Qt::QueuedConnection);
}

void FontSample::load(quint64 generation) {
    auto *watcher = new QFutureWatcher<Read>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != *m_shown)
            return; // Stepped on to another file while this one was read.
        const Read read = watcher->result();
        if (read.data.isEmpty()) {
            m_status = "error";
            emit changed();
            return;
        }
        // Kept, not added to Folio's fonts: Qt has one list of those, where another window's file of the same name would take this one's place.
        m_data = read.data;
        m_family = read.family;
        m_style = read.style;
        m_faces = read.faces;
        m_glyphs = read.glyphs;
        m_status = "ready";
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run(readFont, m_path, generation, m_shown));
}

QRawFont FontSample::font(qreal pixelSize) const {
    return m_data.isEmpty() ? QRawFont() : QRawFont(m_data, pixelSize);
}

void FontSample::unload() {
    m_data.clear();
    m_status.clear();
    m_family.clear();
    m_style.clear();
    m_faces = 0;
    m_glyphs.clear();
}

FontText::FontText(QQuickItem *parent) : QQuickPaintedItem(parent) {
    connect(this, &QQuickItem::widthChanged, this, &FontText::lay);
}

void FontText::setSource(FontSample *source) {
    if (source == m_source)
        return;
    if (m_source)
        disconnect(m_source, &FontSample::changed, this, &FontText::load);
    m_source = source;
    if (m_source)
        connect(m_source, &FontSample::changed, this, &FontText::load);
    emit sourceChanged();
    load();
}

void FontText::setText(const QString &text) {
    if (text == m_text)
        return;
    m_text = text;
    emit textChanged();
    lay();
}

void FontText::setPixelSize(qreal pixelSize) {
    if (pixelSize == m_pixelSize)
        return;
    m_pixelSize = pixelSize;
    emit pixelSizeChanged();
    load();
}

void FontText::setColor(const QColor &color) {
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    draw();
}

void FontText::setAnywhere(bool anywhere) {
    if (anywhere == m_anywhere)
        return;
    m_anywhere = anywhere;
    emit anywhereChanged();
    lay();
}

void FontText::load() {
    m_font = m_source && m_pixelSize > 0 ? m_source->font(m_pixelSize) : QRawFont();
    lay();
}

void FontText::lay() {
    m_runs.clear();
    qreal height = 0;
    if (m_font.isValid() && width() > 0 && !m_text.isEmpty()) {
        // One paragraph to the layout: a new line in the text is a line separator in it.
        QTextLayout layout(QString(m_text).replace(QChar('\n'), QChar(QChar::LineSeparator)));
        // Shaped with this font alone, so a character it lacks is its own missing glyph, never another font's.
        layout.setRawFont(m_font);
        QTextOption option;
        option.setWrapMode(m_anywhere ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(width());
            line.setPosition({0, height});
            height += line.height();
        }
        layout.endLayout();
        m_runs = layout.glyphRuns();
    }
    setImplicitHeight(std::ceil(height));
    draw();
}

void FontText::draw() {
    // A QRawFont and the glyph runs made with it belong to this thread, so they're drawn here and never in paint().
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : qGuiApp->devicePixelRatio();
    const QSize size = (QSizeF(width(), implicitHeight()) * ratio).toSize();
    m_picture = QImage();
    if (!m_runs.isEmpty() && !size.isEmpty()) {
        m_picture = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m_picture.setDevicePixelRatio(ratio);
        m_picture.fill(Qt::transparent);
        QPainter painter(&m_picture);
        painter.setPen(m_color);
        painter.setRenderHint(QPainter::TextAntialiasing);
        for (const QGlyphRun &run : std::as_const(m_runs))
            painter.drawGlyphRun({}, run);
    }
    update();
}

void FontText::itemChange(ItemChange change, const ItemChangeData &value) {
    // A window of another scale needs a picture of its own.
    if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange)
        draw();
    QQuickPaintedItem::itemChange(change, value);
}

void FontText::paint(QPainter *painter) {
    painter->drawImage(QPointF(0, 0), m_picture);
}
