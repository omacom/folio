#pragma once

#include <QColor>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QTextLayout>
#include <QVariant>
#include <QVector>
#include <functional>

// Draws a source text in color, as bat reads it: Omarchy ships bat, and its "ansi" theme names six terminal colors, drawn here in the theme's own.
class CodeColors : public QSyntaxHighlighter {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged)
    Q_PROPERTY(QVariant lines READ lines WRITE setLines NOTIFY linesChanged)
    Q_PROPERTY(QVariantList inks READ inks WRITE setInks NOTIFY inksChanged)

public:
    // A stretch of a line in one ink: 1 to 6 are red, green, yellow, blue, magenta and cyan.
    struct Run {
        int start = 0;
        int length = 0;
        int ink = 0;
        bool operator==(const Run &other) const { return start == other.start && length == other.length && ink == other.ink; }
    };
    // A line as bat printed it, without its escapes, and the runs that aren't the foreground.
    struct Line {
        QString text;
        QVector<Run> runs;
        bool operator==(const Line &other) const { return text == other.text && runs == other.runs; }
    };
    using Lines = QVector<Line>;

    explicit CodeColors(QObject *parent = nullptr);

    // Whether bat is asked about a text of this type: not about a plain text, nor about Markdown, which is shown as it always was.
    static bool isCode(const QString &mime) { return mime != "text/plain" && mime != "text/markdown"; }
    // What bat printed, line by line.
    static Lines parse(const QString &printed);
    // The colors of a text from a file of this name; none without bat, when it fails, takes longer than the limit or prints more than colors take, or once they aren't wanted.
    static Lines of(const QString &text, const QString &name, int limitMs, const std::function<bool()> &wanted = {});

    QQuickTextDocument *textDocument() const { return m_textDocument; }
    void setTextDocument(QQuickTextDocument *textDocument);
    QVariant lines() const { return QVariant::fromValue(m_lines); }
    void setLines(const QVariant &lines);
    QVariantList inks() const { return m_inks; }
    void setInks(const QVariantList &inks);

signals:
    void textDocumentChanged();
    void linesChanged();
    void inksChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    // The colors of one of the document's lines, none when it isn't the line bat read.
    QList<QTextLayout::FormatRange> formatsOf(int number, const QString &text) const;
    void repaint();

    QQuickTextDocument *m_textDocument = nullptr;
    Lines m_lines;
    QVariantList m_inks;
};

Q_DECLARE_METATYPE(CodeColors::Lines)
