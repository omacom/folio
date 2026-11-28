import QtQuick
import QtQuick.Controls
import Folio

// Columns, as in the macOS Finder: the folders on the way here from the
// root (~, another machine, or /), the current folder, and what's inside
// the selected subfolder. → goes into the selected folder, ← back out;
// clicking anywhere to the left jumps there.
//
// With `fixed` (threeColumns in folio.conf), three columns that never
// scroll, as in ranger or yazi: the parent folder, the current one and a
// preview of the selected item, sharing the width 1:3:4: the preview gets
// half, names don't need it. The lines between them can be dragged; a
// double-click on one shares the width 1:3:4 again.
Item {
    id: view

    property alias currentIndex: here.currentIndex
    property bool fixed: false
    readonly property int columnWidth: Math.round(260 * theme.textScale)
    // The fixed parent's and folder's shares of the width, the preview
    // taking the rest. Dragging a line sets them and says so, for the pane
    // to remember.
    property real parentShare: 1 / 8
    property real currentShare: 3 / 8
    signal resized(real parentShare, real currentShare)
    // No fixed column gets narrower than this, by dragging or not.
    readonly property int narrowest: Math.round(120 * theme.textScale)
    // The fixed columns' widths, less a separator each.
    readonly property int parentWidth: Math.max(narrowest, Math.round(flick.width * parentShare) - 1)
    readonly property int currentWidth: Math.max(narrowest, Math.round(flick.width * currentShare) - 1)
    readonly property int previewWidth: Math.max(0, flick.width - parentWidth - currentWidth - 2)
    // Moves a line between the fixed columns by dx from where the drag
    // started: the first trades width between the parent and the folder,
    // the second between the folder and the preview. None goes past
    // `narrowest`.
    function moveLine(line, dx, parentFrom, currentFrom) {
        let parentTo = parentFrom, currentTo = currentFrom
        if (line === 0) {
            const d = Math.max(narrowest - parentFrom, Math.min(dx, currentFrom - narrowest))
            parentTo += d
            currentTo -= d
        } else {
            const preview = flick.width - 2 - parentFrom - currentFrom
            currentTo += Math.max(narrowest - currentFrom, Math.min(dx, preview - narrowest))
        }
        shareWidth((parentTo + 1) / flick.width, (currentTo + 1) / flick.width)
    }
    // Fixed, the keys drive: once the cursor moves, the row left under a
    // still pointer isn't lit as hovered, so only the cursor's row stands
    // out. Moving the pointer lights it again.
    property bool pointerStill: false
    HoverHandler {
        property point at
        onPointChanged: {
            const moved = point.scenePosition
            if (moved.x !== at.x || moved.y !== at.y) {
                at = moved
                view.pointerStill = false
            }
        }
    }
    function shareWidth(parentPart, currentPart) {
        parentShare = parentPart
        currentShare = currentPart
        resized(parentPart, currentPart)
    }
    // Fixed, the pane leaves no room above the columns, so their lines meet
    // the bar: the room above the first row is the lists' own.
    readonly property int topRoom: fixed ? 16 : 0
    readonly property int rowHeight: Math.round(32 * theme.textScale)
    // The pane whose selected file the details column describes.
    property var detailsPane: null
    readonly property var details: detailsColumn.item

    function select(row, keep) { selecting.select(row, keep) }
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }

    Selecting { id: selecting; view: here; pageStep: Math.max(1, Math.floor(here.height / view.rowHeight) - 1) }
    function focusView() { here.forceActiveFocus() }
    // The preview's ↑ and ↓: the row above or below in the current column.
    function stepRow(direction) { direction < 0 ? here.decrementCurrentIndex() : here.incrementCurrentIndex() }
    function menuAnchor() { return here.currentItem }

    // Every column has a menu: on an item there, or on the column's folder.
    signal contextMenu(var item, string folder, point at)

    readonly property var crumbs: backend.crumbs

    // The folders on the way here, kept as a list model that is only trimmed
    // and extended: moving one folder deeper or back out adds or drops one
    // column, and the rest stay as they are instead of reloading (which
    // flashed).
    ListModel { id: trail }
    // How many of the folders on the way here are scrolled out of sight to
    // the left: a place picked in the sidebar is the first column shown,
    // with the folders above it there to scroll back to. Going up past it
    // shows what's above again.
    property int hidden: 0
    property string startingAt: ""
    function startAt(path) {
        startingAt = path
        if (backend.path === path)
            started()
    }
    function started() {
        startingAt = ""
        hidden = trail.count
        revealEnd()
    }
    function syncTrail() {
        const want = []
        for (let i = 0; i + 1 < crumbs.length; ++i)
            want.push({ path: crumbs[i].path, next: crumbs[i + 1].name })
        let same = 0
        while (same < trail.count && same < want.length && trail.get(same).path === want[same].path)
            ++same
        // Somewhere else altogether (another drive, /) starts from its top.
        if (same === 0)
            hidden = 0
        // Columns that stay may now point at a different next folder.
        for (let i = 0; i < same; ++i)
            if (trail.get(i).next !== want[i].next)
                trail.setProperty(i, "next", want[i].next)
        if (trail.count > same)
            trail.remove(same, trail.count - same)
        for (let i = same; i < want.length; ++i)
            trail.append(want[i])
        hidden = Math.min(hidden, trail.count)
        if (startingAt.length > 0 && backend.path === startingAt)
            started()
    }
    onCrumbsChanged: syncTrail()
    Component.onCompleted: syncTrail()
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // A hidden ancestor needs no listing until it is near the viewport.
    // Once shown, keep its model and scroll position just as before.
    function loadColumns() {
        row.forceLayout()
        for (let i = 0; i < ancestors.count; ++i) {
            const column = ancestors.itemAt(i)
            if (!column || !column.visible) continue
            const x = column.mapToItem(flick, 0, 0).x
            if (x + column.width > -columnWidth && x < flick.width + columnWidth)
                column.shown = true
        }
    }

    // Where the first column shown starts, from the side the columns grow
    // from: past the hidden ones and their separators.
    readonly property int hiddenWidth: hidden * (columnWidth + 1)
    // Keeps the newest columns in view as the trail grows, and the hidden
    // ones out of it: on the left, when mirrored.
    function revealEnd() {
        if (view.fixed) {
            flick.contentX = 0
            Qt.callLater(view.loadColumns)
            return
        }
        const from = Math.max(view.hiddenWidth, row.width - flick.width)
        const most = Math.max(0, flick.contentWidth - flick.width)
        flick.contentX = view.LayoutMirroring.enabled ? most - from : from
        Qt.callLater(view.loadColumns)
    }
    LayoutMirroring.onEnabledChanged: revealEnd()
    onFixedChanged: revealEnd()
    function enter() {
        if (view.selected.isDir && (insideFolder.loading || insideFolder.count > 0 || insideFolder.error.length > 0))
            backend.navigate(view.selected.path)
    }

    component Separator: Rectangle { width: 1; height: parent ? parent.height : 0; color: Qt.alpha(theme.foreground, 0.1) }
    // Fixed, a grip over a line between the columns: 0 after the parent, 1
    // after the folder. Explicit x isn't mirrored, so it's placed by hand.
    component LineGrip: MouseArea {
        required property int line
        readonly property int at: line === 0 ? view.parentWidth : view.parentWidth + 1 + view.currentWidth
        visible: view.fixed
        z: 1
        width: 9
        height: view.height
        x: (view.LayoutMirroring.enabled ? view.width - 1 - at : at) - 4
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        property real fromX: 0
        property int parentFrom: 0
        property int currentFrom: 0
        onPressed: (mouse) => {
            fromX = mapToItem(view, mouse.x, 0).x
            parentFrom = view.parentWidth
            currentFrom = view.currentWidth
        }
        onPositionChanged: (mouse) => {
            const dx = mapToItem(view, mouse.x, 0).x - fromX
            view.moveLine(line, view.LayoutMirroring.enabled ? -dx : dx, parentFrom, currentFrom)
        }
        onDoubleClicked: view.shareWidth(1 / 8, 3 / 8)
    }
    LineGrip { line: 0 }
    LineGrip { line: 1 }
    // Files dropped in a column land in the folder it shows. It lies over
    // the whole column, under its items, rather than in the list's content,
    // which ends at its last item.
    component ColumnDrop: FolderDrop { parent: items; anchors.fill: parent; z: -1 }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        acceptedButtons: Qt.NoButton
        // Fixed, nothing scrolls sideways.
        interactive: !view.fixed
        // Room to scroll the hidden columns out of sight even when the rest
        // would fit.
        contentWidth: view.fixed ? width : Math.max(width, row.width, view.hiddenWidth + width)
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 1800
        maximumFlickVelocity: 5000
        KineticScroll { target: flick; horizontal: true }
        onContentXChanged: Qt.callLater(view.loadColumns)
        onContentWidthChanged: view.revealEnd()
        onWidthChanged: view.revealEnd()
        ScrollBar.horizontal: FolioScrollBar { visible: !view.fixed }

        Row {
            id: row
            // A Row isn't placed by its Flickable, so it's moved to the right
            // edge by hand when mirrored.
            x: LayoutMirroring.enabled ? parent.width - width : 0
            height: flick.height

            // Fixed, at the top of the trail (~, /), the parent's column is
            // left empty so the others stay where they are.
            Item {
                visible: view.fixed && trail.count === 0
                width: view.parentWidth + 1
                height: row.height
            }

            // The folders on the way here, each showing which way the trail goes.
            // Fixed, only the last of them: the parent.
            Repeater {
                id: ancestors
                model: trail
                onItemAdded: Qt.callLater(view.loadColumns)
                Row {
                    id: ancestor
                    required property string path
                    required property string next
                    required property int index
                    property bool shown: false
                    readonly property bool parentColumn: view.fixed && index === trail.count - 1
                    visible: !view.fixed || parentColumn
                    height: row.height

                    FileListView {
                        id: column
                        topMargin: view.topRoom
                        width: view.fixed ? view.parentWidth : view.columnWidth
                        height: parent.height
                        model: folderLoader.item
                        Loader {
                            id: folderLoader
                            visible: false
                            active: ancestor.shown || ancestor.parentColumn
                            onLoaded: Qt.callLater(column.revealNext)
                            sourceComponent: FolderModel {
                                path: ancestor.path
                                showHidden: backend.folder.showHidden
                                sortMode: { backend.sortsRevision; return backend.sortOf(ancestor.path) }
                                filter: backend.folder.filter
                                onLoaded: column.revealNext()
                            }
                        }
                        // Names sort by the language's alphabet.
                        Connections {
                            target: translator
                            function onChanged() { if (column.model) column.model.reload() }
                        }
                        function revealNext() { if (model) positionViewAtIndex(Math.max(0, model.indexOf(ancestor.next)), ListView.Contain) }
                        ColumnDrop { items: column; folder: ancestor.path }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: (point) => {
                                const index = column.indexAt(point.position.x, point.position.y)
                                view.contextMenu(index >= 0 ? column.model.get(index) : null, ancestor.path, point.scenePosition)
                            }
                        }
                        Connections {
                            target: ancestor
                            function onNextChanged() { column.revealNext() }
                        }
                        delegate: FileRow {
                            width: column.width
                            hoverLit: !view.pointerStill
                            chevron: true
                            trail: name === ancestor.next
                            onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                            onActivated: backend.open(path)
                        }
                    }
                    Separator { }
                }
            }

            // The folder being shown: its selection is the window's.
            FileListView {
                id: here
                function letGo() { view.letGo() }
                topMargin: view.topRoom
                width: view.fixed ? view.currentWidth : view.columnWidth
                height: row.height
                focus: true
                keyNavigationEnabled: true
                // No cursor of its own to start with: the pane puts it back
                // where the last view had it, and one on the first item would
                // select that instead.
                currentIndex: -1
                highlightFollowsCurrentItem: false
                model: backend.folder

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => view.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
                }
                Band {
                    view: here
                    rectOf: (i) => Qt.rect(0, i * view.rowHeight, here.width, view.rowHeight)
                    rangeFor: (area) => [
                        Math.max(0, Math.floor(area.y / view.rowHeight)),
                        Math.min(here.count - 1, Math.floor((area.y + area.height) / view.rowHeight))
                    ]
                }
                ColumnDrop { items: here; folder: backend.path }

                onCurrentIndexChanged: {
                    selecting.cursorMoved()
                    if (view.fixed)
                        view.pointerStill = true
                }
                Keys.onPressed: (event) => selecting.page(event) || selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)
                // Into a folder is the way the columns grow: leftward, when
                // mirrored.
                Keys.onRightPressed: view.LayoutMirroring.enabled ? backend.goUp() : view.enter()
                Keys.onLeftPressed: view.LayoutMirroring.enabled ? view.enter() : backend.goUp()

                delegate: FileRow {
                    width: here.width
                    hoverLit: !view.pointerStill
                    chevron: true
                    current: selected
                    onClicked: (modifiers) => { selecting.click(index, modifiers); here.forceActiveFocus() }
                    onActivated: backend.open(path)
                }

                // Where the first item would be, lined up with the names.
                FolioText {
                    readonly property int nameInset: 14 + Math.round(20 * theme.textScale) + 12
                    x: LayoutMirroring.enabled ? here.width - width - nameInset : nameInset
                    y: 8
                    readonly property bool searching: backend.folder.loading && backend.folder.query.length > 0
                    visible: here.count === 0 && (!backend.folder.loading || searching) && backend.folder.error.length === 0
                    text: searching ? qsTr("Searching…") : backend.folder.query ? qsTr("No matches") : qsTr("No items")
                    color: theme.secondaryForeground
                }
            }
            Separator { }

            // A look inside the selected folder; clicking in it goes there.
            FileListView {
                id: inside
                topMargin: view.topRoom
                width: view.fixed ? view.previewWidth : view.columnWidth
                height: row.height
                visible: view.selected.isDir === true
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => {
                        const index = inside.indexAt(point.position.x, point.position.y)
                        view.contextMenu(index >= 0 ? inside.model.get(index) : null, view.selected.path, point.scenePosition)
                    }
                }
                model: FolderModel {
                    id: insideFolder
                    path: view.selected.isDir === true ? view.selected.path : ""
                    showHidden: backend.folder.showHidden
                    sortMode: { backend.sortsRevision; return backend.sortOf(insideFolder.path) }
                    filter: backend.folder.filter
                }
                Connections {
                    target: translator
                    function onChanged() { insideFolder.reload() }
                }
                ColumnDrop { items: inside; folder: view.selected.path ?? "" }
                delegate: FileRow {
                    width: inside.width
                    hoverLit: !view.pointerStill
                    chevron: true
                    onClicked: backend.showItem(path)
                    onActivated: backend.open(path)
                }

                FolioText {
                    readonly property int nameInset: 14 + Math.round(20 * theme.textScale) + 12
                    x: LayoutMirroring.enabled ? inside.width - width - nameInset : nameInset
                    y: 8
                    visible: inside.count === 0 && !inside.model.loading
                    text: qsTr("No items")
                    color: theme.secondaryForeground
                }
            }

            // The selected file's details, where a folder would show what's
            // inside, filling the rest of the width. Loaded by URL so
            // PreviewPane isn't compiled until a file is selected.
            Loader {
                id: detailsColumn
                active: !!view.detailsPane && view.selected.path !== undefined && view.selected.isDir !== true
                visible: active
                source: "PreviewPane.qml"
                onLoaded: {
                    // All the room right of the columns in sight, and no
                    // narrower than it reads well.
                    // Fixed, the third column's width.
                    item.width = Qt.binding(() => view.fixed ? view.previewWidth
                                                  : Math.max(300, flick.width - (trail.count - view.hidden + 1) * (view.columnWidth + 1)))
                    item.height = Qt.binding(() => row.height)
                    item.folderPane = Qt.binding(() => view.detailsPane)
                }
            }
            Connections {
                target: detailsColumn.item
                function onFinished() { view.focusView() }
            }
        }
    }
}
