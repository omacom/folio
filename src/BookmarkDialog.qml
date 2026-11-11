import QtQuick
import QtQuick.Layouts

// Adding or editing a bookmark (its name, its path, the folder it's in), and
// making or renaming a folder of bookmarks. The name is the bookmark's own;
// the path is typed as in the path bar, Tab completing a folder name.
// Enter saves, Escape leaves everything as it was.
FolioDialog {
    id: dialog

    // The pane's Backend, whose folder relative paths start from.
    property var completer
    // "" (closed), "add", "edit", "newFolder" or "renameFolder".
    property string mode: ""
    property string itemId
    property string parentId
    // Where a new bookmark or folder goes in its folder; -1 is the end.
    property int index: -1
    property string problem
    readonly property bool forBookmark: mode === "add" || mode === "edit"

    title: mode === "add" ? qsTr("Add Bookmark") : mode === "edit" ? qsTr("Edit Bookmark")
         : mode === "newFolder" ? qsTr("New Folder") : qsTr("Rename Folder")
    visible: mode !== ""
    onScrimClicked: close()

    // The folder a relative path is taken from: the pane's when the dialog
    // opened, for Tab completion and saving alike (the pane can move on
    // while the dialog is open).
    property string base
    // What it showed when it opened, to write only what the user changed.
    property string openedName
    property string openedPath
    property string openedParent
    function start(newMode, name, path, id, parent) {
        problem = ""
        base = completer ? completer.path : ""
        mode = newMode
        itemId = id
        parentId = parent
        openedName = name
        openedPath = path
        openedParent = parent
        nameField.text = name
        pathField.text = path
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function add(path, parent, at) {
        start("add", bookmarks.defaultName(path), bookmarks.normalize(path), "", parent)
        index = at === undefined ? -1 : at
    }
    function newFolder(parent, at) {
        start("newFolder", "", "", "", parent)
        index = at === undefined ? -1 : at
    }
    function edit(id) {
        const item = bookmarks.get(id)
        if (item.id === undefined)
            return
        start(item.type === "folder" ? "renameFolder" : "edit", item.name, item.path || "", id, item.parentId)
    }
    function close() { mode = "" }

    function submit() {
        const name = nameField.text.trim()
        // The path as shown is exact (it may end in a space, a folder's own
        // name); only a path the user typed is cleaned up.
        const pathTyped = pathField.text !== openedPath
        const path = pathTyped ? bookmarks.absolute(pathField.text.trim(), base) : openedPath
        if (forBookmark && path.length === 0) {
            problem = qsTr("A bookmark needs a path.")
            return
        }
        // Name, path and folder change together, or not at all.
        const saveProblem = () => bookmarks.problem.length > 0 ? bookmarks.problem : qsTr("That couldn't be saved.")
        let failure = ""
        if (mode === "add")
            failure = bookmarks.add(parentId, name, path, index) !== "" ? "" : saveProblem()
        else if (mode === "newFolder")
            failure = bookmarks.addFolder(parentId, name, index) !== "" ? "" : saveProblem()
        else {
            // Untouched fields are left out, so another window's change to
            // them meanwhile stands.
            const newName = name !== openedName ? name : ""
            const newPath = pathTyped ? path : ""
            failure = parentId !== openedParent ? bookmarks.edit(itemId, newName, newPath, parentId)
                                                : bookmarks.edit(itemId, newName, newPath)
        }
        if (failure.length > 0) {
            problem = failure
            return
        }
        close()
    }

    Keys.onEscapePressed: dialog.close()

    Text { text: qsTr("Name"); color: theme.secondaryForeground; font.pixelSize: 12 }
    PlainField {
        id: nameField
        Layout.fillWidth: true
        Layout.topMargin: -10
        placeholderText: dialog.forBookmark ? bookmarks.defaultName(pathField.text) : qsTr("New Folder")
        onAccepted: dialog.submit()
    }
    Text { visible: dialog.forBookmark; text: qsTr("Path"); color: theme.secondaryForeground; font.pixelSize: 12 }
    PlainField {
        id: pathField
        visible: dialog.forBookmark
        Layout.fillWidth: true
        Layout.topMargin: -10
        placeholderText: qsTr("~/Documents, /mnt/backup, a file…")
        onAccepted: dialog.submit()
        // Tab completes a folder name; with nothing to complete it moves on,
        // as Tab does everywhere else.
        Keys.onTabPressed: (event) => {
            const completed = bookmarks.completePath(text, dialog.base)
            if (completed === text) {
                event.accepted = false
                return
            }
            text = completed
            cursorPosition = text.length
        }
    }
    Text { text: qsTr("Folder"); color: theme.secondaryForeground; font.pixelSize: 12 }
    FolderPicker {
        Layout.fillWidth: true
        Layout.topMargin: -10
        folderId: dialog.parentId
        exclude: dialog.mode === "renameFolder" ? dialog.itemId : ""
        // Somewhere else than beside the bookmark it came from: the end.
        onChosen: (id) => { dialog.parentId = id; dialog.index = -1; dialog.problem = "" }
        onFailed: (message) => dialog.problem = message
    }
    Text {
        Layout.fillWidth: true
        visible: dialog.problem.length > 0
        text: dialog.problem
        color: theme.red
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10
        PlainButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        PlainButton {
            text: dialog.mode === "add" ? qsTr("Add") : dialog.mode === "newFolder" ? qsTr("Create") : qsTr("Save")
            primary: true
            onClicked: dialog.submit()
        }
    }
}
