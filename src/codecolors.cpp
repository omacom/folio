#include "codecolors.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTextBlock>
#include <QTextDocument>

namespace {

// How often a run of bat is looked at, and the most it may print for each byte it was given: a color around every character is ten.
constexpr int lookEveryMs = 25, mostPrintedPerByte = 16;

// The ink an SGR number sets, or -1 for one that isn't a foreground: 31 to 36 and their bright 91 to 96 are the six, and 0, 39, black and white are the text's own.
int inkOf(int sgr) {
    if (sgr == 0 || sgr == 39 || sgr == 30 || sgr == 37 || sgr == 90 || sgr == 97)
        return 0;
    if (sgr >= 31 && sgr <= 36)
        return sgr - 30;
    if (sgr >= 91 && sgr <= 96)
        return sgr - 90;
    return -1;
}

} // namespace

CodeColors::CodeColors(QObject *parent) : QSyntaxHighlighter(parent) {}

// Sample input: "\x1b[35mint\x1b[0m \x1b[34mmain\x1b[0m() {}\n" is one line, "int main() {}", with int in ink 5 and main in ink 4.
CodeColors::Lines CodeColors::parse(const QString &printed) {
    Lines lines;
    Line line;
    int ink = 0;
    int from = 0;
    const auto close = [&] {
        if (ink > 0 && line.text.size() > from)
            line.runs.append({from, int(line.text.size()) - from, ink});
        from = line.text.size();
    };
    for (qsizetype at = 0; at < printed.size(); ++at) {
        const QChar ch = printed.at(at);
        if (ch == u'\x1b' && at + 1 < printed.size() && printed.at(at + 1) == u'[') {
            // A control sequence ends at its first letter; only "m", the colors, is read.
            qsizetype end = at + 2;
            while (end < printed.size() && !printed.at(end).isLetter())
                ++end;
            if (end < printed.size() && printed.at(end) == u'm') {
                const auto numbers = QStringView(printed).mid(at + 2, end - at - 2).split(u';');
                for (const QStringView number : numbers) {
                    const int sgr = number.isEmpty() ? 0 : number.toInt();
                    // 38 and 48 are colors by number, with numbers of their own after them: none of the six.
                    if (sgr == 38 || sgr == 48)
                        break;
                    if (const int next = inkOf(sgr); next >= 0 && next != ink) {
                        close();
                        ink = next;
                    }
                }
            }
            at = end;
        } else if (ch == u'\n') {
            close();
            lines.append(line);
            line = {};
            from = 0;
        } else {
            line.text.append(ch);
        }
    }
    close();
    if (!line.text.isEmpty())
        lines.append(line);
    return lines;
}

CodeColors::Lines CodeColors::of(const QString &text, const QString &name, int limitMs, const std::function<bool()> &wanted) {
    QElapsedTimer run;
    run.start();
    QProcess bat;
    bat.setStandardErrorFile(QProcess::nullDevice());
    // The user's bat settings are left out: a pager, line numbers or a theme of their own would not be the text.
    bat.start("bat", {"--no-config", "--no-custom-assets", "--theme=ansi", "--color=always", "--style=plain", "--paging=never",
                      "--wrap=never", "--tabs=0", "--file-name=" + name});
    if (!bat.waitForStarted(limitMs))
        return {};
    const QByteArray sent = text.toUtf8();
    bat.write(sent);
    bat.closeWriteChannel();
    // More than colors take, whether it is printed without end or all at once by a program that then ends.
    const auto tooMuch = [&] { return bat.bytesAvailable() > sent.size() * mostPrintedPerByte; };
    // The start and the run share the limit, and neither colors nobody will draw nor a program that prints without end are waited for.
    while (!bat.waitForFinished(lookEveryMs) && bat.state() != QProcess::NotRunning) {
        if (run.hasExpired(limitMs) || tooMuch() || (wanted && !wanted())) {
            bat.kill();
            bat.waitForFinished();
            return {};
        }
    }
    if (bat.exitStatus() != QProcess::NormalExit || bat.exitCode() != 0 || tooMuch())
        return {};
    return parse(QString::fromUtf8(bat.readAllStandardOutput()));
}

QVariantList CodeColors::runsOf(const Lines &lines, const QString &text) {
    QStringList texts;
    QVariantList all;
    bool colored = false;
    for (const Line &line : lines) {
        texts << line.text;
        QVariantList runs;
        for (const Run &run : line.runs)
            runs << QVariant(QVariantList{run.start, run.length, run.ink});
        colored = colored || !runs.isEmpty();
        all << QVariant(runs);
    }
    // bat ends a last line that had no end, and nothing else may differ.
    const QString read = texts.join(u'\n');
    return colored && (read == text || read + u'\n' == text) ? all : QVariantList();
}

void CodeColors::setTextDocument(QQuickTextDocument *textDocument) {
    if (textDocument == m_textDocument)
        return;
    m_textDocument = textDocument;
    setDocument(textDocument ? textDocument->textDocument() : nullptr);
    emit textDocumentChanged();
}

void CodeColors::setLines(const QVariant &lines) {
    const Lines next = lines.value<Lines>();
    if (next == m_lines)
        return;
    m_lines = next;
    repaint();
    emit linesChanged();
}

void CodeColors::setInks(const QVariantList &inks) {
    if (inks == m_inks)
        return;
    m_inks = inks;
    repaint();
    emit inksChanged();
}

QList<QTextLayout::FormatRange> CodeColors::formatsOf(int number, const QString &text) const {
    QList<QTextLayout::FormatRange> formats;
    if (number >= m_lines.size())
        return formats;
    const Line &line = m_lines.at(number);
    // Colored only where the document's line is the line bat saw: a text that has moved on, or breaks its lines elsewhere, stays plain.
    if (line.text != text && !(line.text.endsWith(u'\r') && QStringView(line.text).chopped(1) == text))
        return formats;
    for (const Run &run : line.runs) {
        // A run that took in the line's own end stops where the document's line does.
        const int length = qMin(run.length, int(text.size()) - run.start);
        if (run.ink > m_inks.size() || length <= 0)
            continue;
        QTextLayout::FormatRange range;
        range.start = run.start;
        range.length = length;
        range.format.setForeground(m_inks.at(run.ink - 1).value<QColor>());
        formats.append(range);
    }
    return formats;
}

// A text that is set: each of its lines as the document reads it.
void CodeColors::highlightBlock(const QString &text) {
    const auto formats = formatsOf(currentBlock().blockNumber(), text);
    for (const QTextLayout::FormatRange &range : formats)
        setFormat(range.start, range.length, range.format);
}

// Colors that come for a text already shown: every line's at once and the document told once, where rehighlight() tells it line by line, half a second for a long text.
void CodeColors::repaint() {
    QTextDocument *shown = document();
    if (!shown)
        return;
    for (QTextBlock block = shown->begin(); block.isValid(); block = block.next())
        block.layout()->setFormats(formatsOf(block.blockNumber(), block.text()));
    shown->markContentsDirty(0, shown->characterCount());
}
