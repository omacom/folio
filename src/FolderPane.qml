import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One folder pane: its path bar, problems banner and the files (grid,
// list or columns), with its own menu and dialogs. A window
// has one, or two side by side (Ctrl+Alt+N); each has its own folder,
// history, selection and preview through its own `backend` and `fileOps`.
// Keys act on the pane that was used last.
FocusScope {
    id: pane

    // The window this pane is in, for the state the panes share.
    readonly property var host: ApplicationWindow.window
    readonly property bool active: !!host && host.activePane === pane
    readonly property var paneBackend: backend
    property bool closable: false
    signal closeRequested()
    // How far this pane's path bar reaches right, over the details column,
    // so the rightmost pane's buttons sit at the window's edge.
    property int barExtension: 0
    property int barLeft: 0

    property string viewMode: host ? host.viewMode : "grid"
    onViewModeChanged: if (host && active) host.viewMode = viewMode
    function cycleView() {
        viewMode = viewMode === "grid" ? "list" : viewMode === "list" ? "columns" : "grid"
    }

    // The selected name, carried across a switch of view.
    property string lastName: ""
    property string loadedPath: ""
    // While a text field has the keyboard (renaming, a password), single-key
    // and clipboard shortcuts stand aside so typing works.
    readonly property bool typing: rename.visible || confirm.visible || backend.mounter.asking || pathBar.editing || pathBar.searchFocused || (!!host && host.helpVisible)
    readonly property bool keys: active && !typing
    // What file actions act on: everything selected, or the item under
    // the cursor when nothing is.
    readonly property var selectedPaths: backend.folder.selectedCount > 0 ? backend.folder.selectedPaths()
                                       : (current.path ? [current.path] : [])
    readonly property var current: {
        // Re-read when the folder changes underneath the selection.
        backend.folder.count
        const index = view.item ? view.item.currentIndex : -1
        return index >= 0 ? backend.folder.get(index) : ({})
    }
    onCurrentChanged: {
        if (current.name)
            lastName = current.name
        // An open preview stays on the selected item.
        if (backend.previewing && current.path)
            backend.followSelection(current.path)
    }

    // In the trash, Delete means for good (after asking), and there's
    // Restore and Empty Trash.
    readonly property bool inTrash: fileOps.inTrash(backend.path)
    function deleteSelected() {
        const paths = pane.selectedPaths
        if (paths.length === 0)
            return
        if (!pane.inTrash) {
            fileOps.trash(paths)
            return
        }
        confirm.ask(qsTr("Delete for good?"),
                    paths.length === 1 ? qsTr("%1 will be deleted permanently. This can't be undone.").arg(pane.current.name)
                                       : qsTr("%n items will be deleted permanently. This can't be undone.", "", paths.length),
                    qsTr("Delete"), () => fileOps.deleteForever(paths))
    }
    function emptyTrash() {
        const count = fileOps.trashCount()
        if (count === 0)
            return
        confirm.ask(qsTr("Empty the trash?"),
                    qsTr("%n items will be deleted permanently. This can't be undone.", "", count),
                    qsTr("Empty Trash"), () => fileOps.emptyTrash())
    }

    // Whichever pane has the keyboard is the one keys, the sidebar and the
    // details pane follow.
    onActiveFocusChanged: if (activeFocus && host) host.activePane = pane
    function focusView() { if (view.item) view.item.focusView() }

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    function formatSize(bytes) {
        const units = [qsTr("B"), qsTr("KB"), qsTr("MB"), qsTr("GB"), qsTr("TB")]
        let i = 0
        let value = bytes
        while (value >= 1000 && i < units.length - 1) { value /= 1000; ++i }
        return value.toLocaleString(Qt.locale(), "f", i === 0 ? 0 : 1) + " " + units[i]
    }

    // Pins the selected folder, or the folder being shown when a file is
    // selected; pressed again, unpins it.
    function togglePin() {
        const target = current.isDir ? current.path : backend.path
        if (places.isPinned(target)) {
            places.unpin(target)
            return
        }
        const problem = places.pin(target)
        if (problem.length > 0)
            backend.showMessage(problem)
    }

    Shortcut { sequence: "Space"; enabled: pane.keys; onActivated: backend.togglePreview(pane.current.path || "") }
    Shortcut { sequences: [StandardKey.Copy]; enabled: pane.keys; onActivated: fileOps.copy(pane.selectedPaths) }
    Shortcut { sequences: [StandardKey.Cut]; enabled: pane.keys; onActivated: fileOps.cut(pane.selectedPaths) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: pane.keys; onActivated: fileOps.paste(backend.path) }
    // "Yank": the selected path (or the folder's) as text, for a terminal.
    Shortcut { sequence: "Ctrl+Shift+C"; enabled: pane.keys; onActivated: fileOps.yank(pane.selectedPaths.length > 0 ? pane.selectedPaths : [backend.path]) }
    Shortcut { sequence: "Delete"; enabled: pane.keys; onActivated: pane.deleteSelected() }
    Shortcut { sequence: "F2"; enabled: pane.keys; onActivated: if (pane.current.path) rename.open(pane.current) }
    Shortcut { sequences: [StandardKey.SelectAll]; enabled: pane.keys; onActivated: backend.folder.selectAll() }
    // Escape narrows a selection of several back to the cursor, then ends
    // a search.
    Shortcut {
        sequence: "Escape"
        enabled: pane.keys && (backend.folder.selectedCount > 1 || backend.folder.query.length > 0)
        onActivated: {
            if (backend.folder.selectedCount > 1)
                backend.folder.selectOnly(view.item ? view.item.currentIndex : -1)
            else
                pathBar.endSearch()
        }
    }
    Shortcut { sequence: "/"; enabled: pane.keys && backend.path !== "recent:"; onActivated: pathBar.search() }
    // The right-click menu from the keyboard, beside the selected item.
    Shortcut {
        sequences: ["Alt+Return", "Alt+Enter", "Menu", "Shift+F10"]
        enabled: pane.keys
        onActivated: {
            const anchor = view.item && view.item.menuAnchor ? view.item.menuAnchor() : null
            fileMenu.show(pane.current.path ? pane.current : null, backend.path, anchor)
        }
    }
    Shortcut { sequence: "Ctrl+Shift+D"; enabled: pane.keys; onActivated: if (pane.current.path) fileOps.duplicate(pane.current.path) }
    Shortcut { sequences: [StandardKey.New]; enabled: pane.keys; onActivated: backend.newWindow() }
    Shortcut { sequence: "Ctrl+Shift+N"; enabled: pane.keys; onActivated: fileOps.newFolder(backend.path) }
    Shortcut { sequences: ["Ctrl+L", "Alt+D"]; enabled: pane.keys; onActivated: pathBar.editPath() }
    Shortcut { sequence: "Ctrl+D"; enabled: pane.active; onActivated: pane.togglePin() }
    Shortcut { sequence: "Ctrl+1"; enabled: pane.active; onActivated: pane.viewMode = "grid" }
    Shortcut { sequence: "Ctrl+2"; enabled: pane.active; onActivated: pane.viewMode = "list" }
    Shortcut { sequence: "Ctrl+3"; enabled: pane.active; onActivated: pane.viewMode = "columns" }
    Shortcut { sequences: ["Return", "Enter"]; enabled: pane.keys && pane.activeFocus; onActivated: if (pane.current.path) backend.open(pane.current.path) }
    Shortcut { sequences: ["Backspace", "Alt+Up"]; enabled: pane.keys; onActivated: backend.goUp() }
    Shortcut { sequences: ["Alt+Left", StandardKey.Back]; enabled: pane.active; onActivated: backend.back() }
    Shortcut { sequences: ["Alt+Right", StandardKey.Forward]; enabled: pane.active; onActivated: backend.forward() }
    Shortcut { sequence: "Alt+Home"; enabled: pane.active; onActivated: backend.navigate(places.home) }
    Shortcut { sequence: "Ctrl+H"; enabled: pane.active; onActivated: backend.folder.showHidden = !backend.folder.showHidden }
    // S steps through the sort orders, Shift+S back.
    function stepSort(by) {
        const modes = ["name", "kind", "modified", "oldest", "size"]
        backend.folder.sortMode = modes[(modes.indexOf(backend.folder.sortMode) + by + modes.length) % modes.length]
    }
    Shortcut { sequence: "S"; enabled: pane.keys; onActivated: pane.stepSort(1) }
    Shortcut { sequence: "Shift+S"; enabled: pane.keys; onActivated: pane.stepSort(-1) }
    Shortcut { sequences: ["F5", StandardKey.Refresh]; enabled: pane.active; onActivated: { backend.folder.reload(); places.refresh() } }

    Connections {
        target: backend
        function onSelectRequested(name) { if (view.item) view.item.select(name, false) }
        function onActivateRequested() { if (pane.host) { pane.host.raise(); pane.host.requestActivate() } }
    }

    Connections {
        target: view.item
        ignoreUnknownSignals: true
        function onContextMenu(item, folder) { fileMenu.show(item, folder) }
    }

    // Finished file work selects what it made (the pasted file, the new
    // folder, the new name) once the folder has re-read.
    Connections {
        target: fileOps
        function onDone(folder, name) { if (folder === backend.path) backend.expectSelection(name) }
        function onFailed(message) { backend.showMessage(message) }
        function onNotice(notice) { backend.showNotice(notice) }
    }

    Connections {
        target: backend.folder
        // A folder arriving selects where we came from (or the first item);
        // the same folder re-read (a remembered listing refreshed, or files
        // changing on disk) keeps what was selected.
        function onLoaded() {
            if (!view.item)
                return
            const pending = backend.takeSelection()
            const same = backend.folder.path === pane.loadedPath
            pane.loadedPath = backend.folder.path
            view.item.select(pending || (same ? pane.lastName : ""), same && !pending)
        }
    }

    FileMenu {
        id: fileMenu
        objectName: "fileMenu"
        onRenameRequested: (target) => rename.open(target)
        onDeleteRequested: pane.deleteSelected()
        onEmptyTrashRequested: pane.emptyTrash()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Room for the path bar, which is drawn on top so it can reach over
        // the details column.
        Item { Layout.fillWidth: true; implicitHeight: pathBar.implicitHeight }
        // Problems and slow work are said in the window, not swallowed.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            visible: backend.message.length > 0 || backend.busy.length > 0 || fileOps.busy.length > 0 || backend.folder.error.length > 0 || backend.notice.length > 0
            color: backend.message.length > 0 || backend.folder.error.length > 0 ? pane.tint(theme.red, 0.14) : pane.tint(theme.accent, 0.10)
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 12
                Text {
                    Layout.fillWidth: true
                    text: backend.message || backend.folder.error || backend.busy || fileOps.busy || backend.notice
                    color: theme.brightForeground
                    elide: Text.ElideMiddle
                }
                IconButton {
                    visible: backend.message.length > 0
                    glyph: "close"
                    text: qsTr("Dismiss")
                    onClicked: backend.dismissMessage()
                }
            }
        }

        // The trash offers one thing: emptying it.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            visible: pane.inTrash && backend.folder.count > 0
            color: pane.tint(theme.foreground, 0.04)
            PlainButton {
                anchors.centerIn: parent
                text: qsTr("Empty Trash")
                onClicked: pane.emptyTrash()
            }
        }

        Item { implicitHeight: 16 }

        // Grid, list or columns: each takes the folder's model and keeps the
        // pane's one selection.
        Loader {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: pane.viewMode === "grid" ? 24 : pane.viewMode === "list" ? 20 : 0
            Layout.rightMargin: pane.viewMode === "columns" ? 0 : 24
            sourceComponent: pane.viewMode === "list" ? listView
                           : pane.viewMode === "columns" ? columnsView
                           : gridView
            // A new view picks up the selection the old one had.
            onLoaded: { item.select(pane.lastName, true); item.focusView() }

            // Files dropped on empty space land in this folder; folders
            // inside take drops on themselves first.
            DropArea {
                id: viewDrop
                anchors.fill: parent
                z: -1
                keys: ["text/uri-list"]
                onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, backend.path) }
                onDropped: (drop) => { fileOps.dropInto(drop.urls, backend.path); drop.accept(Qt.MoveAction) }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    visible: viewDrop.containsDrag
                    color: pane.tint(theme.accent, 0.05)
                    border.width: 2
                    border.color: theme.accent
                    radius: Math.min(6, theme.radius)
                }
            }
        }
        Component { id: gridView; FolderGrid { } }
        Component { id: listView; FolderList { } }
        Component { id: columnsView; FolderColumns { } }
    }

    // Drawn over the window's top bar. It reaches left over the sidebar for
    // the first pane (so the path starts at the window's edge) and right over
    // the details column for the last one (so the buttons end at the edge).
    PathBar {
        id: pathBar
        x: -pane.barLeft
        width: pane.barLeft + pane.width + pane.barExtension
        onEditDone: pane.focusView()
        previewVisible: pane.host ? pane.host.previewVisible : true
        viewMode: pane.viewMode
        closable: pane.closable
        onTogglePreview: if (pane.host) pane.host.previewVisible = !pane.host.previewVisible
        onCycleView: pane.cycleView()
        onCloseRequested: pane.closeRequested()
    }

    // With two panes, a line along the top marks the one keys go to.
    Rectangle {
        visible: pane.closable || (pane.host && pane.host.split)
        y: pathBar.height - 1
        width: parent.width
        height: 2
        color: pane.active ? theme.accent : "transparent"
    }

    // The mouse's back and forward thumb buttons move through history, as in
    // a browser. Only those buttons are taken; every other click passes
    // through to what's underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onPressed: mouse => mouse.button === Qt.BackButton ? backend.back() : backend.forward()
    }

    MountPrompt {
        anchors.fill: parent
    }

    RenameDialog {
        id: rename
        anchors.fill: parent
    }

    ConfirmDialog {
        id: confirm
        anchors.fill: parent
    }
}
