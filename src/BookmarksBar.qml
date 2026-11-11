import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The bookmarks bar, as in a browser: one row across the window under the
// path. A bookmark opens its path in the pane used last (a folder is gone
// to, a file opened); middle- or Ctrl+click opens a folder in a new window.
// A folder of bookmarks drops down, its folders as submenus; while one is
// open, pointing at another folder switches to it, as a menu bar does.
// Bookmarks drag to reorder, into folders, and out of drop-downs; files and
// folders dragged in from a pane become bookmarks where they're dropped.
// What doesn't fit goes behind ». F6 puts the keyboard on the bar.
Rectangle {
    id: bar

    // The pane's Backend and FileOps bookmarks act on.
    property var target
    property var targetFileOps
    // While a dialog or the bubble is up, tooltips stay out of its way.
    property bool quiet: false
    // `index` -1 is the end of the folder; beside a bookmark it's after it.
    signal addRequested(string path, string parentId, int index)
    signal editRequested(string id)
    signal newFolderRequested(string parentId, int index)
    signal deleteRequested(string id)
    signal hideRequested()
    // The keyboard leaves the bar (Escape, Tab, F6, or a bookmark opened).
    signal done()

    implicitHeight: 36
    color: theme.background
    // The chips, for the tests to find by name.
    readonly property alias chipRow: row

    // Goes where a bookmark points, as if its path were typed in the path
    // bar: folders open here, files in their app, and a path that's gone
    // says so in the pane.
    function openItem(item, newWindow) {
        if (!target)
            return
        // The exact path, never re-read as typed text (a name may end in a
        // space).
        const path = bookmarks.resolve(item.path)
        if (item.kind === "missing")
            target.showMessage(qsTr("%1 doesn't exist.").arg(item.path))
        else if (newWindow && item.kind === "folder")
            target.newWindowRequested(path)
        else if (item.kind === "file")
            target.open(path)
        else
            target.navigate(path)
    }
    // The file selected in its folder, by its exact path (never re-read as
    // typed text: a name may end in a space).
    function revealItem(item) { if (target) target.showItem(bookmarks.resolve(item.path)) }
    // After opening something with the keys on the bar, they go to the files,
    // as when a bookmark is opened with ⏎.
    function leaveIfFocused() { if (activeFocus) done() }
    function openAndLeave(item, newWindow) {
        openItem(item, newWindow)
        leaveIfFocused()
    }

    // ---- Drop-downs ----------------------------------------------------

    // The drop-down open now, as {menu, source}: `source` is a folder's id,
    // or "»" for what doesn't fit. It follows changes made while it's open
    // (a bookmark deleted from it stays open, as in a browser), and closes
    // when its folder goes.
    property var openDropDown: null
    // Which drop-down closed last and when: the press on its own chip closes
    // it before the click arrives, and that click mustn't open it again.
    property var lastClosed: ({source: "", at: 0})
    // A bookmark is being dragged out of a drop-down.
    property bool menuDragging: false

    function dropDown(source, anchor, fromKeys) {
        const contents = contentsOf(source)
        if (!contents)
            return
        const menu = menuComponent.createObject(bar)
        fill(menu, contents, source === "»" ? "" : source, source === "»" ? fitting : 0)
        menu.closed.connect(() => {
            if (openDropDown && openDropDown.menu === menu) {
                lastClosed = {source: openDropDown.source, at: Date.now()}
                openDropDown = null
            }
            menu.destroy()
        })
        // Whether the keys were on the bar: they leave it once something's
        // opened, however it's opened.
        openDropDown = {menu: menu, source: source, fromKeys: !!fromKeys || activeFocus}
        // Hung from the bar, not the chip: chips are made anew on every
        // change, which would take an open drop-down with them.
        const at = anchor.mapToItem(bar, 0, anchor.height + 4)
        menu.popup(bar, at.x, at.y)
        if (fromKeys)
            menu.currentIndex = 0
    }
    // A click on a folder's chip: opens it, or closes it when it was open.
    function toggleDropDown(source, anchor) {
        if (lastClosed.source === source && Date.now() - lastClosed.at < 300)
            return
        dropDown(source, anchor, false)
    }
    function contentsOf(source) {
        if (source === "»")
            return bookmarks.items.slice(fitting)
        const folder = bookmarks.get(source)
        return folder.children !== undefined ? folder.children : null
    }
    function closeDropDown() { if (openDropDown) openDropDown.menu.dismiss() }
    // Runs `action` once the drop-down is gone: a closing menu hands the
    // keys back to where they were, which would take them from a dialog the
    // action opens.
    function afterDropDown(action) {
        if (!openDropDown) {
            action()
            return
        }
        const menu = openDropDown.menu
        menu.closed.connect(() => Qt.callLater(action))
        menu.dismiss()
    }
    Connections {
        target: bookmarks
        // The chips follow at once; an open drop-down later, as the change
        // may come from a drop whose dragged entry is in this very menu.
        function onChanged() {
            bar.syncChips()
            Qt.callLater(bar.rebuildDropDown)
        }
    }
    // The chips, one per bookmark on the bar, kept across changes by id:
    // remade on every change, a chip under a click or a drag would go with
    // it (a rename saved by the bubble as the pointer presses a chip).
    ListModel { id: chipModel }
    property var itemsById: ({})
    function syncChips() {
        const items = bookmarks.items
        const byId = {}
        for (const item of items)
            byId[item.id] = item
        itemsById = byId
        for (let i = chipModel.count - 1; i >= 0; --i)
            if (!byId[chipModel.get(i).itemId])
                chipModel.remove(i)
        for (let i = 0; i < items.length; ++i) {
            if (i < chipModel.count && chipModel.get(i).itemId === items[i].id)
                continue
            let found = -1
            for (let j = i + 1; j < chipModel.count; ++j)
                if (chipModel.get(j).itemId === items[i].id) {
                    found = j
                    break
                }
            if (found >= 0)
                chipModel.move(found, i, 1)
            else
                chipModel.insert(i, {itemId: items[i].id})
        }
    }
    Component.onCompleted: syncChips()
    function rebuildDropDown() {
        const open = openDropDown
        if (!open || menuDragging)
            return
        const contents = contentsOf(open.source)
        if (!contents) {
            open.menu.dismiss()
            return
        }
        while (open.menu.count > 0) {
            const entry = open.menu.itemAt(0)
            const sub = entry.subMenu
            open.menu.removeItem(entry)
            if (sub)
                sub.destroy()
        }
        fill(open.menu, contents, open.source === "»" ? "" : open.source, open.source === "»" ? fitting : 0)
    }
    // `parentId` and `offset` say where the entries are, for drops on them:
    // the folder they're in, and the index of the first.
    function fill(menu, children, parentId, offset) {
        // Entries are made inside the menu's list (an Item), which addItem
        // then takes over; a Menu itself isn't in the scene.
        if (children.length === 0)
            menu.addItem(entryComponent.createObject(menu.contentItem, {text: qsTr("(empty)"), enabled: false, parentId: parentId}))
        for (let i = 0; i < children.length; ++i) {
            const child = children[i]
            if (child.type === "folder") {
                const sub = menuComponent.createObject(menu, {title: child.name, folder: child, parentId: parentId, position: offset + i})
                fill(sub, child.children, child.id, 0)
                menu.addMenu(sub)
            } else {
                menu.addItem(entryComponent.createObject(menu.contentItem, {item: child, text: child.name, parentId: parentId, position: offset + i}))
            }
        }
    }

    // ---- Context menu ----------------------------------------------------

    // At the pointer, given in `anchor`'s coordinates.
    function showContext(item, anchor, x, y) {
        const at = anchor.mapToItem(bar, x, y)
        contextMenu.item = item
        contextMenu.x = at.x
        contextMenu.y = at.y
        contextMenu.open()
    }

    // ---- Fitting -----------------------------------------------------------

    // How many chips fit, counted from the chips' own widths so hiding the
    // ones that don't never moves the ones that do. Room for » is kept only
    // when not everything fits.
    readonly property int fitting: {
        const widths = []
        for (let i = 0; i < chips.count; ++i) {
            const chip = chips.itemAt(i)
            if (!chip)
                return i
            widths.push(chip.implicitWidth)
        }
        const fits = (room) => {
            let used = 0
            for (let i = 0; i < widths.length; ++i) {
                used += widths[i] + (i > 0 ? row.spacing : 0)
                if (used > room)
                    return i
            }
            return widths.length
        }
        const all = fits(strip.width)
        return all === widths.length ? all : fits(strip.width - more.implicitWidth - row.spacing)
    }
    // An open » holds what didn't fit when it opened: a resize closes it
    // rather than leave chips that went behind it out of reach.
    onFittingChanged: if (openDropDown && openDropDown.source === "»") openDropDown.menu.dismiss()

    // ---- Dropping ---------------------------------------------------------

    // Accepted only when it took, so the source knows a refused drop.
    function dropAt(drop, parentId, index) {
        let ok = false
        if (drop.source && drop.source.bookmarkId !== undefined)
            ok = bookmarks.move(drop.source.bookmarkId, parentId, index)
        else if (drop.hasUrls)
            ok = bookmarks.addUrls(parentId, drop.urls, index) > 0
        if (ok)
            drop.accept(Qt.LinkAction)
        else
            drop.accepted = false
    }
    // The bookmark being dragged, if one is: a folder can't be dropped into
    // itself or anything inside it, so those don't light up.
    function draggedId(drag) { return drag.source && drag.source.bookmarkId !== undefined ? drag.source.bookmarkId : "" }
    function canGoInto(drag, folderId) {
        const id = draggedId(drag)
        return id.length === 0 || !bookmarks.isWithin(folderId, id)
    }

    // Empty space: drops land after the last chip shown; right-click offers
    // new things.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: (mouseEvent) => bar.showContext(null, bar, mouseEvent.x, mouseEvent.y)
    }
    DropArea {
        id: endDrop
        anchors.fill: parent
        keys: ["folio-bookmark", "text/uri-list"]
        onDropped: (drop) => bar.dropAt(drop, "", bar.fitting)
    }

    // ---- Keyboard ------------------------------------------------------------

    // F6: the keyboard on the bar. ←/→ move, ⏎/↓/Space open (a folder drops
    // down), Ctrl+⏎ opens in a new window, the menu key or Shift+F10 shows
    // the right-click menu, Escape/Tab/F6 go back to the files.
    property int keyIndex: 0
    readonly property int keyCount: fitting + (fitting < chips.count ? 1 : 0)
    function focusBar() {
        if (keyCount === 0)
            return false
        keyIndex = Math.min(keyIndex, keyCount - 1)
        forceActiveFocus()
        return true
    }
    function keyChip() { return keyIndex < fitting ? chips.itemAt(keyIndex) : more.visible ? more : null }
    // A change under the keyboard (a bookmark deleted, the window narrowed)
    // keeps it on a chip that's there; with none left it goes back.
    onKeyCountChanged: {
        if (keyIndex >= keyCount)
            keyIndex = Math.max(0, keyCount - 1)
        if (keyCount === 0 && activeFocus)
            done()
    }
    // The menu key or Shift+F10 on a drop-down entry (the keys are on the
    // entry itself there): its right-click menu.
    function isMenuKey(event) {
        return event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))
    }
    function activateKeyChip(newWindow) {
        const chip = keyChip()
        if (!chip)
            return
        if (!chip.item) {
            dropDown("»", chip, true)
        } else if (chip.isFolder) {
            dropDown(chip.item.id, chip, true)
        } else {
            openItem(chip.item, newWindow)
            done()
        }
    }
    Keys.onPressed: (event) => {
        const plain = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
        if (event.key === Qt.Key_Right && plain) {
            keyIndex = (keyIndex + 1) % keyCount
        } else if (event.key === Qt.Key_Left && plain) {
            keyIndex = (keyIndex + keyCount - 1) % keyCount
        } else if (event.key === Qt.Key_Home) {
            keyIndex = 0
        } else if (event.key === Qt.Key_End) {
            keyIndex = keyCount - 1
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Down
                   || event.key === Qt.Key_Space) {
            activateKeyChip(!!(event.modifiers & Qt.ControlModifier))
        } else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            const chip = keyChip()
            if (chip && chip.item)
                showContext(chip.item, chip, chip.width / 2, chip.height)
        } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
                   || event.key === Qt.Key_F6) {
            done()
        } else {
            return
        }
        event.accepted = true
    }

    // ---- The row ---------------------------------------------------------------

    Item {
        id: strip
        anchors.left: parent.left
        anchors.right: problemText.visible ? problemText.left : parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        Row {
            id: row
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Repeater {
                id: chips
                model: chipModel
                delegate: Chip {
                    required property string itemId
                    required property int index
                    item: bar.itemsById[itemId] || null
                    position: index
                    visible: index < bar.fitting
                    keyed: bar.activeFocus && bar.keyIndex === index
                }
            }

            // Everything that doesn't fit, as one menu.
            Chip {
                id: more
                visible: bar.fitting < chips.count
                label: "»"
                tip: qsTr("More bookmarks")
                keyed: bar.activeFocus && bar.keyIndex === bar.fitting
            }
        }

        // Where a drop on empty space goes: after the last chip shown.
        Rectangle {
            visible: endDrop.containsDrag && chips.count > 0
            x: bar.fitting > 0 && chips.itemAt(bar.fitting - 1)
               ? chips.itemAt(bar.fitting - 1).x + chips.itemAt(bar.fitting - 1).width + 1 : 0
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 28
            color: theme.accent
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: chips.count === 0
            width: parent.width
            elide: Text.ElideRight
            text: endDrop.containsDrag ? qsTr("Drop to bookmark")
                : bookmarks.problem.length > 0 ? "" : qsTr("Bookmark this folder with Ctrl+B, or drag folders and files here")
            color: theme.secondaryForeground
            font.pixelSize: 12
        }
    }

    // A file that can't be read or saved is said on the bar, full or empty.
    Text {
        id: problemText
        visible: bookmarks.problem.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, chips.count > 0 ? bar.width * 0.4 : bar.width - 24)
        elide: Text.ElideMiddle
        text: bookmarks.problem
        color: theme.red
        font.pixelSize: 12
        HoverHandler { id: problemHover }
        ToolTip.visible: problemHover.hovered && problemText.truncated
        ToolTip.text: bookmarks.problem
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }

    // What's dragged out of a drop-down: a copy under the pointer, over the
    // menus, which stay open until it's dropped.
    Rectangle {
        id: menuGhost
        parent: Overlay.overlay
        z: 1000
        visible: Drag.active
        property string bookmarkId
        property alias label: menuGhostText.text
        width: menuGhostText.implicitWidth + 24
        height: 28
        radius: Math.min(4, theme.radius)
        color: Qt.alpha(theme.background, 0.92)
        border.width: 1
        border.color: theme.accent
        Drag.source: menuGhost
        Drag.keys: ["folio-bookmark"]
        Drag.hotSpot.x: 12
        Drag.hotSpot.y: height / 2
        Text {
            id: menuGhostText
            anchors.centerIn: parent
            color: theme.brightForeground
            font.pixelSize: 13
        }
    }
    // A drag handler on a drop-down entry steers the ghost; the drop goes to
    // whatever is under it. Dropped outside a drop-down, the drop-down
    // closes, as a browser's does.
    function menuDragMoved(handler, id, label) {
        if (!menuGhost.Drag.active) {
            menuGhost.bookmarkId = id
            menuGhost.label = label
            menuDragging = true
        }
        const p = menuGhost.parent.mapFromItem(null, handler.centroid.scenePosition.x, handler.centroid.scenePosition.y)
        menuGhost.x = p.x - menuGhost.Drag.hotSpot.x
        menuGhost.y = p.y - menuGhost.Drag.hotSpot.y
        menuGhost.Drag.active = true
    }
    function menuDragEnded() {
        if (!menuGhost.Drag.active)
            return
        const target = menuGhost.Drag.target
        const intoMenu = !!target && !!target.inMenu
        menuGhost.Drag.drop()
        menuGhost.Drag.active = false
        menuDragging = false
        if (intoMenu)
            Qt.callLater(rebuildDropDown)
        else
            Qt.callLater(closeDropDown)
    }

    // ---- Chips -------------------------------------------------------------

    // A bookmark or folder on the bar. `item` is from bookmarks.items; the
    // » chip has none.
    component Chip: Item {
        id: chip

        property var item: null
        property int position: -1
        property bool keyed: false
        property string label: item ? item.name : ""
        // Name and path, as a browser shows a bookmark's title and URL; a
        // folder's full name when it's cut short.
        property string tip: !item ? "" : item.type === "bookmark" ? item.name + "\n" + item.path
                                         : labelText.truncated ? item.name : ""
        readonly property bool isFolder: !!item && item.type === "folder"
        readonly property bool missing: !!item && item.kind === "missing"
        readonly property string source: !item ? "»" : isFolder ? item.id : ""
        readonly property bool open: !!bar.openDropDown && source.length > 0 && bar.openDropDown.source === source
        // For drops: what's being dragged.
        readonly property string bookmarkId: item ? item.id : ""

        implicitWidth: content.implicitWidth + 20
        implicitHeight: 28
        // While dragged, the chip's copy rides over the chips after it.
        z: mouse.drag.active ? 10 : 0

        // Where a drag over this chip would land: before it, after it, or
        // (on a folder's middle) inside it.
        property string dropZone: ""
        function zoneAt(x, intoAllowed) {
            if (isFolder && intoAllowed && x > width * 0.25 && x < width * 0.75)
                return "into"
            return x < width / 2 ? "before" : "after"
        }

        Rectangle {
            anchors.fill: parent
            radius: Math.min(4, theme.radius)
            color: chip.dropZone === "into" ? Qt.alpha(theme.accent, 0.20)
                 : chip.open || mouse.pressed ? Qt.alpha(theme.foreground, 0.12)
                 : mouse.containsMouse || chip.keyed ? Qt.alpha(theme.foreground, 0.08)
                 : "transparent"
            border.width: chip.dropZone === "into" || chip.keyed ? 1 : 0
            border.color: theme.accent
        }
        // The insertion mark for a drop before or after.
        Rectangle {
            visible: chip.dropZone === "before" || chip.dropZone === "after"
            x: chip.dropZone === "before" ? -2 : chip.width
            width: 2
            height: chip.height
            color: theme.accent
        }

        RowLayout {
            id: content
            opacity: mouse.drag.active ? 0.35 : 1
            anchors.centerIn: parent
            spacing: 6
            Glyph {
                visible: !!chip.item
                name: chip.isFolder ? "folder" : chip.item && chip.item.kind === "file" ? "file" : "bookmark"
                size: 14
                color: chip.missing ? theme.secondaryForeground : chip.isFolder ? theme.accent : theme.foreground
            }
            Text {
                id: labelText
                text: chip.label
                Layout.maximumWidth: 180
                elide: Text.ElideRight
                color: chip.missing ? theme.secondaryForeground : theme.foreground
                font.pixelSize: 13
            }
        }

        ToolTip {
            visible: mouse.containsMouse && !mouse.pressed && chip.tip.length > 0 && !bar.quiet && !bar.openDropDown
                     && !contextMenu.visible
            delay: 600
            text: chip.missing ? qsTr("%1 (not there right now)").arg(chip.tip) : chip.tip
            enter: null
            exit: null
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            drag.target: chip.item ? ghost : null
            drag.threshold: 8
            // The copy sits under the pointer from the start, rather than
            // catching up from where the drag began.
            drag.smoothed: false
            // While a drop-down is open, pointing at another folder (or »)
            // switches to it, as in a menu bar.
            onContainsMouseChanged: {
                if (containsMouse && chip.source.length > 0 && bar.openDropDown && !bar.menuDragging
                        && bar.openDropDown.source !== chip.source)
                    bar.dropDown(chip.source, chip, false)
            }
            onClicked: (mouseEvent) => {
                if (mouseEvent.button === Qt.RightButton) {
                    if (chip.item)
                        bar.showContext(chip.item, chip, mouseEvent.x, mouseEvent.y)
                } else if (chip.source.length > 0) {
                    if (mouseEvent.button === Qt.LeftButton)
                        bar.toggleDropDown(chip.source, chip)
                } else {
                    bar.openAndLeave(chip.item, mouseEvent.button === Qt.MiddleButton
                                                || (mouseEvent.modifiers & Qt.ControlModifier))
                }
            }
            // The drop lands where the pointer is, wherever on the chip it
            // took hold.
            onPressed: (mouseEvent) => {
                ghost.Drag.hotSpot.x = mouseEvent.x
                ghost.Drag.hotSpot.y = mouseEvent.y
            }
            onReleased: {
                if (ghost.Drag.active)
                    ghost.Drag.drop()
                ghost.x = 0
                ghost.y = 0
            }
        }
        // What's dragged: a copy under the pointer, while the chip stays
        // put, faded, until it's dropped.
        Rectangle {
            id: ghost
            width: chip.width
            height: chip.height
            visible: mouse.drag.active
            radius: Math.min(4, theme.radius)
            color: Qt.alpha(theme.background, 0.92)
            border.width: 1
            border.color: theme.accent
            readonly property string bookmarkId: chip.bookmarkId
            Drag.active: mouse.drag.active
            Drag.source: ghost
            Drag.keys: ["folio-bookmark"]
            Text {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 12)
                text: chip.label
                elide: Text.ElideRight
                color: theme.brightForeground
                font.pixelSize: 13
            }
        }

        DropArea {
            anchors.fill: parent
            enabled: !!chip.item
            keys: ["folio-bookmark", "text/uri-list"]
            onEntered: (drag) => {
                // A chip dropped on itself goes nowhere.
                if (drag.source === ghost) {
                    drag.accepted = false
                    return
                }
                chip.dropZone = chip.zoneAt(drag.x, bar.canGoInto(drag, chip.bookmarkId))
            }
            onPositionChanged: (drag) => chip.dropZone = chip.zoneAt(drag.x, bar.canGoInto(drag, chip.bookmarkId))
            onExited: chip.dropZone = ""
            onDropped: (drop) => {
                const zone = chip.dropZone
                chip.dropZone = ""
                if (zone === "into")
                    bar.dropAt(drop, chip.item.id, -1)
                else
                    bar.dropAt(drop, "", chip.position + (zone === "after" ? 1 : 0))
            }
        }
    }

    // ---- Drop-down entries -----------------------------------------------------

    // Drop-downs show what each entry is, as a browser's favicons do. A
    // folder inside one has its own right-click menu and drags like a
    // bookmark.
    Component {
        id: menuComponent
        FolioMenu {
            id: folderMenu
            // A little wider than the file menus, as a browser's bookmark
            // menus are; longer names end in an ellipsis.
            width: 300
            // The folder this submenu shows, and where its row is.
            property var folder: null
            property string parentId
            property int position: -1
            delegate: FolioMenuItem {
                id: sub
                readonly property var folder: sub.subMenu ? sub.subMenu.folder : null
                Keys.onPressed: (event) => {
                    if (sub.folder && bar.isMenuKey(event)) {
                        event.accepted = true
                        bar.showContext(sub.folder, sub, sub.width / 2, sub.height)
                    }
                }
                contentItem: EntryLabel { id: subLabel; text: sub.text; glyph: "folder"; tint: theme.accent; highlighted: sub.highlighted; reserve: 20 }
                ToolTip.visible: sub.hovered && subLabel.truncated && !bar.menuDragging && !contextMenu.visible
                ToolTip.delay: 800
                ToolTip.text: sub.text
                MenuDrop {
                    anchors.fill: parent
                    entry: sub
                    folderId: sub.folder ? sub.folder.id : ""
                    parentId: sub.subMenu ? sub.subMenu.parentId : ""
                    slot: sub.subMenu ? sub.subMenu.position : -1
                }
                // On press, not on release: the submenu opening on hover
                // can take the pointer before a release arrives.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    enabled: !!sub.folder
                    onPressed: (mouseEvent) => {
                        // No row current, so the hover timer has no submenu
                        // to open over the menu shown now.
                        sub.menu.currentIndex = -1
                        const at = sub.mapToItem(bar, mouseEvent.x, mouseEvent.y)
                        bar.showContext(sub.folder, bar, at.x, at.y)
                    }
                }
                DragHandler {
                    id: subDrag
                    target: null
                    enabled: !!sub.folder
                    onCentroidChanged: if (active) bar.menuDragMoved(subDrag, sub.folder.id, sub.folder.name)
                    onActiveChanged: if (!active) bar.menuDragEnded()
                }
            }
        }
    }
    component EntryLabel: Item {
        property alias text: label.text
        property alias glyph: icon.name
        property color tint: theme.foreground
        property bool highlighted: false
        property bool dim: false
        // Room kept at the right end (a submenu's arrow).
        property int reserve: 0
        readonly property bool truncated: label.truncated
        implicitWidth: label.implicitWidth + 22 + 40
        implicitHeight: label.implicitHeight
        Glyph {
            id: icon
            // The empty folder's "(empty)" has no glyph.
            visible: name.length > 0
            anchors.verticalCenter: parent.verticalCenter
            size: 14
            color: parent.dim ? theme.secondaryForeground : parent.tint
        }
        Text {
            id: label
            x: icon.visible ? 22 : 0
            anchors.verticalCenter: parent.verticalCenter
            // Within the row, short of a submenu's arrow, so a long name ends
            // in an ellipsis (and `truncated` says so, for the tooltip).
            width: Math.max(0, Math.min(implicitWidth, parent.width - x - parent.reserve))
            elide: Text.ElideRight
            color: parent.dim ? theme.secondaryForeground : parent.highlighted ? theme.brightForeground : theme.foreground
        }
    }
    // Drops on a drop-down entry: before or after it, or into a folder's
    // row (its middle).
    component MenuDrop: DropArea {
        id: drop
        property var entry
        property string folderId
        property string parentId
        // The entry's index in its folder (-1: the "(empty)" entry). Not
        // `position`, whose change signal would hide DropArea's own.
        property int slot: -1
        readonly property bool inMenu: true
        property string zone: ""
        keys: ["folio-bookmark", "text/uri-list"]
        function zoneAt(drag) {
            if (slot < 0)
                return "into"
            if (folderId.length > 0 && bar.canGoInto(drag, folderId) && drag.y > height * 0.25 && drag.y < height * 0.75)
                return "into"
            return drag.y < height / 2 ? "before" : "after"
        }
        onEntered: (drag) => {
            // Not on itself, and not anywhere inside a folder being dragged.
            const dragged = bar.draggedId(drag)
            if (dragged.length > 0 && (dragged === (entry.item ? entry.item.id : folderId)
                                       || (parentId.length > 0 && bookmarks.isWithin(parentId, dragged)))) {
                drag.accepted = false
                return
            }
            zone = zoneAt(drag)
        }
        onPositionChanged: (drag) => zone = zoneAt(drag)
        onExited: zone = ""
        onDropped: (event) => {
            const z = zone
            zone = ""
            if (z === "into" && folderId.length > 0)
                bar.dropAt(event, folderId, -1)
            else if (slot < 0)
                bar.dropAt(event, parentId, -1)
            else
                bar.dropAt(event, parentId, slot + (z === "after" ? 1 : 0))
        }
        Rectangle {
            visible: drop.zone === "before" || drop.zone === "after"
            x: 8
            y: drop.zone === "before" ? 0 : drop.height - 2
            width: drop.width - 16
            height: 2
            color: theme.accent
        }
        Rectangle {
            visible: drop.zone === "into"
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            radius: Math.min(4, theme.radius)
            color: Qt.alpha(theme.accent, 0.20)
            border.width: 1
            border.color: theme.accent
        }
    }
    // A bookmark in a drop-down: right-click for its own menu over the
    // drop-down, which stays open; middle-click opens a new window and
    // leaves it open too, as in a browser. It drags out like a chip.
    Component {
        id: entryComponent
        FolioMenuItem {
            id: entry
            property var item: null
            property string parentId
            property int position: -1
            Keys.onPressed: (event) => {
                if (entry.item && bar.isMenuKey(event)) {
                    event.accepted = true
                    bar.showContext(entry.item, entry, entry.width / 2, entry.height)
                }
            }
            // Opened with the keys on the bar (F6), they go back to the files
            // after.
            onTriggered: {
                if (!item)
                    return
                const fromKeys = !!bar.openDropDown && bar.openDropDown.fromKeys
                bar.openItem(item, false)
                if (fromKeys)
                    Qt.callLater(bar.done)
            }
            contentItem: EntryLabel {
                text: entry.text
                glyph: !entry.item ? "" : entry.item.kind === "file" ? "file" : "bookmark"
                dim: !entry.item || entry.item.kind === "missing"
                highlighted: entry.highlighted
            }
            ToolTip.visible: entry.hovered && !!entry.item && !bar.menuDragging && !contextMenu.visible
            ToolTip.delay: 800
            ToolTip.text: !entry.item ? "" : entry.item.kind === "missing"
                ? qsTr("%1 (not there right now)").arg(entry.item.name + "\n" + entry.item.path)
                : entry.item.name + "\n" + entry.item.path
            MenuDrop {
                anchors.fill: parent
                entry: entry
                parentId: entry.parentId
                slot: entry.position
            }
            TapHandler {
                acceptedButtons: Qt.RightButton | Qt.MiddleButton
                onTapped: (point, button) => {
                    if (!entry.item)
                        return
                    const at = entry.mapToItem(bar, point.position.x, point.position.y)
                    if (button === Qt.RightButton)
                        bar.showContext(entry.item, bar, at.x, at.y)
                    else
                        bar.openItem(entry.item, true)
                }
            }
            DragHandler {
                id: entryDrag
                target: null
                enabled: !!entry.item
                onCentroidChanged: if (active) bar.menuDragMoved(entryDrag, entry.item.id, entry.item.name)
                onActiveChanged: if (!active) bar.menuDragEnded()
            }
        }
    }

    // ---- Right-click menu ------------------------------------------------------

    // The right-click menu of a bookmark, a folder or the bar. A Popup made
    // to look and key like a Menu: Qt closes every other Menu when one
    // opens, and this one opens over a folder's drop-down, which stays open
    // beneath it as in a browser.
    Popup {
        id: contextMenu
        // The bookmark or folder right-clicked, or null for the bar itself.
        property var item: null
        readonly property bool forBookmark: !!item && item.type === "bookmark"
        readonly property bool forFolder: !!item && item.type === "folder"
        // Where Add Bookmark and New Folder put things: into a folder, beside
        // a bookmark, or on the bar.
        readonly property string addInto: forFolder ? item.id : forBookmark ? (bookmarks.get(item.id).parentId || "") : ""
        // Beside a bookmark: right after it; otherwise at the end.
        readonly property int addAt: {
            if (!forBookmark)
                return -1
            const siblings = addInto.length > 0 ? bookmarks.get(addInto).children : bookmarks.items
            const at = siblings ? siblings.findIndex(c => c.id === item.id) : -1
            return at < 0 ? -1 : at + 1
        }
        // The entry the arrow keys are on, or null.
        property var current: null

        function entries() {
            const out = []
            for (const child of column.children)
                if (child.isEntry && child.shown)
                    out.push(child)
            return out
        }
        function step(by) {
            const list = entries()
            if (list.length === 0)
                return
            const at = list.indexOf(current)
            current = at < 0 ? (by > 0 ? list[0] : list[list.length - 1])
                             : list[(at + by + list.length) % list.length]
        }

        z: 10
        width: 280
        margins: 8
        topPadding: 4
        bottomPadding: 4
        leftPadding: 0
        rightPadding: 0
        enter: null
        exit: null
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: { current = null; column.forceActiveFocus() }
        // The entry clicked, run after the close (see Entry).
        property var chosenEntry: null
        onClosed: {
            const entry = chosenEntry
            chosenEntry = null
            if (entry)
                Qt.callLater(() => entry.chosen())
        }

        background: Rectangle {
            color: theme.background
            border.width: 1
            border.color: Qt.alpha(theme.foreground, 0.16)
            radius: Math.min(6, theme.radius)
        }

        contentItem: Column {
            id: column
            focus: true
            Keys.onDownPressed: contextMenu.step(1)
            Keys.onUpPressed: contextMenu.step(-1)
            Keys.onReturnPressed: if (contextMenu.current) contextMenu.current.click()
            Keys.onEnterPressed: if (contextMenu.current) contextMenu.current.click()

            Entry {
                shown: contextMenu.forBookmark
                text: qsTr("Open")
                onChosen: { const item = contextMenu.item; bar.afterDropDown(() => bar.openAndLeave(item, false)) }
            }
            Entry {
                shown: contextMenu.forBookmark && contextMenu.item.kind === "folder"
                text: qsTr("Open in New Window")
                onChosen: { const item = contextMenu.item; bar.afterDropDown(() => bar.openAndLeave(item, true)) }
            }
            Entry {
                shown: contextMenu.forBookmark && contextMenu.item.kind === "file"
                text: qsTr("Show in Folder")
                onChosen: { const item = contextMenu.item; bar.afterDropDown(() => { bar.revealItem(item); bar.leaveIfFocused() }) }
            }
            Entry {
                shown: contextMenu.forBookmark
                text: qsTr("Copy Path")
                onChosen: if (bar.targetFileOps) bar.targetFileOps.yank([bookmarks.resolve(contextMenu.item.path)])
            }
            MenuLine { shown: contextMenu.forBookmark }
            Entry {
                text: qsTr("Add Bookmark…")
                onChosen: { const into = contextMenu.addInto, at = contextMenu.addAt; bar.afterDropDown(() => bar.addRequested(bar.target ? bar.target.path : "", into, at)) }
            }
            Entry {
                text: qsTr("New Folder…")
                onChosen: { const into = contextMenu.addInto, at = contextMenu.addAt; bar.afterDropDown(() => bar.newFolderRequested(into, at)) }
            }
            MenuLine { shown: !!contextMenu.item }
            Entry {
                shown: !!contextMenu.item
                text: contextMenu.forFolder ? qsTr("Rename…") : qsTr("Edit…")
                onChosen: { const id = contextMenu.item.id; bar.afterDropDown(() => bar.editRequested(id)) }
            }
            Entry {
                shown: !!contextMenu.item
                text: qsTr("Delete")
                // A drop-down stays open and loses the entry; a folder that asks
                // first closes it, so the question isn't under it.
                onChosen: {
                    const id = contextMenu.item.id
                    if (contextMenu.forFolder && contextMenu.item.children.length > 0)
                        bar.afterDropDown(() => bar.deleteRequested(id))
                    else
                        bar.deleteRequested(id)
                }
            }
            MenuLine { shown: !contextMenu.item }
            Entry {
                shown: !contextMenu.item
                text: qsTr("Hide Bookmarks Bar")
                keys: "Ctrl+Shift+B"
                onChosen: bar.hideRequested()
            }
        }
    }

    // A context menu entry as a Menu's, highlighted by the pointer or the keys.
    // What it does is `chosen`, run once the menu has closed: closing hands
    // the keys back to where they were, which would take them from a dialog
    // the entry opened.
    component Entry: FolioMenuItem {
        id: entry
        readonly property bool isEntry: true
        signal chosen()
        width: parent ? parent.width : implicitWidth
        focusPolicy: Qt.NoFocus
        highlighted: hovered || contextMenu.current === entry
        onHoveredChanged: if (hovered) contextMenu.current = entry
        onClicked: {
            contextMenu.chosenEntry = entry
            contextMenu.close()
        }
    }
}
