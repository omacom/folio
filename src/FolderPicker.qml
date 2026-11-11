import QtQuick
import QtQuick.Controls

// Where a bookmark goes, as a browser's folder chooser: the folder's name in
// a field-like box that drops down every folder of bookmarks, indented by
// depth, and New Folder… to make one there and then (named in place, Enter
// makes it, Escape doesn't). `exclude` (a folder being moved) and what's
// inside it can't be chosen. Choosing says chosen(id); folderId stays bound
// to whoever owns it.
Item {
    id: picker

    property string folderId
    property string exclude
    signal chosen(string id)
    // A new folder couldn't be made; the typed name stays to try again.
    signal failed(string message)
    readonly property bool naming: nameField.visible
    readonly property string folderName: {
        for (const f of bookmarks.folders)
            if (f.id === picker.folderId)
                return f.name
        // Deleted meanwhile (another window): said, not passed off as the bar.
        return picker.folderId.length > 0 ? qsTr("A folder that's gone") : qsTr("Bookmarks Bar")
    }

    implicitHeight: 38
    implicitWidth: 200

    function startNaming() {
        // A name typed before (when making it failed) is kept.
        nameField.visible = true
        nameField.forceActiveFocus()
    }
    function stopNaming() {
        nameField.visible = false
        button.forceActiveFocus()
    }

    AbstractButton {
        id: button
        anchors.fill: parent
        visible: !picker.naming
        hoverEnabled: true
        onClicked: menu.popup(button, 0, button.height + 4)

        background: Rectangle {
            color: theme.darkBackground
            border.width: 1
            border.color: button.activeFocus || menu.visible ? theme.accent : Qt.alpha(theme.foreground, button.hovered ? 0.30 : 0.18)
            radius: Math.min(4, theme.radius)
        }
        contentItem: Item {
            Glyph {
                id: icon
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                name: "folder"
                size: 14
                color: theme.accent
            }
            Text {
                anchors.left: icon.right
                anchors.leftMargin: 8
                anchors.right: arrow.left
                anchors.verticalCenter: parent.verticalCenter
                text: picker.folderName
                color: theme.brightForeground
                elide: Text.ElideRight
            }
            Glyph {
                id: arrow
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                name: "forward"
                rotation: 90
                size: 12
                color: theme.secondaryForeground
            }
        }
    }

    // A new folder's name, inside the folder chosen now.
    PlainField {
        id: nameField
        anchors.fill: parent
        visible: false
        placeholderText: qsTr("New folder in %1").arg(picker.folderName)
        onAccepted: {
            const id = bookmarks.addFolder(picker.folderId, text)
            if (id.length === 0) {
                picker.failed(bookmarks.problem.length > 0 ? bookmarks.problem
                              : picker.folderId.length > 0 && bookmarks.get(picker.folderId).id === undefined
                              ? qsTr("That folder is gone; choose another.") : qsTr("That couldn't be saved."))
                return
            }
            text = ""
            picker.stopNaming()
            picker.chosen(id)
        }
        Keys.onEscapePressed: (event) => {
            event.accepted = true
            picker.stopNaming()
        }
        // Left for the folder menu (to choose another place): the name stays.
        onActiveFocusChanged: if (!activeFocus && visible && !menu.visible) visible = false
    }

    FolioMenu {
        id: menu
        Instantiator {
            model: menu.visible || picker.visible ? bookmarks.folders : []
            delegate: FolioMenuItem {
                required property var modelData
                enabled: picker.exclude.length === 0 || !bookmarks.isWithin(modelData.id, picker.exclude)
                id: row
                text: "    ".repeat(modelData.depth) + modelData.name
                checkable: true
                checked: modelData.id === picker.folderId
                onTriggered: picker.chosen(modelData.id)
                // Long names end in an ellipsis, short of the check, and show
                // in full on hover.
                contentItem: Text {
                    text: row.text
                    elide: Text.ElideRight
                    rightPadding: 24
                    color: !row.enabled ? theme.secondaryForeground : row.highlighted ? theme.brightForeground : theme.foreground
                    verticalAlignment: Text.AlignVCenter
                }
                ToolTip.visible: row.hovered && row.contentItem.truncated
                ToolTip.delay: 800
                ToolTip.text: modelData.name
            }
            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }
        MenuLine { }
        FolioMenuItem {
            text: qsTr("New Folder…")
            onTriggered: picker.startNaming()
        }
    }
}
