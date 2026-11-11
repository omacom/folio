import QtQuick
import QtQuick.Layouts

// Renaming (F2): the name with everything but its extension selected, so
// typing replaces the name and keeps the type. Enter renames, Escape leaves
// it alone; a clash with an existing name is said here, never overwritten.
// Tab renames and goes on to the next item, Shift+Tab to the one before.
FolioDialog {
    id: dialog

    property var item: null
    // The list the item is a row of, whose order Tab follows: the open folder's, or in the columns view another column's.
    property var rows: null
    property string problem: ""

    title: qsTr("Rename")
    visible: item !== null
    onScrimClicked: close()

    function open(target, list) {
        problem = ""
        item = target
        rows = list || backend.folder
        // From the path: in search results the name is the path from where
        // the search started ("sub/x.txt").
        const name = target.path.slice(target.path.lastIndexOf("/") + 1)
        field.text = name
        const dot = fileOps.extensionStart(name, target.isDir)
        field.forceActiveFocus()
        field.select(0, dot > 0 ? dot : name.length)
    }
    function close() { item = null }
    // With `step` (1 or -1), opens on the item that far along in the folder's order, or closes past its end.
    function submit(step) {
        // A column closed since, as when the folder renamed was on the way to the open one, leaves no list to go along.
        const row = step && rows ? rows.indexOfPath(item.path) : -1
        const next = row >= 0 ? rows.get(row + step) : ({})
        const from = item.path
        problem = fileOps.rename(from, field.text)
        if (problem.length > 0)
            return
        // A search lists a folder and what's in it: the next item's path, kept from before, moves with the folder renamed.
        if (next.path && next.path.startsWith(from + "/"))
            next.path = from.slice(0, from.lastIndexOf("/") + 1) + field.text + next.path.slice(from.length)
        if (next.path)
            open(next, rows)
        else
            close()
    }

    // On the dialog, not the field, so Escape still cancels after Tab has
    // moved the keyboard to a button.
    Keys.onEscapePressed: dialog.close()

    PlainField {
        id: field
        Layout.fillWidth: true
        onAccepted: dialog.submit()
        // Taken here before the dialog's own Tab, which goes round its buttons.
        Keys.onShortcutOverride: (event) => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
        Keys.onTabPressed: dialog.submit(1)
        Keys.onBacktabPressed: dialog.submit(-1)
    }
    // Aligned explicitly, so that it's mirrored: a problem that starts with
    // a file name would otherwise follow the name's script.
    Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
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
            text: qsTr("Rename")
            primary: true
            onClicked: dialog.submit()
        }
    }
}
