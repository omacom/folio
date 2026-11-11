import QtQuick

// How a view selects, shared by the grid, the list and the column view:
// click selects one, Shift+click or Shift+arrows a range from the anchor,
// Alt+click adds or removes one, and moving the cursor with the arrows
// selects what it lands on. The selection itself lives in the folder model.
QtObject {
    // The ListView or GridView whose currentIndex is the cursor.
    required property var view
    property int anchor: -1
    // Set while the cursor is moved on purpose, so moving it doesn't also
    // reset the selection.
    property bool quiet: false

    function moveTo(index) {
        quiet = true
        view.currentIndex = index
        quiet = false
    }

    // The cursor moved by the keyboard (or the view itself).
    function cursorMoved() {
        if (quiet)
            return
        backend.folder.selectOnly(view.currentIndex)
        anchor = view.currentIndex
        if (view.currentIndex >= 0)
            view.positionViewAtIndex(view.currentIndex, ListView.Contain)
    }

    function click(index, modifiers) {
        if (index < 0)
            return
        if (modifiers & Qt.ShiftModifier) {
            backend.folder.selectRange(anchor < 0 ? index : anchor, index, (modifiers & Qt.AltModifier) !== 0)
        } else if (modifiers & Qt.AltModifier) {
            backend.folder.toggleSelected(index)
            anchor = index
        } else {
            backend.folder.selectOnly(index)
            anchor = index
        }
        moveTo(index)
    }

    // Shift+arrow: extend from the anchor to where the cursor goes.
    function extendTo(index) {
        if (index < 0 || index >= view.count)
            return
        if (anchor < 0)
            anchor = view.currentIndex
        moveTo(index)
        backend.folder.selectRange(anchor, index, false)
        view.positionViewAtIndex(index, ListView.Contain)
    }

    // Puts the cursor on `name` (or the first item). `keep` leaves a
    // selection of several alone, for a folder that was only re-read.
    function select(name, keep) {
        const found = name ? backend.folder.indexOf(name) : -1
        const index = found >= 0 ? found : (view.count > 0 ? 0 : -1)
        moveTo(index)
        if (!(keep && backend.folder.selectedCount > 0)) {
            backend.folder.selectOnly(index)
            anchor = index
        }
        if (index >= 0)
            view.positionViewAtIndex(index, ListView.Contain)
    }

    // Right-click on an item outside the selection selects just it, so
    // the menu acts on what was clicked.
    function menuAt(index) {
        if (index >= 0 && !backend.folder.isSelected(index))
            click(index, 0)
    }
}
