import QtQuick
import QtQuick.Controls
import Folio

// Columns, as in the macOS Finder: the folders on the way here from the
// root (~, another machine, or /), the current folder, and what's inside
// the selected subfolder. → goes into the selected folder, ← back out;
// clicking anywhere to the left jumps there.
Item {
    id: view

    property alias currentIndex: here.currentIndex
    // Every column has the one width the window's pane widths give (the
    // fixed one in a window without them). Fitted to what they show, it's
    // the widest row across the columns in view; a dragged width needs no
    // rows measured.
    readonly property var widths: (ApplicationWindow.window && ApplicationWindow.window.widths) || null
    readonly property bool fitting: !!widths && widths.mode === "automatic" && widths.draggedColumn === 0
    readonly property int contentFit: {
        if (!fitting)
            return 0
        let widest = Math.max(here.fit, inside.visible ? inside.fit : 0)
        for (let i = 0; i < ancestors.count; ++i) {
            const column = ancestors.itemAt(i)
            if (column)
                widest = Math.max(widest, column.fit)
        }
        return widest
    }
    readonly property int columnWidth: widths ? (widths.revision, widths.column(contentFit)) : 260

    function select(row, keep) { selecting.select(row, keep) }
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }

    Selecting { id: selecting; view: here }
    function focusView() { here.forceActiveFocus() }
    function menuAnchor() { return here.currentItem }

    // Every column has a menu: on an item there, or on the column's folder.
    signal contextMenu(var item, string folder, point at)

    readonly property var crumbs: backend.crumbs

    // The folders on the way here, kept as a list model that is only trimmed
    // and extended: moving one folder deeper or back out adds or drops one
    // column, and the rest stay as they are instead of reloading (which
    // flashed).
    ListModel { id: trail }
    function syncTrail() {
        const want = []
        for (let i = 0; i + 1 < crumbs.length; ++i)
            want.push({ path: crumbs[i].path, next: crumbs[i + 1].name })
        let same = 0
        while (same < trail.count && same < want.length && trail.get(same).path === want[same].path)
            ++same
        // Columns that stay may now point at a different next folder.
        for (let i = 0; i < same; ++i)
            if (trail.get(i).next !== want[i].next)
                trail.setProperty(i, "next", want[i].next)
        if (trail.count > same)
            trail.remove(same, trail.count - same)
        for (let i = same; i < want.length; ++i)
            trail.append(want[i])
    }
    onCrumbsChanged: syncTrail()
    Component.onCompleted: syncTrail()
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // Keeps the newest columns in view as the trail grows: on the left,
    // when mirrored.
    function revealEnd() { flick.contentX = view.LayoutMirroring.enabled ? 0 : Math.max(0, flick.contentWidth - flick.width) }
    LayoutMirroring.onEnabledChanged: revealEnd()
    function enter() {
        if (view.selected.isDir && (insideFolder.loading || insideFolder.count > 0 || insideFolder.error.length > 0))
            backend.navigate(view.selected.path)
    }

    // After each column, with the grip that sizes them all, on both
    // sides of the line as the window's dividers are. The column after it
    // comes later and would be drawn over the grip, taking its presses, so
    // the line is raised above it (and each column on the way here above
    // the next, for the same reason).
    component Separator: Rectangle {
        property alias widths: grip.widths
        property alias paneWidth: grip.paneWidth
        z: 1
        width: 1
        height: parent ? parent.height : 0
        color: Qt.alpha(theme.foreground, 0.1)
        PaneDivider {
            id: grip
            objectName: "columnDivider"
            anchors.horizontalCenter: parent.horizontalCenter
            height: parent.height
            pane: "column"
        }
    }

    // Two rows no one sees, a file's and a folder's, laid out as the
    // columns' own: the automatic widths take the font of their names and
    // the room beside them from these, so the widest name fits the row
    // that's really drawn.
    FileRow {
        id: fileSample
        opacity: 0
        enabled: false
        chevron: true
        index: -1; name: ""; path: ""; isDir: false; kind: ""; mime: ""; extension: ""; size: 0
        modified: new Date(0); itemCount: 0; icon: ""; selected: false
    }
    FileRow {
        id: folderSample
        opacity: 0
        enabled: false
        chevron: true
        index: -1; name: ""; path: ""; isDir: true; kind: "folder"; mime: ""; extension: ""; size: 0
        modified: new Date(0); itemCount: 0; icon: ""; selected: false
    }
    function fitRows(model) {
        return widths.fitRows(model, fileSample.nameFont, Math.ceil(fileSample.besideName), Math.ceil(folderSample.besideName))
    }
    // Files dropped in a column land in the folder it shows. It lies over
    // the whole column, under its items, rather than in the list's content,
    // which ends at its last item.
    component ColumnDrop: FolderDrop { parent: items; anchors.fill: parent; z: -1 }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        acceptedButtons: Qt.NoButton
        contentWidth: Math.max(width, row.width)
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        onContentWidthChanged: view.revealEnd()
        onWidthChanged: view.revealEnd()
        ScrollBar.horizontal: FolioScrollBar { }

        Row {
            id: row
            // A Row isn't placed by its Flickable, so it's moved to the right
            // edge by hand when mirrored.
            x: LayoutMirroring.enabled ? parent.width - width : 0
            height: flick.height

            // The folders on the way here, each showing which way the trail goes.
            Repeater {
                id: ancestors
                model: trail
                Row {
                    id: ancestor
                    required property int index
                    required property string path
                    required property string next
                    // Its widest row, while the columns are fitted to them.
                    readonly property int fit: view.fitting ? (folder.count, folder.loading, view.fitRows(folder)) : 0
                    // Above the next one, for its divider's grip (see Separator).
                    z: ancestors.count - index + 1
                    height: row.height

                    ListView {
                        id: column
                        objectName: "column"
                        width: view.columnWidth
                        height: parent.height
                        clip: true
                        acceptedButtons: Qt.NoButton
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: FolioScrollBar { }
                        model: FolderModel {
                            id: folder
                            path: ancestor.path
                            showHidden: backend.folder.showHidden
                            sortMode: backend.folder.sortMode
                            onLoaded: column.revealNext()
                        }
                        // Names sort by the language's alphabet.
                        Connections {
                            target: translator
                            function onChanged() { folder.reload() }
                        }
                        function revealNext() { positionViewAtIndex(Math.max(0, folder.indexOf(ancestor.next)), ListView.Contain) }
                        ColumnDrop { items: column; folder: ancestor.path }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: (point) => {
                                const index = column.indexAt(point.position.x + column.contentX, point.position.y + column.contentY)
                                view.contextMenu(index >= 0 ? folder.get(index) : null, ancestor.path, point.scenePosition)
                            }
                        }
                        Connections {
                            target: ancestor
                            function onNextChanged() { column.revealNext() }
                        }
                        delegate: FileRow {
                            width: column.width
                            chevron: true
                            trail: name === ancestor.next
                            onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                            onActivated: backend.open(path)
                        }
                    }
                    Separator { widths: view.widths; paneWidth: view.columnWidth }
                }
            }

            // The folder being shown: its selection is the window's.
            ListView {
                id: here
                objectName: "column"
                readonly property int fit: view.fitting ? (backend.folder.count, backend.folder.loading, view.fitRows(backend.folder)) : 0
                width: view.columnWidth
                height: row.height
                clip: true
                focus: true
                keyNavigationEnabled: true
                // No cursor of its own to start with: the pane puts it back
                // where the last view had it, and one on the first item would
                // select that instead.
                currentIndex: -1
                acceptedButtons: Qt.NoButton
                boundsBehavior: Flickable.StopAtBounds
                highlightFollowsCurrentItem: false
                model: backend.folder
                ScrollBar.vertical: FolioScrollBar { }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => view.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
                }
                ColumnDrop { items: here; folder: backend.path }

                onCurrentIndexChanged: selecting.cursorMoved()
                Keys.onPressed: (event) => selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)
                // Into a folder is the way the columns grow: leftward, when
                // mirrored.
                Keys.onRightPressed: view.LayoutMirroring.enabled ? backend.goUp() : view.enter()
                Keys.onLeftPressed: view.LayoutMirroring.enabled ? view.enter() : backend.goUp()

                delegate: FileRow {
                    width: here.width
                    chevron: true
                    current: selected
                    onClicked: (modifiers) => { selecting.click(index, modifiers); here.forceActiveFocus() }
                    onActivated: backend.open(path)
                }

                // Where the first item would be, lined up with the names.
                Text {
                    x: LayoutMirroring.enabled ? here.width - width - 46 : 46
                    y: 8
                    visible: here.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
                    text: backend.folder.query ? qsTr("No matches") : qsTr("No items")
                    color: theme.secondaryForeground
                }
            }
            Separator { widths: view.widths; paneWidth: view.columnWidth }

            // A look inside the selected folder; clicking in it goes there.
            ListView {
                id: inside
                objectName: "column"
                readonly property int fit: view.fitting ? (insideFolder.count, insideFolder.loading, view.fitRows(insideFolder)) : 0
                width: view.columnWidth
                height: row.height
                visible: view.selected.isDir === true
                clip: true
                acceptedButtons: Qt.NoButton
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: FolioScrollBar { }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => {
                        const index = inside.indexAt(point.position.x + inside.contentX, point.position.y + inside.contentY)
                        view.contextMenu(index >= 0 ? inside.model.get(index) : null, view.selected.path, point.scenePosition)
                    }
                }
                model: FolderModel {
                    id: insideFolder
                    path: view.selected.isDir === true ? view.selected.path : ""
                    showHidden: backend.folder.showHidden
                    sortMode: backend.folder.sortMode
                }
                Connections {
                    target: translator
                    function onChanged() { insideFolder.reload() }
                }
                ColumnDrop { items: inside; folder: view.selected.path ?? "" }
                delegate: FileRow {
                    width: inside.width
                    chevron: true
                    onClicked: backend.showItem(path)
                    onActivated: backend.open(path)
                }

                Text {
                    x: LayoutMirroring.enabled ? inside.width - width - 46 : 46
                    y: 8
                    visible: inside.count === 0 && !inside.model.loading
                    text: qsTr("No items")
                    color: theme.secondaryForeground
                }
            }
        }
    }
}
