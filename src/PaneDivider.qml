import QtQuick
import QtQuick.Controls

// The grip on a divider: dragged, the pane beside it grows or shrinks by as
// far as the pointer went, and keeps that width whatever way the widths are
// worked out (W); double-clicked, it goes back to the way's width. It has
// no look of its own, only the pointer that shows it can be dragged, so the
// window looks as it always has.
MouseArea {
    id: divider

    // The window's PaneWidths.
    property var widths: null
    // Which pane it sizes: "sidebar", "details", "column" or "split".
    property string pane
    // 1 when dragging right widens the pane (it's on the divider's left),
    // -1 when dragging left does. Mirrored, the panes swap sides, and so
    // does the way.
    property int growth: 1
    // For the split: the room its two halves have between them.
    property int shared: 0
    // The pane's width on screen, where a drag starts from: the automatic
    // sidebar and columns are as wide as their rows, which only the window
    // knows. Left at -1, the widths work it out.
    property real paneWidth: -1

    width: 8
    enabled: !!widths
    visible: enabled
    cursorShape: Qt.SplitHCursor
    preventStealing: true

    // Measured in the window, not here: the divider moves with the pane
    // as it's dragged.
    property real startX: 0
    property real startWidth: 0
    // Whether the pointer has gone far enough across to be a drag: a
    // click, or a press that only wobbles up and down, keeps the pane as
    // the way has it, rather than holding it at the width it happens to
    // have.
    property bool dragging: false
    function sceneX(mouse) { return mapToItem(null, mouse.x, 0).x }

    // A scroll bar showing under the point in the window (hovered or
    // scrolling, with something to scroll): the grip reaches over the edge
    // of the pane beside it, where that pane's scroll bar is, and a press
    // on the bar the pointer has brought up goes to it. Where no bar shows,
    // the grip takes presses on both sides of its line.
    function scrollBarAt(item, sx, sy) {
        for (let i = item.children.length - 1; i >= 0; --i) {
            const child = item.children[i]
            if (child === divider || !child.visible || !child.enabled)
                continue
            const at = child.mapFromItem(null, sx, sy)
            if (!child.contains(at))
                continue
            if (child instanceof ScrollBar && child.size < 1 && (child.hovered || child.active))
                return child
            const found = scrollBarAt(child, sx, sy)
            if (found)
                return found
        }
        return null
    }

    onPressed: (mouse) => {
        const scene = mapToItem(null, mouse.x, mouse.y)
        if (Window.window && scrollBarAt(Window.window.contentItem, scene.x, scene.y)) {
            mouse.accepted = false
            return
        }
        dragging = false
        startX = sceneX(mouse)
        // From the width the pane has on screen: the split's halves may be
        // held at their limits, away from the share kept.
        startWidth = pane !== "split" && paneWidth >= 0 ? paneWidth : widths.dragStart(pane, shared)
    }
    onPositionChanged: (mouse) => {
        if (!pressed)
            return
        const across = sceneX(mouse) - startX
        if (!dragging && Math.abs(across) < Qt.styleHints.startDragDistance)
            return
        dragging = true
        const way = LayoutMirroring.enabled ? -growth : growth
        widths.resize(pane, startWidth, Math.round(across * way), shared)
    }
    onDoubleClicked: widths.reset(pane)
}
