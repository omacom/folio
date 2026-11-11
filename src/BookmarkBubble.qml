import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The star's bubble, as in a browser: Ctrl+B or the star in the path bar
// bookmarks the folder right away and opens this under the star to name it
// and choose its folder; on a folder that's bookmarked already, it edits
// that bookmark. Done, Enter, Escape or a click outside keep it (with any
// changes); Remove takes it off again. Only what was changed here is
// written, so another window's change meanwhile isn't undone; a change that
// can't be saved keeps the bubble (or brings it back) with the draft and
// says why.
Popup {
    id: bubble

    // The folder the last bookmark went in; a new one goes there too.
    property string lastFolder
    signal folderUsed(string id)
    property string itemId
    property bool added: false
    property string parentId
    property bool removed: false
    // What it showed when it opened, to tell what the user changed.
    property string openedName
    property string openedParent
    // Kept already by Done, so closing doesn't write it again.
    property bool committed: false
    property string problem
    // Closed with a draft that couldn't be saved, about to come back.
    property bool reopening: false

    // `anchor` is the star it hangs from; without one it hangs from the
    // window's top right.
    function openFor(path, anchor) {
        // A draft kept after a failed save stays: the star again (or Ctrl+B)
        // on the same bookmark comes back to it, and another bookmark waits
        // until it's saved.
        if ((visible || reopening) && itemId.length > 0 && problem.length > 0) {
            if (bookmarks.find(path) !== itemId) {
                const failure = commit()
                if (failure.length > 0) {
                    problem = failure
                    if (!visible) open()
                    nameField.forceActiveFocus()
                    return true
                }
            } else {
                if (!visible) open()
                nameField.forceActiveFocus()
                return true
            }
        }
        let id = bookmarks.find(path)
        added = id.length === 0
        if (added) {
            const into = lastFolder.length > 0 && bookmarks.get(lastFolder).type === "folder" ? lastFolder : ""
            id = bookmarks.add(into, "", path)
        }
        if (id.length === 0) {
            // Nothing to edit: Main says why, where it's seen even with the
            // bar hidden.
            return false
        }
        const item = bookmarks.get(id)
        itemId = id
        parentId = item.parentId
        openedParent = item.parentId
        openedName = item.name
        removed = false
        committed = false
        problem = ""
        nameField.text = item.name
        const host = parent
        if (anchor && anchor.visible) {
            const at = anchor.mapToItem(host, anchor.width, anchor.height)
            x = Math.max(8, Math.min(at.x - width, host.width - width - 8))
            y = at.y + 6
        } else {
            x = host.width - width - 16
            y = 64
        }
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
        return true
    }
    // Writes what was changed here; empty, or why it couldn't be.
    function commit() {
        const nameChanged = nameField.text.trim().length > 0 && nameField.text.trim() !== openedName
        const parentChanged = parentId !== openedParent
        if (!nameChanged && !parentChanged)
            return ""
        // Deleted meanwhile (another window): there's nothing left to change,
        // as in a browser, whose bubble just goes.
        if (bookmarks.problem.length === 0 && bookmarks.get(itemId).id === undefined)
            return ""
        // Untouched fields are left out, so they keep what's on disk now.
        const failure = parentChanged
            ? bookmarks.edit(itemId, nameChanged ? nameField.text : "", "", parentId)
            : bookmarks.edit(itemId, nameChanged ? nameField.text : "", "")
        if (failure.length === 0 && parentChanged)
            folderUsed(parentId)
        return failure
    }
    function done() {
        const failure = commit()
        if (failure.length > 0) {
            problem = failure
            return
        }
        committed = true
        close()
    }
    function removeIt() {
        const goneAlready = bookmarks.problem.length === 0 && bookmarks.get(itemId).id === undefined
        if (!goneAlready && !bookmarks.remove(itemId)) {
            problem = bookmarks.problem.length > 0 ? bookmarks.problem : qsTr("That couldn't be removed.")
            return
        }
        removed = true
        close()
    }
    // However it closes, what was typed is kept, as a browser's is; when it
    // can't be, the bubble comes back with the draft.
    onClosed: {
        if (removed || committed)
            return
        const failure = commit()
        if (failure.length > 0) {
            problem = failure
            reopening = true
            Qt.callLater(() => { reopening = false; bubble.open(); nameField.forceActiveFocus() })
        }
    }

    width: 340
    padding: 20
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: null
    exit: null

    background: Rectangle {
        color: theme.background
        border.width: 1
        border.color: theme.accent
        radius: Math.min(6, theme.radius)
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            text: bubble.added ? qsTr("Bookmark added") : qsTr("Edit bookmark")
            color: theme.brightForeground
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 10
            Layout.fillWidth: true

            Text { text: qsTr("Name"); color: theme.secondaryForeground; font.pixelSize: 12 }
            PlainField {
                id: nameField
                Layout.fillWidth: true
                onAccepted: bubble.done()
            }
            Text { text: qsTr("Folder"); color: theme.secondaryForeground; font.pixelSize: 12 }
            FolderPicker {
                id: picker
                Layout.fillWidth: true
                folderId: bubble.parentId
                onChosen: (id) => { bubble.parentId = id; bubble.problem = "" }
                onFailed: (message) => bubble.problem = message
            }
        }
        Text {
            Layout.fillWidth: true
            visible: bubble.problem.length > 0
            text: bubble.problem
            color: theme.red
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 4
            spacing: 10
            PlainButton {
                text: qsTr("Remove")
                onClicked: bubble.removeIt()
            }
            PlainButton {
                text: qsTr("Done")
                primary: true
                onClicked: bubble.done()
            }
        }
    }

}
