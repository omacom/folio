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
    readonly property int columnWidth: 260

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
    property bool complete: false
    property bool syncingTrail: false
    property var recentColumns: []
    // Two expanded viewports, including partial columns at their edges.
    readonly property int columnLimit: 2 * (Math.ceil(flick.width / (columnWidth + 1)) + 3)
    onColumnLimitChanged: updateColumns()

    function updateColumns() {
        if (!complete || syncingTrail) return
        const paths = []
        const near = []
        for (let i = 0; i < trail.count; ++i) {
            const path = trail.get(i).path
            paths.push(path)
            const x = row.x + (view.LayoutMirroring.enabled
                ? (trail.count - i) * (columnWidth + 1) + (inside.visible ? inside.width : 0)
                : i * (columnWidth + 1))
            if (trail.count <= columnLimit || (x + columnWidth > flick.contentX - columnWidth
                    && x < flick.contentX + flick.width + columnWidth))
                near.push(path)
        }
        // Protect the whole current area before evicting old ones. Updating
        // individual Loaders in binding order could evict a neighbour first.
        const older = recentColumns.filter(path => paths.indexOf(path) >= 0 && near.indexOf(path) < 0)
        const spare = Math.max(0, columnLimit - near.length)
        const keep = older.slice(Math.max(0, older.length - spare)).concat(near)
        if (keep.length !== recentColumns.length || keep.some((path, i) => path !== recentColumns[i]))
            recentColumns = keep
    }

    function syncTrail() {
        syncingTrail = true
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
        if (same < want.length)
            trail.append(want.slice(same))
        syncingTrail = false
        revealEnd()
    }
    onCrumbsChanged: syncTrail()
    Component.onCompleted: {
        syncTrail()
        complete = true
        revealEnd()
    }
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // Keeps the newest columns in view as the trail grows: on the left,
    // when mirrored.
    function revealEnd() {
        flick.contentX = view.LayoutMirroring.enabled ? 0 : Math.max(0, flick.contentWidth - flick.width)
        updateColumns()
    }
    LayoutMirroring.onEnabledChanged: revealEnd()
    function enter() {
        if (view.selected.isDir && (insideFolder.loading || insideFolder.count > 0 || insideFolder.error.length > 0))
            backend.navigate(view.selected.path)
    }

    component Separator: Rectangle { width: 1; height: parent ? parent.height : 0; color: Qt.alpha(theme.foreground, 0.1) }
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
        onContentXChanged: view.updateColumns()
        onContentWidthChanged: view.revealEnd()
        onWidthChanged: view.revealEnd()
        ScrollBar.horizontal: FolioScrollBar { }

        Row {
            id: row
            // A Row isn't placed by its Flickable, so it's moved to the right
            // edge by hand when mirrored.
            x: LayoutMirroring.enabled ? parent.width - width : 0
            // Known before Row's first layout, so RTL and revealEnd select
            // the right initial area without loading a transient first one.
            width: (trail.count + 1) * (view.columnWidth + 1) + (inside.visible ? inside.width : 0)
            height: flick.height
            onXChanged: view.updateColumns()

            // The folders on the way here, each showing which way the trail goes.
            Repeater {
                model: trail
                Row {
                    id: ancestor
                    required property string path
                    required property string next
                    width: view.columnWidth + 1
                    height: row.height
                    property real scrollY: NaN
                    onNextChanged: {
                        scrollY = NaN
                        if (columnLoader && columnLoader.item) columnLoader.item.revealNext()
                    }
                    Connections {
                        target: backend.folder
                        function onShowHiddenChanged() { ancestor.scrollY = NaN }
                        function onSortModeChanged() { ancestor.scrollY = NaN }
                    }
                    Connections {
                        target: translator
                        function onChanged() { ancestor.scrollY = NaN }
                    }

                    Loader {
                        id: columnLoader
                        property Item dragSource: null
                        width: view.columnWidth
                        height: parent.height
                        // Recently shown columns stay alive within the shared
                        // budget. A drag keeps its source until it finishes.
                        active: dragSource !== null || view.recentColumns.indexOf(ancestor.path) >= 0
                        sourceComponent: ListView {
                            id: column
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
                                onLoaded: if (column.complete) column.revealNext()
                            }
                            // Names sort by the language's alphabet.
                            Connections {
                                target: translator
                                function onChanged() { folder.reload() }
                            }
                            property bool complete: false
                            property bool positioned: false
                            property bool restoring: false
                            Connections {
                                target: folder
                                function onModelAboutToBeReset() { column.positioned = false }
                            }
                            function revealNext() {
                                const saved = ancestor.scrollY
                                restoring = true
                                forceLayout()
                                if (Number.isFinite(saved))
                                    contentY = originY + Math.max(0, Math.min(saved, contentHeight - height))
                                else
                                    positionViewAtIndex(Math.max(0, folder.indexOf(ancestor.next)), ListView.Contain)
                                restoring = false
                                positioned = true
                                if (Number.isFinite(saved)) ancestor.scrollY = contentY - originY
                            }
                            Component.onCompleted: {
                                complete = true
                                if (folder.count > 0 || !folder.loading) revealNext()
                            }
                            onContentYChanged: if (complete && positioned && !restoring && columnLoader.active) ancestor.scrollY = contentY - originY
                            ColumnDrop { items: column; folder: ancestor.path }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: (point) => {
                                    const index = column.indexAt(point.position.x + column.contentX, point.position.y + column.contentY)
                                    view.contextMenu(index >= 0 ? folder.get(index) : null, ancestor.path, point.scenePosition)
                                }
                            }
                            delegate: FileRow {
                                id: entry
                                width: column.width
                                chevron: true
                                trail: name === ancestor.next
                                Drag.onActiveChanged: {
                                    if (Drag.active) columnLoader.dragSource = entry
                                    else if (columnLoader.dragSource === entry) columnLoader.dragSource = null
                                }
                                onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                                onActivated: backend.open(path)
                            }
                        }
                    }
                    Separator { }
                }
            }

            // The folder being shown: its selection is the window's.
            ListView {
                id: here
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
            Separator { }

            // A look inside the selected folder; clicking in it goes there.
            ListView {
                id: inside
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
