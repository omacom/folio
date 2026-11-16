#include "panewidths.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QStringList>
#include <QtMath>
#include <algorithm>

#include "foldermodel.h"

namespace {

int clamped(int width, const PaneWidths::Limits &limits) {
    return std::clamp(width, limits.min, limits.max);
}

// The three ways, in the order W steps through them.
const QStringList &modes() {
    static const QStringList known{"fixed", "automatic", "proportional"};
    return known;
}

} // namespace

PaneWidths::PaneWidths(QObject *parent) : QObject(parent) {}

void PaneWidths::bump() {
    ++m_revision;
    emit changed();
}

void PaneWidths::setMode(const QString &mode) {
    const QString known = modes().contains(mode) ? mode : QStringLiteral("fixed");
    if (known == m_mode)
        return;
    m_mode = known;
    bump();
}

QString PaneWidths::stepMode(int by) {
    const int count = modes().size();
    const int at = modes().indexOf(m_mode);
    // A new way is for every pane: what was dragged lets go.
    m_sidebar = m_details = m_column = 0;
    m_split = 0;
    m_mode = modes().at(((at + by) % count + count) % count);
    bump();
    return m_mode;
}

void PaneWidths::setWindowWidth(int width) {
    if (width == m_windowWidth)
        return;
    m_windowWidth = width;
    // Only the proportional widths follow the window; the others would
    // re-read for nothing on every step of a resize.
    if (m_mode == QLatin1String("proportional"))
        bump();
}

// 0 (or less) lets go of a dragged width; anything else is held to the
// pane's limits.
static int keptWidth(int width, const PaneWidths::Limits &limits) {
    return width <= 0 ? 0 : clamped(width, limits);
}

void PaneWidths::setDraggedSidebar(int width) {
    width = keptWidth(width, SidebarLimits);
    if (width == m_sidebar)
        return;
    m_sidebar = width;
    bump();
}

void PaneWidths::setDraggedDetails(int width) {
    width = keptWidth(width, DetailsLimits);
    if (width == m_details)
        return;
    m_details = width;
    bump();
}

void PaneWidths::setDraggedColumn(int width) {
    width = keptWidth(width, ColumnLimits);
    if (width == m_column)
        return;
    m_column = width;
    bump();
}

void PaneWidths::setDraggedSplit(double share) {
    // A share from a hand-edited setting that isn't a number, or leaves a
    // half with nothing, is no share: the halves are even.
    if (!(share > 0 && share < 1))
        share = 0;
    if (share == m_split)
        return;
    m_split = share;
    bump();
}

// Each pane's share of the reference window, kept at the window's width.
int PaneWidths::scaled(const Limits &limits) const {
    return clamped(qRound(double(limits.fixed) * m_windowWidth / ReferenceWidth), limits);
}

int PaneWidths::modeWidth(const Limits &limits, int content) const {
    if (m_mode == QLatin1String("proportional"))
        return scaled(limits);
    if (m_mode == QLatin1String("automatic"))
        return clamped(content, limits);
    return limits.fixed;
}

int PaneWidths::sidebar(int content) const {
    return m_sidebar > 0 ? m_sidebar : modeWidth(SidebarLimits, content);
}

int PaneWidths::details() const {
    if (m_details > 0)
        return m_details;
    // The details show a picture and facts, not rows to fit: automatic
    // leaves them as they are.
    return m_mode == QLatin1String("proportional") ? scaled(DetailsLimits) : DetailsLimits.fixed;
}

int PaneWidths::column(int content) const {
    return m_column > 0 ? m_column : modeWidth(ColumnLimits, content);
}

int PaneWidths::splitLeft(int shared) const {
    // Halves not dragged, or too narrow to share out unevenly, stay even.
    if (m_split <= 0 || shared < 2 * MinSplitHalf)
        return shared / 2;
    return std::clamp(qRound(m_split * shared), MinSplitHalf, shared - MinSplitHalf);
}

double PaneWidths::dragged(const QString &pane) const {
    if (pane == QLatin1String("sidebar"))
        return m_sidebar;
    if (pane == QLatin1String("details"))
        return m_details;
    if (pane == QLatin1String("column"))
        return m_column;
    if (pane == QLatin1String("split"))
        return m_split;
    return 0;
}

void PaneWidths::resize(const QString &pane, double from, int by, int shared) {
    if (pane == QLatin1String("split")) {
        if (shared < 2 * MinSplitHalf)
            return;
        const int left = std::clamp(qRound(from * shared) + by, MinSplitHalf, shared - MinSplitHalf);
        setDraggedSplit(double(left) / shared);
        return;
    }
    const int width = qRound(from) + by;
    if (pane == QLatin1String("sidebar"))
        setDraggedSidebar(clamped(width, SidebarLimits));
    else if (pane == QLatin1String("details"))
        setDraggedDetails(clamped(width, DetailsLimits));
    else if (pane == QLatin1String("column"))
        setDraggedColumn(clamped(width, ColumnLimits));
}

double PaneWidths::dragStart(const QString &pane, int shared) const {
    if (pane == QLatin1String("split"))
        return double(splitLeft(shared)) / qMax(1, shared);
    if (pane == QLatin1String("sidebar"))
        return sidebar();
    if (pane == QLatin1String("details"))
        return details();
    if (pane == QLatin1String("column"))
        return column();
    return 0;
}

void PaneWidths::drag(const QString &pane, int by, int shared) {
    resize(pane, dragStart(pane, shared), by, shared);
}

void PaneWidths::reset(const QString &pane) {
    if (pane == QLatin1String("sidebar"))
        setDraggedSidebar(0);
    else if (pane == QLatin1String("details"))
        setDraggedDetails(0);
    else if (pane == QLatin1String("column"))
        setDraggedColumn(0);
    else if (pane == QLatin1String("split"))
        setDraggedSplit(0);
}

int PaneWidths::fitRows(QObject *model) const {
    // Text draws a name in the application's font when it's given none.
    return fitRows(model, QGuiApplication::font(), FileRowRoom, FolderRowRoom);
}

int PaneWidths::fitRows(QObject *model, const QFont &font, int fileRoom, int folderRoom) const {
    const auto *folder = qobject_cast<FolderModel *>(model);
    if (!folder)
        return 0;
    // One set of metrics for the whole folder, and the widest name of each
    // kind: a row is its name and the room beside it.
    const QFontMetricsF metrics(font);
    qreal file = -1, dir = -1;
    for (int row = 0; row < folder->rowCount(); ++row) {
        const QModelIndex index = folder->index(row);
        const qreal text = metrics.horizontalAdvance(folder->data(index, FolderModel::NameRole).toString());
        qreal &widest = folder->data(index, FolderModel::IsDirRole).toBool() ? dir : file;
        widest = std::max(widest, text);
    }
    return std::max(file < 0 ? 0 : qCeil(file) + fileRoom, dir < 0 ? 0 : qCeil(dir) + folderRoom);
}
