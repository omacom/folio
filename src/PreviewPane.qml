import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// A larger look at the selected item and the facts about it, with the one
// action that matters: open it.
Rectangle {
    id: pane

    // Scrolls the details so `item`, which the keyboard just reached, is in
    // sight.
    function reveal(item) {
        const top = item.mapToItem(facts, 0, 0).y
        scroller.contentY = Math.max(Math.min(scroller.contentY, top), top + item.height - scroller.height)
    }

    // The pane whose selected item this shows, and whose backend and file
    // work the button acts through: in a split, the one used last.
    property var folderPane: null
    // Hidden, it shows nothing, so moving through files doesn't read them,
    // look for their trash notes or make large thumbnails for nobody.
    readonly property var item: visible && folderPane ? folderPane.current : ({})
    readonly property bool hasItem: item !== undefined && item.path !== undefined
    // An item in the trash shows where it came from and when it was
    // deleted, and offers Restore rather than Open. Its note is read in the
    // background; until it's in, the item looks trashed, as nearly all there
    // are, but Restore waits. What's inside a trashed folder has no note.
    readonly property bool noting: hasItem && folderPane.inTrash && info.origin === undefined
    readonly property string origin: info.origin || ""
    readonly property bool trashed: hasItem && origin.length > 0
    // A plugin's item shows its subtitle and its own details, and has no
    // date.
    readonly property bool pluginItem: hasItem && item.kind === "item"
    // The drive whose top folder is selected or shown, for its free space.
    readonly property var drive: {
        const shown = folderPane ? folderPane.paneBackend.path : ""
        for (const d of places.devices)
            if (d.free >= 0 && ((hasItem && d.path === item.path) || d.path === shown))
                return d
        return null
    }
    // Tab reaches the details' controls after the panes, while there are
    // any (an app to pick for the file, a plugin's buttons), one after the
    // other; Escape hands the keyboard back to the files.
    function controls() {
        const list = visible && opensWith.visible ? [opensWith] : []
        for (let i = 0; visible && i < sections.count; ++i) {
            const buttons = sections.itemAt(i).buttons
            for (let j = 0; j < buttons.count; ++j)
                list.push(buttons.itemAt(j))
        }
        return list
    }
    function takesKeyboard() { return controls().length > 0 }
    function hasKeyboard() { return controls().some(control => control.activeFocus) }
    // The first control, or going back, the last.
    function focusKeyboard(step) {
        const list = controls()
        if (list.length > 0)
            list[step < 0 ? list.length - 1 : 0].forceActiveFocus(Qt.TabFocusReason)
    }
    // The next control, or false past either end, where the keyboard leaves.
    function moveKeyboard(step) {
        const list = controls()
        const next = list.findIndex(control => control.activeFocus) + step
        if (next < 0 || next >= list.length)
            return false
        list[next].forceActiveFocus(Qt.TabFocusReason)
        return true
    }
    signal finished()
    function homeless(path) { return path.startsWith(places.home + "/") ? "~" + path.slice(places.home.length) : path }
    // A picture's size or a text's opening lines once they've been read off
    // the UI thread.
    readonly property var info: backend.details
    // Described once the folder's loaded has put the cursor back on its
    // item: a re-read leaves another item on the cursor's row for a moment,
    // and describing that would drop what's been read of this one.
    onItemChanged: Qt.callLater(describe)
    function describe() { backend.describe(item, !!folderPane && folderPane.inTrash) }

    color: theme.darkBackground

    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Qt.alpha(theme.foreground, 0.12) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 22
        visible: pane.hasItem

        // Everything but the Open button scrolls when the window is too
        // short for it: a file in a repository, say, adds a Git section.
        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: facts.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FolioScrollBar { }

            ColumnLayout {
                id: facts
                width: scroller.width
                spacing: 22

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Image {
                        id: big
                        anchors.fill: parent
                        asynchronous: true
                        fillMode: Image.PreserveAspectFit
                        source: pane.hasItem && !pane.item.isDir && pane.item.kind !== "text" && !pane.pluginItem ? Format.thumb(pane.item.path, pane.item.mime, 512, pane.item.modified) : ""
                        visible: status === Image.Ready
                    }

                    // Text files show their opening lines instead of a picture.
                    Rectangle {
                        anchors.fill: parent
                        visible: pane.hasItem && pane.item.kind === "text"
                        color: theme.background
                        border.width: 1
                        border.color: Qt.alpha(theme.foreground, 0.12)
                        clip: true
                        Text {
                            anchors.fill: parent
                            anchors.margins: 14
                            text: pane.info.text || ""
                            color: theme.foreground
                            font.pixelSize: 10
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                    }

                    Glyph {
                        anchors.centerIn: parent
                        visible: !big.visible && pane.hasItem && pane.item.kind !== "text"
                        name: pane.pluginItem ? pane.item.icon : pane.hasItem && pane.item.isDir ? "folder" : "file"
                        size: 72
                        stroke: 1.2
                        color: pane.hasItem && pane.item.isDir ? theme.accent : theme.secondaryForeground
                    }
                }

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true
                    // Aligned explicitly, so that they're mirrored: left as they
                    // are, a Latin name would stay on the left.
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignLeft
                        text: pane.item.name || ""
                        color: theme.brightForeground
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        wrapMode: Text.WrapAnywhere
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignLeft
                        text: {
                            translator.language // The type's name comes from the system, in the language of the moment.
                            return Format.summary(pane.item, backend.typeName(pane.item.mime || ""))
                        }
                        color: theme.secondaryForeground
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    rowSpacing: 12
                    columnSpacing: 16

                    Text { visible: pane.trashed; text: qsTr("Original location"); color: theme.secondaryForeground; font.pixelSize: 12 }
                    Text {
                        visible: pane.trashed
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: pane.trashed ? pane.homeless(pane.origin.replace(/\/[^\/]*$/, "") || "/") : ""
                        color: theme.foreground
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                    Text { visible: !pane.pluginItem; text: pane.trashed || pane.noting ? qsTr("Deleted") : qsTr("Modified"); color: theme.secondaryForeground; font.pixelSize: 12 }
                    Text {
                        visible: !pane.pluginItem
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: {
                            translator.language // Re-read when the language (and so the default locale) changes.
                            const when = pane.trashed ? pane.info.deleted : pane.noting ? null : pane.item.modified
                            return pane.hasItem && when ? when.toLocaleString(Qt.locale(), Locale.ShortFormat) : ""
                        }
                        color: theme.foreground
                        font.pixelSize: 12
                    }
                    Text { visible: !!pane.drive; text: qsTr("Free space"); color: theme.secondaryForeground; font.pixelSize: 12 }
                    Text {
                        visible: !!pane.drive
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: {
                            translator.language // Re-read when the language (and so its digits) changes.
                            return pane.drive ? Format.size(pane.drive.free) : ""
                        }
                        color: theme.foreground
                        font.pixelSize: 12
                    }
                    // What the file opens with, picked from the apps that can open
                    // its type, which makes that app the type's default.
                    Text { visible: opensWith.visible; text: qsTr("Opens with"); color: theme.secondaryForeground; font.pixelSize: 12 }
                    PlainButton {
                        id: opensWith
                        onActiveFocusChanged: if (activeFocus) pane.reveal(this)
                        // Read again after picking one, which changes the default.
                        property int picked: 0
                        readonly property var apps: {
                            picked
                            return pane.hasItem && !pane.item.isDir && !pane.trashed && !pane.pluginItem && pane.folderPane
                                ? pane.folderPane.paneFileOps.appsFor(pane.item.path) : []
                        }
                        readonly property var current: apps.find(app => app.isDefault)
                        visible: apps.length > 1
                        Layout.alignment: Qt.AlignRight
                        implicitHeight: 28
                        text: (current ? current.name : qsTr("Choose")) + "  ▾"
                        onClicked: appsMenu.popup(opensWith, 0, opensWith.height + 4)
                        Keys.onEscapePressed: pane.finished()
                        // Gone with the keyboard on it (another item selected, or the
                        // details hidden), it hands the keyboard back.
                        onVisibleChanged: if (!visible && activeFocus) pane.finished()
                        FolioMenu {
                            id: appsMenu
                            Repeater {
                                model: opensWith.apps
                                FolioMenuItem {
                                    required property var modelData
                                    text: modelData.name
                                    keys: modelData.isDefault ? "✓" : ""
                                    onTriggered: {
                                        pane.folderPane.paneFileOps.setDefaultApp(pane.item.path, modelData.id)
                                        opensWith.picked++
                                    }
                                }
                            }
                        }
                    }
                    Text { visible: !!pane.info.width; text: qsTr("Dimensions"); color: theme.secondaryForeground; font.pixelSize: 12 }
                    Text {
                        visible: !!pane.info.width
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: pane.info.width ? qsTr("%1 × %2").arg(pane.info.width.toLocaleString(Qt.locale(), "f", 0))
                                                             .arg(pane.info.height.toLocaleString(Qt.locale(), "f", 0)) : ""
                        color: theme.foreground
                        font.pixelSize: 12
                    }
                }

                // What plugins say, after Folio's own rows: a plugin's item's
                // details, or each plugin's about a file or folder, under its title
                // when it gives one, with buttons for what it offers there. A row
                // can have a tone (a Git status's color) and a quieter second line.
                Repeater {
                    id: sections
                    model: pane.pluginItem ? [{ rows: pane.item.details || [], actions: [] }] : pane.info.plugins || []
                    ColumnLayout {
                        id: section
                        required property var modelData
                        property alias buttons: buttons
                        Layout.fillWidth: true
                        spacing: 12
                        Text {
                            visible: !!section.modelData.title
                            Layout.topMargin: 6
                            text: section.modelData.title || ""
                            color: theme.brightForeground
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: section.modelData.rows
                            RowLayout {
                                id: row
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 16
                                Text {
                                    Layout.alignment: Qt.AlignTop
                                    text: row.modelData.label
                                    color: theme.secondaryForeground
                                    font.pixelSize: 12
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                        text: row.modelData.value
                                        color: (row.modelData.tone && theme.tones[row.modelData.tone]) || theme.foreground
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        visible: !!row.modelData.detail
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                        text: row.modelData.detail
                                        color: theme.secondaryForeground
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            visible: buttons.count > 0
                            spacing: 8
                            Repeater {
                                id: buttons
                                model: section.modelData.actions
                                PlainButton {
                                    required property var modelData
                                    implicitHeight: 28
                                    text: modelData.label
                                    onClicked: pane.folderPane.paneBackend.perform(modelData.plugin, modelData.id, [pane.item.path])
                                    onActiveFocusChanged: if (activeFocus) pane.reveal(this)
                                    Keys.onEscapePressed: pane.finished()
                                    // Gone with the keyboard on it (another item
                                    // selected), it hands the keyboard back.
                                    Component.onDestruction: if (activeFocus) pane.finished()
                                }
                            }
                        }
                    }
                }
            }
        }

        AbstractButton {
            id: openButton
            Layout.fillWidth: true
            implicitHeight: 40
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            enabled: !pane.noting
            // Names the app the file opens in: "Open with Plain", read again
            // when Opens with picks another.
            readonly property string app: {
                opensWith.picked
                return pane.hasItem && !pane.item.isDir && pane.folderPane ? pane.folderPane.paneFileOps.defaultAppName(pane.item.mime || "") : ""
            }
            text: pane.trashed || pane.noting ? qsTr("Restore") : pane.hasItem && pane.item.isDir ? qsTr("Open folder")
                //: %1 is the name of the app the file opens in, like "Plain".
                : app ? qsTr("Open with %1").arg(app) : qsTr("Open")
            Accessible.name: text
            onClicked: pane.trashed ? pane.folderPane.paneFileOps.restore([pane.item.path]) : pane.folderPane.paneBackend.open(pane.item.path)
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: openButton.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            }
            contentItem: RowLayout {
                // A long app name ends in … rather than running past the button.
                Text { Layout.leftMargin: 14; Layout.fillWidth: true; text: openButton.text; elide: Text.ElideRight; color: theme.accentForeground; font.weight: Font.DemiBold }
                Text { Layout.rightMargin: 14; text: "⏎"; color: theme.accentForeground }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: !pane.hasItem
        text: qsTr("Nothing selected")
        color: theme.secondaryForeground
    }
}
