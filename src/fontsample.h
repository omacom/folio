#pragma once

#include <QByteArray>
#include <QColor>
#include <QGlyphRun>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QImage>
#include <QQuickPaintedItem>
#include <QRawFont>
#include <QString>
#include <QVariant>
#include <atomic>
#include <memory>

// A font file as a sample sheet: read off the UI thread and drawn from its own bytes, never added to Folio's fonts.
class FontSample : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    // The file's modified time, or anything that tells one writing of it from the next: a change reads it again.
    Q_PROPERTY(QVariant modified READ modified WRITE setModified NOTIFY modifiedChanged)
    // Empty for a file that isn't a font, then loading, ready or error.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    // The family's name and its first face's style, as the file gives them.
    Q_PROPERTY(QString family READ family NOTIFY changed)
    Q_PROPERTY(QString style READ style NOTIFY changed)
    Q_PROPERTY(int faces READ faces NOTIFY changed)
    // Characters it has, drawn in place of the alphabet, digits and pangram; empty when it has all of those.
    Q_PROPERTY(QString glyphs READ glyphs NOTIFY changed)

public:
    explicit FontSample(QObject *parent = nullptr);
    ~FontSample() override;

    QString path() const { return m_path; }
    void setPath(const QString &path);
    QVariant modified() const { return m_modified; }
    void setModified(const QVariant &modified);
    QString status() const { return m_status; }
    QString family() const { return m_family; }
    QString style() const { return m_style; }
    int faces() const { return m_faces; }
    QString glyphs() const { return m_glyphs; }
    // The file's first face at a size; not valid unless ready.
    QRawFont font(qreal pixelSize) const;

    // TrueType, OpenType, collections, WOFF and Type 1, by their current or older names.
    static bool isFont(const QString &mime);
    // Whether a name could be a font's; by extension alone ".otf" is an OpenDocument template.
    static bool isFontFile(const QString &path);
    // The font type a file's name could mean, or `mime` when it's one already or the name means none.
    static QString typeOf(const QString &mime, const QString &path);
    // The faces in a font collection; 1 for a single font.
    static int faceCount(const QByteArray &data);

signals:
    void pathChanged();
    void modifiedChanged();
    void changed();

private:
    void show();
    void load(quint64 generation);
    void unload();

    QString m_path;
    QVariant m_modified;
    QString m_status;
    QString m_family;
    QString m_style;
    int m_faces = 0;
    QString m_glyphs;
    // The file's bytes while it's shown.
    QByteArray m_data;
    // Counts what was asked for; a reading goes on, and is kept, only while it's still the newest. Shared with the readings under way.
    std::shared_ptr<std::atomic<quint64>> m_shown = std::make_shared<std::atomic<quint64>>(0);
};

// Text drawn in a sample's own font: two files of one family name each draw as themselves.
class FontText : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(FontSample *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(qreal pixelSize READ pixelSize WRITE setPixelSize NOTIFY pixelSizeChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // Breaks a line at any character, for a sheet of characters; otherwise between words where it can.
    Q_PROPERTY(bool anywhere READ anywhere WRITE setAnywhere NOTIFY anywhereChanged)

public:
    explicit FontText(QQuickItem *parent = nullptr);

    FontSample *source() const { return m_source; }
    void setSource(FontSample *source);
    QString text() const { return m_text; }
    void setText(const QString &text);
    qreal pixelSize() const { return m_pixelSize; }
    void setPixelSize(qreal pixelSize);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    bool anywhere() const { return m_anywhere; }
    void setAnywhere(bool anywhere);

    // What's drawn: the glyphs of the source's own font, line by line.
    QList<QGlyphRun> runs() const { return m_runs; }
    // Those glyphs as a picture, made on the thread that owns the font: paint() runs on the render thread and only copies it.
    QImage picture() const { return m_picture; }
    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void textChanged();
    void pixelSizeChanged();
    void colorChanged();
    void anywhereChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void load();
    void lay();
    void draw();

    QPointer<FontSample> m_source;
    QString m_text;
    qreal m_pixelSize = 13;
    QColor m_color;
    bool m_anywhere = false;
    // Loaded once for a source and a size: a window being resized only lays the lines again.
    QRawFont m_font;
    QList<QGlyphRun> m_runs;
    QImage m_picture;
};
