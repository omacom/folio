import QtQuick
import QtQuick.Controls

// The folder as a list: name, size and date (the date, then the size, step
// aside in a narrow window). No headings: the sort menu and S sort it.
ListView {
    id: list

    // Whether folders open in place (the window's tree, T). Off, the
    // list is a plain list: → and ← go into and out of folders, as in the
    // columns. Turned off, the open folders close, and the rows shifting
    // put the cursor on the folder at the top its item was in.
    property bool tree: false
    onTreeChanged: if (!tree) backend.folder.collapseAll()

    function select(row, keep) { selecting.select(row, keep) }
    // A click on empty space lets go of the cursor; the pane then keeps it
    // let go of through a re-read or a switch of view.
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }
    function focusView() { forceActiveFocus() }
    // Where the keyboard's menu (Alt+Return) opens: beside the cursor's item.
    function menuAnchor() { return currentItem }

    Selecting { id: selecting; view: list }
    onCurrentIndexChanged: selecting.cursorMoved()

    // Rows coming and going in place (a folder opened or closed above or
    // around the cursor, its items arriving, a file appearing, a new order)
    // would move the cursor off its item, and the cursor moving selects
    // what it lands on. So the rows are laid out at once, quietly, and the
    // cursor put back on its item; when that went out of sight with a
    // folder closed, on the folder, which is then selected if nothing is.
    property string shiftingPath: ""
    property string shiftingAnchor: ""
    Connections {
        target: backend.folder
        function onRowsShifting() {
            list.shiftingPath = backend.folder.get(list.currentIndex).path || ""
            list.shiftingAnchor = backend.folder.get(selecting.anchor).path || ""
        }
        function onRowsShifted() {
            const folder = backend.folder
            selecting.quiet = true
            list.forceLayout()
            selecting.quiet = false
            if (!list.shiftingPath)
                return
            const shown = folder.path.endsWith("/") ? folder.path : folder.path + "/"
            let row = folder.indexOfPath(list.shiftingPath)
            const hidden = row < 0
            for (let up = list.shiftingPath; row < 0 && up.startsWith(shown);) {
                up = up.substring(0, up.lastIndexOf("/"))
                row = folder.indexOfPath(up)
            }
            // Gone from the folder itself (trashed, moved away): the cursor
            // stays on its row, and the folder's re-read places it as before.
            if (row < 0)
                return
            if (row !== list.currentIndex)
                selecting.moveTo(row)
            if (hidden) {
                selecting.anchor = row
                if (folder.selectedCount === 0)
                    folder.selectOnly(row)
            } else {
                selecting.anchor = folder.indexOfPath(list.shiftingAnchor)
            }
        }
    }
    Keys.onPressed: (event) => selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)

    Band {
        view: list
        rectOf: (i) => Qt.rect(0, i * 32, list.width, 32)
    }

    // With the tree on, → opens the cursor's folder in place, below it, and on an open one
    // steps to its first item; ← closes it, or steps out to the folder it's
    // in, and at the top goes back up as before. Mirrored, the other way
    // round, with the reading direction. Where nothing nests (a search,
    // Recents, a plugin's view) → goes into the folder, as in the columns.
    function inward() {
        const folder = backend.folder
        const item = folder.get(currentIndex)
        if (!item.isDir)
            return
        if (!tree) {
            backend.navigate(item.path)
        } else if (item.open) {
            // Its items may still be on their way; until they're there, the
            // cursor stays put.
            if (currentIndex + 1 < count && folder.get(currentIndex + 1).depth > item.depth)
                moveCursor(currentIndex + 1)
        } else if (!folder.expand(currentIndex)) {
            backend.navigate(item.path)
        }
    }
    function outward() {
        const folder = backend.folder
        const item = folder.get(currentIndex)
        if (!tree)
            backend.goUp()
        else if (item.open)
            folder.collapse(currentIndex)
        else if (folder.parentRow(currentIndex) >= 0)
            moveCursor(folder.parentRow(currentIndex))
        else
            backend.goUp()
    }
    // Moved like the arrows move it, so what it lands on is selected.
    function moveCursor(row) {
        currentIndex = row
        positionViewAtIndex(row, ListView.Contain)
    }
    Keys.onRightPressed: LayoutMirroring.enabled ? outward() : inward()
    Keys.onLeftPressed: LayoutMirroring.enabled ? inward() : outward()

    // Right-click on an item (selecting it) or on empty space.
    signal contextMenu(var item, string folder, point at)
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => list.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
    }

    clip: true
    focus: true
    keyNavigationEnabled: true
    // No cursor of its own to start with: the pane puts it back where the
    // last view had it, and one on the first item would select that instead.
    currentIndex: -1
    acceptedButtons: Qt.NoButton
    boundsBehavior: Flickable.StopAtBounds
    highlightFollowsCurrentItem: false
    model: backend.folder
    ScrollBar.vertical: FolioScrollBar { }

    delegate: FileRow {
        width: list.width
        detailed: true
        tree: list.tree
        current: selected
        onClicked: (modifiers) => { selecting.click(index, modifiers); list.forceActiveFocus() }
        // The arrow opens or closes the folder in place, leaving the folder
        // shown and the selection as they are.
        onToggled: {
            if (list.tree)
                open ? backend.folder.collapse(index) : backend.folder.expand(index)
            list.forceActiveFocus()
        }
        onActivated: backend.open(path)
    }

    Text {
        anchors.centerIn: parent
        visible: list.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
        text: backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query)
            : plugins.views[backend.path] ? plugins.views[backend.path].empty : qsTr("This folder is empty")
        color: theme.secondaryForeground
    }
}
