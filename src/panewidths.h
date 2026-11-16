#pragma once

#include <QFont>
#include <QObject>
#include <QString>

// How wide the sidebar, the details, the columns and the two halves of a
// split are. There are three ways, which W steps through:
//   fixed         the widths Folio has always had;
//   automatic     the sidebar and the columns fit their widest row;
//   proportional  each pane keeps its share of a 1280 px window, so a big
//                 screen gets wider panes and a small one narrower.
// On top of any of them, a divider dragged gives its pane the width it was
// dragged to, until that divider is double-clicked or another way is
// stepped to. The sums live here, apart from the window, so they can be
// checked without one; Main.qml keeps the choice and the dragged widths in
// its Settings, and the views ask for their widths.
class PaneWidths : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    // The window's width, which the proportional widths follow.
    Q_PROPERTY(int windowWidth READ windowWidth WRITE setWindowWidth NOTIFY changed)
    // The dragged widths, 0 for a pane whose divider hasn't been dragged.
    // The split's is the left half's share of the room the two halves have
    // between them.
    Q_PROPERTY(int draggedSidebar READ draggedSidebar WRITE setDraggedSidebar NOTIFY changed)
    Q_PROPERTY(int draggedDetails READ draggedDetails WRITE setDraggedDetails NOTIFY changed)
    Q_PROPERTY(int draggedColumn READ draggedColumn WRITE setDraggedColumn NOTIFY changed)
    Q_PROPERTY(double draggedSplit READ draggedSplit WRITE setDraggedSplit NOTIFY changed)
    Q_PROPERTY(int details READ details NOTIFY changed)
    // Changes with everything above, for QML bindings that call sidebar(),
    // column() or splitLeft() to be read again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    // Each pane's fixed width, and how narrow and how wide it may go.
    struct Limits {
        int fixed;
        int min;
        int max;
    };
    static constexpr Limits SidebarLimits{190, 140, 400};
    static constexpr Limits DetailsLimits{300, 220, 600};
    static constexpr Limits ColumnLimits{260, 180, 480};
    // A split's half is never narrower than this.
    static constexpr int MinSplitHalf = 360;
    // The window the fixed widths were made for.
    static constexpr int ReferenceWidth = 1280;

    explicit PaneWidths(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    // Anything but the three ways is taken as fixed, so a hand-edited
    // setting can't leave the panes without a width.
    void setMode(const QString &mode);
    // W and Shift+W: the next way (or the one before), which every pane
    // takes, dragged or not. Returns the way it's now.
    Q_INVOKABLE QString stepMode(int by);
    int windowWidth() const { return m_windowWidth; }
    void setWindowWidth(int width);
    int draggedSidebar() const { return m_sidebar; }
    void setDraggedSidebar(int width);
    int draggedDetails() const { return m_details; }
    void setDraggedDetails(int width);
    int draggedColumn() const { return m_column; }
    void setDraggedColumn(int width);
    double draggedSplit() const { return m_split; }
    void setDraggedSplit(double share);
    int revision() const { return m_revision; }

    // The sidebar's width; `content` is its widest row, which only the
    // automatic widths use.
    Q_INVOKABLE int sidebar(int content = 0) const;
    int details() const;
    // Every column's width (they share one): `content` is the widest row
    // across the columns shown.
    Q_INVOKABLE int column(int content = 0) const;
    // The left half of a split whose halves have `shared` px between them;
    // the right half has the rest.
    Q_INVOKABLE int splitLeft(int shared) const;

    // Dragging a divider: the pane named ("sidebar", "details", "column"
    // or "split") grows by `by` px from `from`, its width (or share) when
    // the drag began, within its limits. `shared` is the split's room.
    Q_INVOKABLE void resize(const QString &pane, double from, int by, int shared = 0);
    // The same, from where it is now.
    Q_INVOKABLE void drag(const QString &pane, int by, int shared = 0);
    // A pane's dragged width (the split's share), 0 when it has none.
    Q_INVOKABLE double dragged(const QString &pane) const;
    // Where a drag of that divider starts from: the width the pane has
    // (its widest row unknown here, so the automatic sidebar and columns
    // start from their narrowest; the dividers pass the width on screen),
    // and for the split the share its left half has on screen, which the
    // limits may hold away from the share kept. Started from the kept one,
    // the divider wouldn't move until the pointer had made up the gap.
    Q_INVOKABLE double dragStart(const QString &pane, int shared = 0) const;
    // A double-click on a divider: that pane goes back to the way's width.
    Q_INVOKABLE void reset(const QString &pane);

    // The widest row of a column's folder model: its name in `font`, and
    // beside it `fileRoom` px on a file's row, `folderRoom` on a folder's
    // (the margins, the icon, and on a folder the arrow). The columns pass
    // what their own rows measure; without them it's FileRow's layout in
    // the application's font.
    Q_INVOKABLE int fitRows(QObject *model) const;
    Q_INVOKABLE int fitRows(QObject *model, const QFont &font, int fileRoom, int folderRoom) const;
    // FileRow in a column: 14 px in from each side, the 20 px icon and 12 px
    // after it, and on a folder 12 px more and the 12 px arrow pointing to
    // the next column.
    static constexpr int FileRowRoom = 14 + 20 + 12 + 14;
    static constexpr int FolderRowRoom = FileRowRoom + 12 + 12;

signals:
    void changed();

private:
    int scaled(const Limits &limits) const;
    // The way's own width for a pane, dragged or not.
    int modeWidth(const Limits &limits, int content) const;
    void bump();

    QString m_mode = QStringLiteral("fixed");
    int m_windowWidth = ReferenceWidth;
    int m_sidebar = 0;
    int m_details = 0;
    int m_column = 0;
    double m_split = 0;
    int m_revision = 0;
};
