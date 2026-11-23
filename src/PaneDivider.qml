import QtQuick

// The grip on a 1 px line between panes: dragged, it sizes the pane before
// the line; double-clicked, it puts that pane back to its default. It covers
// the line and a few pixels of the pane after it, never the one before,
// whose scroll bar runs along the line.
MouseArea {
    // The width the pane before the line has on screen.
    property real paneWidth: 0
    // The part of the pointer's travel the pane grows by.
    property real share: 1
    signal dragged(int width)
    signal reset()

    property real startX: 0
    property real startWidth: 0
    property bool dragging: false

    anchors.left: parent.left
    width: 7
    cursorShape: Qt.SplitHCursor
    preventStealing: true
    // The grip moves with its line, so the pointer is followed in the window.
    onPressed: (mouse) => {
        startX = mapToItem(null, mouse.x, 0).x
        startWidth = paneWidth
        dragging = false
    }
    onPositionChanged: (mouse) => {
        let across = mapToItem(null, mouse.x, 0).x - startX
        if (!dragging && Math.abs(across) < Qt.styleHints.startDragDistance)
            return
        dragging = true
        // Right to left, the pane before the line is on its right.
        if (LayoutMirroring.enabled)
            across = -across
        dragged(Math.max(1, Math.round(startWidth + across * share)))
    }
    onDoubleClicked: reset()
}
